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
#include "audio_mixer.h"
#include "growing_file_audio_source.h"
#include "audio_frame_converter.h"

}

#include <string>
#include <map>
#include <queue>

struct media_queue_s {
  void* zmq_ctx;
  void* screencast_socket;
  void* blobsink_socket;
  char is_interrupted;
  uv_thread_t worker_thread;
  uv_mutex_t video_queue_lock;
  std::queue<AVFrame*> video_frame_queue;
  int audio_samples_per_frame;
  struct audio_mixer_s* audio_mixer;
  std::map<std::string, struct audio_source_s*> audio_sources;
  struct frame_converter_s* audio_conv;
  AVFormatContext* output_format;
  AVCodecContext* audio_context;
  AVCodecContext* video_context;
};

struct message_s {
  char* sz_data;
  double timestamp;
  char* sz_sid;
};

static int get_audio_source(struct media_queue_s* pthis,
                            struct message_s* msg,
                            struct audio_source_s** source_out)
{
  int ret;
  auto it = pthis->audio_sources.find(msg->sz_sid);
  if (it != pthis->audio_sources.end()) {
    *source_out = it->second;
    return 0;
  }
  struct audio_source_s* source;
  audio_source_alloc(&source);
  struct audio_source_config_s config;
  config.path = msg->sz_data;
  config.initial_timestamp = msg->timestamp;
  ret = audio_source_load_config(source, &config);
  if (ret) {
    *source_out = NULL;
  } else {
    *source_out = source;
  }
  return ret;
}

static int receive_message(void* socket, struct message_s* msg,
                           char* got_message)
{
  int ret;
  if (msg->sz_data) {
    free(msg->sz_data);
  }
  if (msg->sz_sid) {
    free(msg->sz_sid);
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

static int receive_screencast(struct media_queue_s* pthis) {
  char got_message = 0;
  struct message_s msg = {0};
  // wait for zmq message
  int ret = receive_message(pthis->screencast_socket, &msg, &got_message);
  // process message
  if (ret) {
    printf("trouble? %d %d\n", ret, errno);
  } else if (got_message) {
    printf("received screencast len=%ld ts=%f\n",
           strlen(msg.sz_data),
           msg.timestamp);
    AVFrame* frame = NULL;
    generate_frame(msg.sz_data, &frame);
    frame->pts = msg.timestamp;
    uv_mutex_lock(&pthis->video_queue_lock);
    pthis->video_frame_queue.push(frame);
    uv_mutex_unlock(&pthis->video_queue_lock);
  }
  return ret;
}

static int receive_blobsink(struct media_queue_s* pthis) {
  char got_message = 0;
  struct message_s msg = {0};
  int ret = receive_message(pthis->blobsink_socket, &msg, &got_message);
  if (ret) {
    printf("receive_blobsink: %d %d\n", ret, errno);
  } else if (got_message) {
    printf("received blob len=%ld ts=%f sid=%s\n",
           strlen(msg.sz_data), msg.timestamp, msg.sz_sid);
    struct audio_source_s* source;
    ret = get_audio_source(pthis, &msg, &source);
    if (ret) {
      printf("failed to open audio source %s\n", msg.sz_sid);
    } else {
      extract_audio(source, pthis->audio_mixer);
    }
  }
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
    ret = receive_screencast(pthis);
    ret = receive_blobsink(pthis);
  }
  zmq_close(pthis->screencast_socket);
  zmq_close(pthis->blobsink_socket);
}

int media_queue_create(struct media_queue_s** queue,
                       struct media_queue_config_s* config)
{
  struct media_queue_s* pthis =
  (struct media_queue_s*)calloc(1, sizeof(struct media_queue_s));
  pthis->zmq_ctx = zmq_ctx_new();
  pthis->screencast_socket = zmq_socket(pthis->zmq_ctx, ZMQ_PULL);
  pthis->blobsink_socket = zmq_socket(pthis->zmq_ctx, ZMQ_PULL);
  pthis->video_frame_queue = std::queue<AVFrame*>();
  pthis->audio_sources = std::map<std::string, struct audio_source_s*>();
  uv_mutex_init(&pthis->video_queue_lock);
  pthis->audio_samples_per_frame = 960; // default: 48000 Hz @ 20 ms
  audio_mixer_alloc(&pthis->audio_mixer);
  pthis->output_format = config->format;
  pthis->audio_context = config->audio;
  pthis->video_context = config->video;
  struct frame_converter_config_s converter_config;
  converter_config.num_channels = config->audio->channels;
  converter_config.output_format = config->audio->sample_fmt;
  converter_config.sample_rate = config->audio->sample_rate;
  converter_config.samples_per_frame = config->audio->frame_size;
  frame_converter_create(&pthis->audio_conv, &converter_config);
  *queue = pthis;
  return 0;
}

void media_queue_free(struct media_queue_s* pthis) {
  zmq_ctx_destroy(pthis->zmq_ctx);
  while (!pthis->video_frame_queue.empty()) {
    AVFrame* frame = pthis->video_frame_queue.front();
    av_frame_free(&frame);
    pthis->video_frame_queue.pop();
  }
  auto it = pthis->audio_sources.begin();
  while (it != pthis->audio_sources.end()) {
    audio_source_free(it->second);
    it++;
  }
  pthis->audio_sources.clear();
  uv_mutex_destroy(&pthis->video_queue_lock);
  audio_mixer_free(pthis->audio_mixer);
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

int media_queue_has_next(struct media_queue_s* pthis) {
  int ret = 0;
  uv_mutex_lock(&pthis->video_queue_lock);
  ret = !pthis->video_frame_queue.empty();
  uv_mutex_unlock(&pthis->video_queue_lock);
  return ret;
}

int media_queue_get_next(struct media_queue_s* pthis, AVFrame** frame) {
  AVFrame* ret = NULL;
  uv_mutex_lock(&pthis->video_queue_lock);
  if (!pthis->video_frame_queue.empty()) {
    ret = pthis->video_frame_queue.front();
    pthis->video_frame_queue.pop();
  }
  uv_mutex_unlock(&pthis->video_queue_lock);
  *frame = ret;
  return (NULL == ret);
}
