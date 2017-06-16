//
//  media_queue.c
//  ichabod
//
//  Created by Charley Robinson on 6/1/17.
//

#include <stdlib.h>
#include <zmq.h>
#include <uv.h>
#include <assert.h>
#include "media_queue.h"
#include "frame_generator.h"
#include "audio_mixer.h"
#include "growing_file_audio_source.h"
#include "audio_frame_converter.h"
#include "video_frame_buffer.h"

struct media_queue_s {
  void* zmq_ctx;
  void* screencast_socket;
  void* blobsink_socket;
  char is_interrupted;
  uv_thread_t worker_thread;
  
  uv_mutex_t lock;
  int64_t quiet_cycles;

  void (*on_video_msg)(struct media_queue_s* queue,
                       struct media_queue_msg_s* msg, void* p);
  void (*on_audio_msg)(struct media_queue_s* queue,
                       struct media_queue_msg_s* msg, void* p);
  void* callback_p;
};

void reset_quiet_counter(struct media_queue_s* pthis) {
  uv_mutex_lock(&pthis->lock);
  pthis->quiet_cycles = 0;
  uv_mutex_unlock(&pthis->lock);
}

void increment_quiet_counter(struct media_queue_s* pthis) {
  uv_mutex_lock(&pthis->lock);
  pthis->quiet_cycles++;
  uv_mutex_unlock(&pthis->lock);
}

static void media_queue_msg_free(struct media_queue_msg_s* msg) {
  if (msg->sz_data) {
    free(msg->sz_data);
  }
  if (msg->sz_sid) {
    free(msg->sz_sid);
  }
  free(msg);
}

static int receive_message(void* socket, struct media_queue_msg_s* msg,
                           char* got_message)
{
  int ret;
  if (msg->sz_data) {
    free(msg->sz_data);
  }
  if (msg->sz_sid) {
    free(msg->sz_sid);
  }
  memset(msg, 0, sizeof(struct media_queue_msg_s));
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

static int receive_screencast(struct media_queue_s* pthis, char* got_message) {
  struct media_queue_msg_s* msg = calloc(1, sizeof(struct media_queue_msg_s));
  // wait for zmq message
  int ret = receive_message(pthis->screencast_socket, msg, got_message);
  // process message
  if (ret) {
    printf("trouble? %d %d\n", ret, errno);
  } else if (*got_message) {
    printf("received screencast len=%ld ts=%f\n",
           strlen(msg->sz_data),
           msg->timestamp);
    pthis->on_video_msg(pthis, msg, pthis->callback_p);
  }
  media_queue_msg_free(msg);
  return ret;
}

static int receive_blobsink(struct media_queue_s* pthis, char* got_message) {
  struct media_queue_msg_s* msg = calloc(1, sizeof(struct media_queue_msg_s));
  int ret = receive_message(pthis->blobsink_socket, msg, got_message);
  if (ret) {
    printf("receive_blobsink: %d %d\n", ret, errno);
  } else if (*got_message) {
    printf("received blob len=%ld ts=%f sid=%s\n",
           strlen(msg->sz_data), msg->timestamp, msg->sz_sid);
    pthis->on_audio_msg(pthis, msg, pthis->callback_p);
  }
  media_queue_msg_free(msg);
  return ret;
}

static void media_queue_main(void* p) {
  int ret;
  printf("media queue is online %p\n", p);
  struct media_queue_s* pthis = (struct media_queue_s*)p;
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

void media_queue_load_config(struct media_queue_s* pthis,
                             struct media_queue_config_s* config)
{
  pthis->on_video_msg = config->on_video_msg;
  pthis->on_audio_msg = config->on_audio_msg;
  pthis->callback_p = config->p;
}

int media_queue_alloc(struct media_queue_s** queue) {
  struct media_queue_s* pthis =
  (struct media_queue_s*)calloc(1, sizeof(struct media_queue_s));
  pthis->zmq_ctx = zmq_ctx_new();
  pthis->screencast_socket = zmq_socket(pthis->zmq_ctx, ZMQ_PULL);
  pthis->blobsink_socket = zmq_socket(pthis->zmq_ctx, ZMQ_PULL);
  uv_mutex_init(&pthis->lock);

  *queue = pthis;
  return 0;
}

void media_queue_free(struct media_queue_s* pthis) {
  zmq_ctx_destroy(pthis->zmq_ctx);
  uv_mutex_destroy(&pthis->lock);
  free(pthis);
}

int media_queue_start(struct media_queue_s* pthis) {
  pthis->is_interrupted = 0;
  int ret = uv_thread_create(&pthis->worker_thread, media_queue_main, pthis);
  return ret;
}

int media_queue_stop(struct media_queue_s* pthis) {
  pthis->is_interrupted = 1;
  int ret = uv_thread_join(&pthis->worker_thread);
  return ret;
}

int64_t media_queue_get_quiet_cycles(struct media_queue_s* pthis) {
  int64_t ret = 0;
  uv_mutex_lock(&pthis->lock);
  ret = pthis->quiet_cycles;
  uv_mutex_unlock(&pthis->lock);
  return ret;
}
