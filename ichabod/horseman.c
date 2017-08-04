//
//  horseman.c
//  ichabod
//
//  Created by Charley Robinson on 6/1/17.
//

#include <stdlib.h>
#include <zmq.h>
#include <uv.h>
#include <assert.h>
#include "horseman.h"
#include "frame_generator.h"
#include "audio_mixer.h"
#include "growing_file_audio_source.h"
#include "audio_frame_converter.h"
#include "video_frame_buffer.h"

struct horseman_s {
  void* zmq_ctx;
  void* screencast_socket;
  void* blobsink_socket;
  char is_interrupted;
  uv_thread_t zmq_thread;
  
  uv_mutex_t lock;
  int64_t quiet_cycles;

  void (*on_video_msg)(struct horseman_s* queue,
                       struct horseman_msg_s* msg, void* p);
  void (*on_audio_msg)(struct horseman_s* queue,
                       struct horseman_msg_s* msg, void* p);
  void* callback_p;

  // Separate runloop for dispatching callbacks.
  uv_loop_t* loop;
  uv_thread_t loop_thread;
  char is_running;
};

static void horseman_loop_main(void* p) {
  struct horseman_s* pthis = (struct horseman_s*)p;
  int ret = 0;
  while (pthis->is_running && 0 == ret) {
    ret = uv_run(pthis->loop, UV_RUN_DEFAULT);
  }
  printf("horseman: exiting worker loop\n");
}

void reset_quiet_counter(struct horseman_s* pthis) {
  uv_mutex_lock(&pthis->lock);
  pthis->quiet_cycles = 0;
  uv_mutex_unlock(&pthis->lock);
}

void increment_quiet_counter(struct horseman_s* pthis) {
  uv_mutex_lock(&pthis->lock);
  pthis->quiet_cycles++;
  uv_mutex_unlock(&pthis->lock);
}

static void horseman_msg_free(struct horseman_msg_s* msg) {
  if (msg->sz_data) {
    free(msg->sz_data);
  }
  if (msg->sz_sid) {
    free(msg->sz_sid);
  }
  free(msg);
}

static int receive_message(void* socket, struct horseman_msg_s* msg,
                           char* got_message)
{
  int ret;
  if (msg->sz_data) {
    free(msg->sz_data);
  }
  if (msg->sz_sid) {
    free(msg->sz_sid);
  }
  memset(msg, 0, sizeof(struct horseman_msg_s));
  while (1) {
    zmq_msg_t message;
    ret = zmq_msg_init (&message);
    ret = zmq_msg_recv (&message, socket, 0);
    if (ret < 0 && EAGAIN == errno) {
      *got_message = 0;
      zmq_msg_close(&message);
      return 0;
    }
    *got_message = 1;
    // Process the message frame
    uint8_t* sz_msg = (uint8_t*)malloc(zmq_msg_size(&message) + 1);
    memcpy(sz_msg, zmq_msg_data(&message), zmq_msg_size(&message));
    sz_msg[zmq_msg_size(&message)] = '\0';

    if (!msg->sz_data) {
      msg->sz_data = (char*)sz_msg;
    } else if (!msg->timestamp) {
      msg->timestamp = atof((char*)sz_msg);
      free(sz_msg);
    } else if (!msg->sz_sid) {
      msg->sz_sid = (char*)sz_msg;
    } else {
      printf("unknown extra message part received. freeing.");
      free(sz_msg);
    }

    zmq_msg_close (&message);
    if (!zmq_msg_more (&message))
      break;      //  Last message frame
  }
  return 0;
}

struct msg_dispatch_s {
  uv_work_t work;
  struct horseman_msg_s* msg;
  struct horseman_s* horseman;
};

static void dispatch_video_msg(uv_work_t* work) {
  struct msg_dispatch_s* async_msg = (struct msg_dispatch_s*) work->data;
  struct horseman_s* pthis = async_msg->horseman;
  struct horseman_msg_s* msg = async_msg->msg;
  pthis->on_video_msg(pthis, msg, pthis->callback_p);
}

static void after_video_msg(uv_work_t* work, int status) {
  struct msg_dispatch_s* msg = (struct msg_dispatch_s*) work->data;
  horseman_msg_free(msg->msg);
  free(msg);
}

static int receive_screencast(struct horseman_s* pthis, char* got_message) {
  struct horseman_msg_s* msg = calloc(1, sizeof(struct horseman_msg_s));
  // wait for zmq message
  int ret = receive_message(pthis->screencast_socket, msg, got_message);
  // process message
  if (ret) {
    printf("trouble? %d %d\n", ret, errno);
  } else if (*got_message) {
    printf("received screencast  ts=%f\n",
           msg->timestamp);

    struct msg_dispatch_s* async_msg = (struct msg_dispatch_s*)
    calloc(1, sizeof(struct msg_dispatch_s));
    async_msg->msg = msg;
    async_msg->horseman = pthis;
    async_msg->work.data = async_msg;
    ret = uv_queue_work(pthis->loop, &async_msg->work,
                        dispatch_video_msg, after_video_msg);
  }
  return ret;
}

static int receive_blobsink(struct horseman_s* pthis, char* got_message) {
  struct horseman_msg_s* msg = calloc(1, sizeof(struct horseman_msg_s));
  int ret = receive_message(pthis->blobsink_socket, msg, got_message);
  if (ret) {
    printf("receive_blobsink: %d %d\n", ret, errno);
  } else if (*got_message) {
    printf("received blob len=%ld ts=%f sid=%s\n",
           strlen(msg->sz_data), msg->timestamp, msg->sz_sid);
    pthis->on_audio_msg(pthis, msg, pthis->callback_p);
  }
  horseman_msg_free(msg);
  return ret;
}

static void horseman_zmq_main(void* p) {
  int ret;
  printf("media queue is online %p\n", p);
  struct horseman_s* pthis = (struct horseman_s*)p;
  int t = 10;
  ret = zmq_connect(pthis->screencast_socket, "ipc:///tmp/ichabod-screencast");
  ret = zmq_connect(pthis->blobsink_socket, "ipc:///tmp/ichabod-blobsink");
  if (ret) {
    printf("failed to connect to media queue socket. errno %d\n", errno);
    return;
  }
  ret = zmq_setsockopt(pthis->screencast_socket, ZMQ_RCVTIMEO, &t, sizeof(int));
  ret = zmq_setsockopt(pthis->blobsink_socket, ZMQ_RCVTIMEO, &t, sizeof(int));
  while (!pthis->is_interrupted) {
    char got_screencast = 0;
    ret = receive_screencast(pthis, &got_screencast);
    char got_blobsink = 0;
    ret = receive_blobsink(pthis, &got_blobsink);
    if (got_screencast || got_blobsink) {
      reset_quiet_counter(pthis);
    } else {
      increment_quiet_counter(pthis);
    }
  }
  zmq_close(pthis->screencast_socket);
  zmq_close(pthis->blobsink_socket);
}

void horseman_load_config(struct horseman_s* pthis,
                             struct horseman_config_s* config)
{
  pthis->on_video_msg = config->on_video_msg;
  pthis->on_audio_msg = config->on_audio_msg;
  pthis->callback_p = config->p;
}

int horseman_alloc(struct horseman_s** queue) {
  struct horseman_s* pthis =
  (struct horseman_s*)calloc(1, sizeof(struct horseman_s));
  pthis->zmq_ctx = zmq_ctx_new();
  pthis->screencast_socket = zmq_socket(pthis->zmq_ctx, ZMQ_PULL);
  pthis->blobsink_socket = zmq_socket(pthis->zmq_ctx, ZMQ_PULL);
  uv_mutex_init(&pthis->lock);

  // shape the thread pool a bit
  uv_cpu_info_t* cpu_infos;
  int cpu_count;
  uv_cpu_info(&cpu_infos, &cpu_count);
  cpu_count = fmax(1, cpu_count - 1);
  char str[4];
  sprintf(str, "%d", cpu_count);
  uv_free_cpu_info(cpu_infos, cpu_count);
  // ...or, don't. your mileage may vary. see what works for you.
  setenv("UV_THREADPOOL_SIZE", str, 0);
  pthis->loop = (uv_loop_t*) malloc(sizeof(uv_loop_t));
  uv_loop_init(pthis->loop);

  *queue = pthis;
  return 0;
}

void horseman_free(struct horseman_s* pthis) {
  int ret;
  pthis->is_running = 0;
  uv_stop(pthis->loop);
  free(pthis->loop);
  uv_thread_join(&pthis->zmq_thread);
  zmq_ctx_destroy(pthis->zmq_ctx);
  uv_mutex_destroy(&pthis->lock);
  free(pthis);
}

int horseman_start(struct horseman_s* pthis) {
  pthis->is_interrupted = 0;
  pthis->is_running = 1;
  int ret = uv_thread_create(&pthis->zmq_thread, horseman_zmq_main, pthis);
  int get = uv_thread_create(&pthis->loop_thread, horseman_loop_main, pthis);
  return ret | get;
}

int horseman_stop(struct horseman_s* pthis) {
  int ret;
  pthis->is_interrupted = 1;
  pthis->is_running = 0;
  do {
    ret = uv_loop_close(pthis->loop);
  } while (UV_EBUSY == ret);
  ret = uv_thread_join(&pthis->zmq_thread);
  int get = uv_thread_join(&pthis->loop_thread);
  return ret | get;
}

int64_t horseman_get_quiet_cycles(struct horseman_s* pthis) {
  int64_t ret = 0;
  uv_mutex_lock(&pthis->lock);
  ret = pthis->quiet_cycles;
  uv_mutex_unlock(&pthis->lock);
  return ret;
}
