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
#include <assert.h>
#include "media_queue.h"
#include "frame_generator.h"
#include "audio_mixer.h"
#include "growing_file_audio_source.h"
#include "audio_frame_converter.h"
#include "video_frame_buffer.h"

}

#include <string>
#include <map>

struct media_queue_s {
  void* zmq_ctx;
  void* screencast_socket;
  void* blobsink_socket;
  char is_interrupted;
  uv_thread_t worker_thread;
  uv_mutex_t frame_queue_lock;
  std::map<int64_t, AVFrame*> video_frame_queue;
  std::map<int64_t, AVFrame*> audio_frame_queue;
  struct audio_mixer_s* audio_mixer;
  std::map<std::string, struct audio_source_s*> audio_sources;
  struct frame_converter_s* audio_conv;
  AVFormatContext* output_format;
  AVCodecContext* audio_context;
  AVCodecContext* video_context;
  double initial_video_timestamp;
  double initial_audio_timestamp;
  struct frame_buffer_s* video_buffer;
};

struct message_s {
  char* sz_data;
  double timestamp;
  char* sz_sid;
};

static int64_t video_queue_tail_pts(struct media_queue_s* pthis) {
  int64_t result;
  uv_mutex_lock(&pthis->frame_queue_lock);
  if (pthis->video_frame_queue.empty()) {
    result = 0;
  } else {
    result = pthis->video_frame_queue.end()->first;
  }
  uv_mutex_unlock(&pthis->frame_queue_lock);
  return result;
}

static int64_t audio_frame_queue_head_pts(struct media_queue_s* pthis) {
  int64_t result;
  uv_mutex_lock(&pthis->frame_queue_lock);
  if (pthis->audio_frame_queue.empty()) {
    result = 0;
  } else {
    result = pthis->audio_frame_queue.begin()->first;
  }
  uv_mutex_unlock(&pthis->frame_queue_lock);
  return result;
}

static void audio_frame_queue_push_safe
(struct media_queue_s* pthis, AVFrame* frame)
{
  uv_mutex_lock(&pthis->frame_queue_lock);
  pthis->audio_frame_queue[frame->pts] = frame;
  uv_mutex_unlock(&pthis->frame_queue_lock);
}

static void video_frame_queue_push_safe
(struct media_queue_s* pthis, AVFrame* frame)
{
  uv_mutex_lock(&pthis->frame_queue_lock);
  pthis->video_frame_queue[frame->pts] = frame;
  uv_mutex_unlock(&pthis->frame_queue_lock);
}

// mergesort-style management of two queues at once. there's probably a cleaner
// way to do this, but I haven't thought of it yet.
static int frame_queue_pop_safe(struct media_queue_s* pthis, AVFrame** frame) {
  AVFrame* audio_head = NULL;
  AVFrame* video_head = NULL;
  AVFrame* ret = NULL;
  uv_mutex_lock(&pthis->frame_queue_lock);
  if (!pthis->audio_frame_queue.empty()) {
    auto audio_it = pthis->audio_frame_queue.begin();
    audio_head = audio_it->second;
  }
  if (!pthis->video_frame_queue.empty()) {
    auto video_it = pthis->video_frame_queue.begin();
    video_head = video_it->second;
  }
  if (audio_head && video_head) {
    // audio pts saved in timescale 48000Hz, rescale back to millis to compare
    // to the video timestamp head for a fair comparison.
    double audio_pts_millis =
    (audio_head->pts / pthis->audio_context->sample_rate);
    // additionally, compensate for the offset of when audio and video started
    // this should get fixed: audio mixer needs to run always, not when first
    // subscriber is set up.
    //+ (pthis->initial_audio_timestamp - pthis->initial_video_timestamp);
    ret = (audio_pts_millis < video_head->pts) ? audio_head : video_head;
  }
  if (ret && audio_head == ret) {
    pthis->audio_frame_queue.erase(audio_head->pts);
    // once again compensate for audio offset time :-(
    double audio_offset_time =
    (pthis->initial_audio_timestamp - pthis->initial_video_timestamp) *
    (pthis->audio_context->sample_rate / 1000);
    ret->pts += audio_offset_time;
  }
  if (ret && video_head == ret) {
    pthis->video_frame_queue.erase(video_head->pts);
  }
  uv_mutex_unlock(&pthis->frame_queue_lock);
  *frame = ret;
  return (NULL == ret);
}

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
    pthis->audio_sources[msg->sz_sid] = source;
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
    ret = generate_frame(msg.sz_data, &frame);
    if (ret) {
      printf("failed to generate frame for ts %f\n", msg.timestamp);
      return ret;
    }
    if (pthis->initial_video_timestamp < 0) {
      pthis->initial_video_timestamp = msg.timestamp;
    }
    double frame_pts = msg.timestamp - pthis->initial_video_timestamp;
    frame->pts = frame_pts;
    frame_buffer_consume(pthis->video_buffer, frame);
    while (frame_buffer_has_next(pthis->video_buffer)) {
      ret = frame_buffer_get_next(pthis->video_buffer, &frame);
      if (!ret && frame) {
        video_frame_queue_push_safe(pthis, frame);
      }
    }
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
    // TODO: This assumes all sources have the same format. Might be we need
    // a different converter for each source (and the converter runs before
    // the mixer)
    if (!pthis->audio_conv) {
      pthis->initial_audio_timestamp = msg.timestamp;
      const AVCodecContext* audio_context = audio_source_get_codec(source);
      struct frame_converter_config_s converter_config;
      converter_config.num_channels = audio_context->channels;
      converter_config.output_format = pthis->audio_context->sample_fmt;
      converter_config.sample_rate = audio_context->sample_rate;
      converter_config.samples_per_frame = pthis->audio_context->frame_size;
      converter_config.channel_layout = audio_context->channel_layout;
      //
      // converter_config.pts_offset = pthis->initial_audio_timestamp;
      converter_config.pts_offset = 0; 
      frame_converter_create(&pthis->audio_conv, &converter_config);
    }
    if (ret) {
      printf("failed to open audio source %s\n", msg.sz_sid);
    } else {
      // pull audio from file into mixer
      extract_audio(source, pthis->audio_mixer);
      // pull from mixer into frame converter
      int64_t mixer_ts = audio_mixer_get_current_ts(pthis->audio_mixer);
      AVFrame* frame = NULL;
      // don't pull out of the mixer until at least 2s of content is buffered
      while (audio_mixer_get_length(pthis->audio_mixer) > 2000) {
        ret = audio_mixer_get_next(pthis->audio_mixer, &frame);
        if (frame && !ret) {
          frame_converter_consume(pthis->audio_conv, frame);
        }
        mixer_ts = audio_mixer_get_current_ts(pthis->audio_mixer);
      }
      // finally, pull from frame converter into audio queue
      frame = NULL;
      ret = frame_converter_get_next(pthis->audio_conv, &frame);
      while (!ret) {
        if (frame) {
          audio_frame_queue_push_safe(pthis, frame);
        }
        ret = frame_converter_get_next(pthis->audio_conv, &frame);
      }
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
  pthis->audio_frame_queue = std::map<int64_t, AVFrame*>();
  pthis->video_frame_queue = std::map<int64_t, AVFrame*>();
  pthis->audio_sources = std::map<std::string, struct audio_source_s*>();
  uv_mutex_init(&pthis->frame_queue_lock);
  audio_mixer_alloc(&pthis->audio_mixer);
  struct audio_mixer_config_s mixer_config;
  mixer_config.output_codec = config->audio;
  mixer_config.output_format = config->format;
  audio_mixer_load_config(pthis->audio_mixer, &mixer_config);
  pthis->output_format = config->format;
  pthis->audio_context = config->audio;
  pthis->video_context = config->video;
  pthis->initial_video_timestamp = -1;
  // 30fps constant output frame buffer. TODO: make this configurable
  frame_buffer_alloc(&pthis->video_buffer, 1000. / 30.);

  *queue = pthis;
  return 0;
}

void media_queue_free(struct media_queue_s* pthis) {
  zmq_ctx_destroy(pthis->zmq_ctx);
  auto frame_queue_it = pthis->audio_frame_queue.begin();
  while (frame_queue_it != pthis->audio_frame_queue.end()) {
    AVFrame* frame = frame_queue_it->second;
    av_frame_free(&frame);
    pthis->audio_frame_queue.erase(frame_queue_it);
    frame_queue_it++;
  }
  frame_queue_it = pthis->video_frame_queue.begin();
  while (frame_queue_it != pthis->video_frame_queue.end()) {
    AVFrame* frame = frame_queue_it->second;
    assert(1 != (int64_t)frame); //wtf?
    av_frame_free(&frame);
    pthis->video_frame_queue.erase(frame_queue_it);
    frame_queue_it++;
  }
  auto sources_it = pthis->audio_sources.begin();
  while (sources_it != pthis->audio_sources.end()) {
    audio_source_free(sources_it->second);
    sources_it++;
  }
  pthis->audio_sources.clear();
  uv_mutex_destroy(&pthis->frame_queue_lock);
  audio_mixer_free(pthis->audio_mixer);
  frame_buffer_free(pthis->video_buffer);
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
  uv_mutex_lock(&pthis->frame_queue_lock);
  ret = !pthis->audio_frame_queue.empty() && !pthis->video_frame_queue.empty();
  uv_mutex_unlock(&pthis->frame_queue_lock);
  return ret;
}

int media_queue_get_next(struct media_queue_s* pthis, AVFrame** frame) {
  return frame_queue_pop_safe(pthis, frame);
}
