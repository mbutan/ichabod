//
//  media_queue.c
//  ichabod
//
//  Created by Charley Robinson on 6/1/17.
//

extern "C" {

#include <stdlib.h>
#include <zmq.h>
#include <uv.h>
#include "media_queue.h"
#include "frame_generator.h"

}

#include <queue>

struct media_queue_s {
  void* zmq_ctx;
  void* screencast_socket;
  void* blobsink_socket;
  char is_interrupted;
  uv_thread_t worker_thread;
  uv_mutex_t queue_lock;
  std::queue<AVFrame*> frame_queue;
};

struct message_s {
  char* sz_data;
  int64_t timestamp;
};

static int receive_message(void* socket, struct message_s* msg,
                           char* got_message)
{
  int ret;
  if (msg->sz_data) {
    free(msg->sz_data);
  }
  memset(msg, 0, sizeof(struct message_s));
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
    //  Process the message frame
    uint8_t* sz_msg = (uint8_t*)malloc(zmq_msg_size(&message) + 1);
    memcpy(sz_msg, zmq_msg_data(&message), zmq_msg_size(&message));
    sz_msg[zmq_msg_size(&message)] = '\0';

    if (!msg->sz_data) {
      msg->sz_data = (char*)sz_msg;
    } else if (!msg->timestamp) {
      msg->timestamp = (int64_t)atof((char*)sz_msg);
      free(sz_msg);
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

static int receive_screencast(struct media_queue_s* pthis) {
  char got_message = 0;
  struct message_s msg = {0};
  // wait for zmq message
  int ret = receive_message(pthis->screencast_socket, &msg, &got_message);
  // process message
  if (ret) {
    printf("trouble? %d\n", ret);
  } else if (got_message) {
    printf("received msg len=%ld ts=%lld\n",
           strlen(msg.sz_data),
           msg.timestamp);
    AVFrame* frame = NULL;
    generate_frame(msg.sz_data, &frame);
    frame->pts = msg.timestamp;
    uv_mutex_lock(&pthis->queue_lock);
    pthis->frame_queue.push(frame);
    uv_mutex_unlock(&pthis->queue_lock);
  }
  return ret;
}

static void media_queue_main(void* p) {
  int ret;
  printf("media queue is online %p\n", p);
  struct media_queue_s* pthis = (struct media_queue_s*)p;
  int t = 100;
  ret = zmq_connect(pthis->screencast_socket, "ipc:///tmp/ichabod-screencast");
  ret = zmq_connect(pthis->blobsink_socket, "ipc:///tmp/ichabod-blobsink");
  if (ret) {
    printf("failed to connect to media queue socket. errno %d\n", errno);
    return;
  }
  ret = zmq_setsockopt(pthis->screencast_socket, ZMQ_RCVTIMEO, &t, sizeof(int));
  ret = zmq_setsockopt(pthis->blobsink_socket, ZMQ_RCVTIMEO, &t, sizeof(int));
  while (!pthis->is_interrupted) {
    ret = receive_screencast(pthis);
    //ret = receive_blobsink(pthis);
  }
}

int media_queue_alloc(struct media_queue_s** queue) {
  struct media_queue_s* pthis =
  (struct media_queue_s*)calloc(1, sizeof(struct media_queue_s));
  pthis->zmq_ctx = zmq_ctx_new();
  pthis->screencast_socket = zmq_socket(pthis->zmq_ctx, ZMQ_PULL);
  pthis->frame_queue = std::queue<AVFrame*>();
  uv_mutex_init(&pthis->queue_lock);
  *queue = pthis;
  return 0;
}

void media_queue_free(struct media_queue_s* pthis) {
  zmq_ctx_destroy(pthis->zmq_ctx);
  while (!pthis->frame_queue.empty()) {
    AVFrame* frame = pthis->frame_queue.front();
    av_frame_free(&frame);
    pthis->frame_queue.pop();
  }
  uv_mutex_destroy(&pthis->queue_lock);
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
  zmq_close(pthis->screencast_socket);
  return ret;
}

int media_queue_has_next(struct media_queue_s* pthis) {
  int ret = 0;
  uv_mutex_lock(&pthis->queue_lock);
  ret = !pthis->frame_queue.empty();
  uv_mutex_unlock(&pthis->queue_lock);
  return ret;
}

int media_queue_get_next(struct media_queue_s* pthis, AVFrame** frame) {
  AVFrame* ret = NULL;
  uv_mutex_lock(&pthis->queue_lock);
  if (!pthis->frame_queue.empty()) {
    ret = pthis->frame_queue.front();
    pthis->frame_queue.pop();
  }
  uv_mutex_unlock(&pthis->queue_lock);
  *frame = ret;
  return (NULL == ret);
}
