//
//  archive_mixer.c
//  ichabod
//
//  Created by Charley Robinson on 6/15/17.
//

extern "C" {

#include <uv.h>
#include "archive_mixer.h"
#include "audio_mixer.h"
#include "growing_file_audio_source.h"
#include "video_frame_buffer.h"
#include "audio_frame_converter.h"
#include "pulse_audio_source.h"
}

#include <map>
#include <string>

struct archive_mixer_s {
  double initial_timestamp;
  double min_buffer_time;
  struct pulse_s* pulse_audio;
  struct audio_mixer_s* audio_mixer;
  struct frame_converter_s* audio_frame_converter;
  struct frame_buffer_s* video_buffer;
  uv_mutex_t queue_lock;
  std::map<int64_t, AVFrame*> video_frame_queue;
  std::map<int64_t, AVFrame*> audio_frame_queue;
  std::map<std::string, struct audio_source_s*> audio_sources;

  AVFormatContext* format_out;
  AVCodecContext* audio_ctx_out;
  AVStream* audio_stream_out;
  AVCodecContext* video_ctx_out;
  AVStream* video_stream_out;

  size_t audio_size_estimated;
  size_t video_size_estimated;
};

#pragma mark - Private Utilities

static void audio_frame_queue_push_safe
(struct archive_mixer_s* pthis, AVFrame* frame)
{
  uv_mutex_lock(&pthis->queue_lock);
  pthis->audio_frame_queue[frame->pts] = frame;
  pthis->audio_size_estimated = pthis->audio_frame_queue.size();
  uv_mutex_unlock(&pthis->queue_lock);
}

static void video_frame_queue_push_safe
(struct archive_mixer_s* pthis, AVFrame* frame)
{
  uv_mutex_lock(&pthis->queue_lock);
  pthis->video_frame_queue[frame->pts] = frame;
  pthis->video_size_estimated = pthis->video_frame_queue.size();
  uv_mutex_unlock(&pthis->queue_lock);
}

// mergesort-style management of two queues at once. there's probably a cleaner
// way to do this, but I haven't thought of it yet.
static int frame_queue_pop_safe(struct archive_mixer_s* pthis,
                                AVFrame** frame,
                                enum AVMediaType* media_type)
{
  *media_type = AVMEDIA_TYPE_UNKNOWN;
  AVFrame* audio_head = NULL;
  AVFrame* video_head = NULL;
  AVFrame* ret = NULL;
  uv_mutex_lock(&pthis->queue_lock);
  if (!pthis->audio_frame_queue.empty()) {
    auto audio_it = pthis->audio_frame_queue.begin();
    audio_head = audio_it->second;
  }
  if (!pthis->video_frame_queue.empty()) {
    auto video_it = pthis->video_frame_queue.begin();
    video_head = video_it->second;
  }
  if (audio_head && video_head) {
    // articulate this comparison better: pts are presented in different units
    // so we need to rescale here before making a fair comparison.
    ret = (audio_head->pts < video_head->pts * 48) ? audio_head : video_head;
  }
  if (ret && audio_head == ret) {
    pthis->audio_frame_queue.erase(audio_head->pts);
    *media_type = AVMEDIA_TYPE_AUDIO;
  }
  if (ret && video_head == ret) {
    pthis->video_frame_queue.erase(video_head->pts);
    *media_type = AVMEDIA_TYPE_VIDEO;
  }
  pthis->video_size_estimated = pthis->video_frame_queue.size();
  pthis->audio_size_estimated = pthis->audio_frame_queue.size();
  uv_mutex_unlock(&pthis->queue_lock);
  printf("mixer: %zu audio %zu video frames in queue\n",
         pthis->audio_size_estimated, pthis->video_size_estimated);
  *frame = ret;
  return (NULL == ret);
}

// fetch existing or configure a new audio source
static int get_audio_source(struct archive_mixer_s* pthis,
                            const char* subscriber_id,
                            const char* file_path,
                            double timestamp,
                            struct audio_source_s** source_out)
{
  int ret;
  auto it = pthis->audio_sources.find(subscriber_id);
  if (it != pthis->audio_sources.end()) {
    *source_out = it->second;
    return 0;
  }
  struct audio_source_s* source;
  audio_source_alloc(&source);
  struct audio_source_config_s config;
  config.path = file_path;
  config.initial_timestamp = timestamp;
  ret = audio_source_load_config(source, &config);
  if (ret) {
    *source_out = NULL;
  } else {
    *source_out = source;
    pthis->audio_sources[subscriber_id] = source;
  }
  return ret;
}

#pragma mark - Public API

int archive_mixer_create(struct archive_mixer_s** mixer_out,
                         struct archive_mixer_config_s* config)
{
  struct archive_mixer_s* pthis = (struct archive_mixer_s*)
  calloc(1, sizeof(struct archive_mixer_s));
  pthis->audio_frame_queue = std::map<int64_t, AVFrame*>();
  pthis->video_frame_queue = std::map<int64_t, AVFrame*>();
  pthis->audio_sources = std::map<std::string, struct audio_source_s*>();
  pthis->initial_timestamp = config->initial_timestamp;
  pthis->min_buffer_time = config->min_buffer_time;
  pthis->format_out = config->format_out;
  pthis->audio_ctx_out = config->audio_ctx_out;
  pthis->video_ctx_out = config->video_ctx_out;
  pthis->audio_stream_out = config->audio_stream_out;
  pthis->video_stream_out = config->video_stream_out;
  pthis->pulse_audio = config->pulse_audio;
  audio_mixer_alloc(&pthis->audio_mixer);
  struct audio_mixer_config_s mixer_config;
  mixer_config.output_codec = config->audio_ctx_out;
  mixer_config.output_format = config->format_out;
  audio_mixer_load_config(pthis->audio_mixer, &mixer_config);


  struct frame_converter_config_s converter_config;
  converter_config.num_channels = pthis->audio_ctx_out->channels;
  converter_config.output_format = pthis->audio_ctx_out->sample_fmt;
  converter_config.sample_rate = pthis->audio_ctx_out->sample_rate;
  converter_config.samples_per_frame = pthis->audio_ctx_out->frame_size;
  converter_config.channel_layout = pthis->audio_ctx_out->channel_layout;
  converter_config.pts_offset = 0;
  frame_converter_create(&pthis->audio_frame_converter, &converter_config);

  double pts_interval =
  (double)config->video_ctx_out->time_base.den / config->video_fps_out;
  frame_buffer_alloc(&pthis->video_buffer, pts_interval);
  uv_mutex_init(&pthis->queue_lock);
  *mixer_out = pthis;
  return 0;
}
void archive_mixer_free(struct archive_mixer_s* pthis) {
  uv_mutex_destroy(&pthis->queue_lock);
  free(pthis);
}

void archive_mixer_drain_audio(struct archive_mixer_s* pthis) {
  int ret;
  AVFrame* frame = NULL;
  while (pulse_has_next(pthis->pulse_audio)) {
    ret = pulse_get_next(pthis->pulse_audio, &frame);
    if (ret) {
      continue;
    }
    frame_converter_consume(pthis->audio_frame_converter, frame);
    av_frame_free(&frame);
  }

  // finally, pull from frame converter into audio queue
  frame = NULL;
  ret = frame_converter_get_next(pthis->audio_frame_converter, &frame);
  while (!ret) {
    if (frame) {
      audio_frame_queue_push_safe(pthis, frame);
    }
    ret = frame_converter_get_next(pthis->audio_frame_converter, &frame);
  }
}

void archive_mixer_consume_video(struct archive_mixer_s* pthis,
                                 AVFrame* frame, double timestamp)
{
  frame->pts = (timestamp - pthis->initial_timestamp);
  frame_buffer_consume(pthis->video_buffer, frame);
  while (frame_buffer_has_next(pthis->video_buffer)) {
    int ret = frame_buffer_get_next(pthis->video_buffer, &frame);
    if (!ret && frame) {
      video_frame_queue_push_safe(pthis, frame);
    }
  }
}

void archive_mixer_consume_audio(struct archive_mixer_s* pthis,
                                 const char* file_path, double timestamp,
                                 const char* subscriber_id)
{
  struct audio_source_s* source = NULL;
  int ret = get_audio_source(pthis, subscriber_id,
                             file_path, timestamp, &source);
  if (ret || !source) {
    printf("unable to get audio source %s\n", subscriber_id);
    return;
  }
  double source_ts_offset =
  audio_source_get_initial_timestamp(source) - pthis->initial_timestamp;
  source_ts_offset -= 1000; // is there a capture delay somewhere?
  AVFrame* frame = NULL;
  while (!ret) {
    ret = audio_source_next_frame(source, &frame);
    if (frame) {
      frame->pts += source_ts_offset;
      audio_mixer_consume(pthis->audio_mixer, frame);
      av_frame_free(&frame);
    }
  }
  int64_t mixer_ts = 0;
  while (audio_mixer_get_length(pthis->audio_mixer) >
         pthis->min_buffer_time * 1000)
  {
    ret = audio_mixer_get_next(pthis->audio_mixer, &frame);
    if (frame && !ret) {
      frame_converter_consume(pthis->audio_frame_converter, frame);
    }
    mixer_ts = audio_mixer_get_head_ts(pthis->audio_mixer);
  }

  // finally, pull from frame converter into audio queue
  frame = NULL;
  ret = frame_converter_get_next(pthis->audio_frame_converter, &frame);
  while (!ret) {
    if (frame) {
      audio_frame_queue_push_safe(pthis, frame);
    }
    ret = frame_converter_get_next(pthis->audio_frame_converter, &frame);
  }
}

char archive_mixer_has_next(struct archive_mixer_s* pthis) {
  char ret = 0;
  uv_mutex_lock(&pthis->queue_lock);
  ret = !pthis->audio_frame_queue.empty() && !pthis->video_frame_queue.empty();
  uv_mutex_unlock(&pthis->queue_lock);
  return ret;
}

int archive_mixer_get_next(struct archive_mixer_s* pthis, AVFrame** frame_out,
                           enum AVMediaType* media_type)
{
  int ret = 0;
  ret = frame_queue_pop_safe(pthis, frame_out, media_type);
  return ret;
}

size_t archive_mixer_get_size(struct archive_mixer_s* pthis) {
  return pthis->video_size_estimated + pthis->audio_size_estimated;
}
