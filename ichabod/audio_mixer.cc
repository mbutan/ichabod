//
//  audio_mixer.c
//  ichabod
//
//  Created by Charley Robinson on 6/7/17.
//

extern "C" {

#include <assert.h>
#include "audio_mixer.h"

}

#include <map>

struct audio_mixer_s {
  std::map<int64_t, AVFrame*> frame_map;
  int frame_size_pts;
  int sample_format;
  int sample_rate;
  int samples_per_frame;
  uint64_t channel_layout;
};

// Frames come in at different times but need to be aligned in the output stream
static inline int64_t normalize_pts(struct audio_mixer_s* pthis, int64_t pts) {
  return (pts - (pts & pthis->frame_size_pts));
}

static AVFrame* frame_for_pts(struct audio_mixer_s* pthis, int64_t ts) {
  AVFrame* result = NULL;
  int64_t pts = normalize_pts(pthis, ts);
  auto it = pthis->frame_map.find(pts);
  if (it == pthis->frame_map.end()) {
    result = av_frame_alloc();
    result->format = pthis->sample_format;
    result->sample_rate = pthis->sample_rate;
    result->channel_layout = pthis->channel_layout;
    result->nb_samples = pthis->samples_per_frame;
    av_frame_get_buffer(result, 0);
    pthis->frame_map[pts] = result;
  } else {
    result = it->second;
  }
  return result;
}

void audio_mixer_alloc(struct audio_mixer_s** mixer_out) {
  struct audio_mixer_s* pthis =
  (struct audio_mixer_s*)calloc(1, sizeof(struct audio_mixer_s));
  pthis->frame_map = std::map<int64_t, AVFrame*>();

  *mixer_out = pthis;
}

void audio_mixer_free(struct audio_mixer_s* pthis) {
  auto it = pthis->frame_map.begin();
  while ( it != pthis->frame_map.end()) {
    av_frame_free(&it->second);
    it++;
  }

  free(pthis);
}

int audio_mixer_consume(struct audio_mixer_s* pthis, AVFrame* frame) {
  // assume all future frames will have the same format and width.
  // this is necessary in order to align packets from different sources
  if (!pthis->frame_size_pts) {
    pthis->frame_size_pts = frame->nb_samples * 1000 / frame->sample_rate;
  }
  if (!pthis->channel_layout) {
    pthis->channel_layout = frame->channel_layout;
  }
  if (!pthis->sample_format) {
    pthis->sample_format = frame->format;
  }
  if (!pthis->samples_per_frame) {
    pthis->samples_per_frame = frame->nb_samples;
  }
  if (!pthis->sample_rate) {
    pthis->sample_rate = frame->sample_rate;
  }

  int64_t pts = normalize_pts(pthis, frame->pts);
  AVFrame* output_frame = frame_for_pts(pthis, pts);
  // TODO: figure out if this needs to be dynamically typed
  assert(AV_SAMPLE_FMT_S16 == output_frame->format);
  int16_t** out_data = (int16_t**)output_frame->data;
  int16_t** src_data = (int16_t**)frame->data;

  for (int channels = 0;
       channels < av_frame_get_channels(output_frame);
       channels++)
  {
    for (int samples = 0; samples < pthis->samples_per_frame; samples++) {
      out_data[channels][samples] += src_data[channels][samples];
    }
  }

  return 0;
}

int audio_mixer_get_next(struct audio_mixer_s* pthis, AVFrame* frame_out) {
  return 0;
}

int64_t audio_mixer_get_current_ts(struct audio_mixer_s* pthis) {
  return pthis->frame_map.begin()->first;
}
