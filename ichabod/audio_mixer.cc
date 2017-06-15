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
  AVFormatContext* out_format_context;
  AVCodecContext* out_codec_context;
  int sample_format;
  int sample_rate;
  uint64_t channel_layout;
  int num_channels;
  int64_t last_pts_out;
};

static inline int samples_per_timeslice(struct audio_mixer_s* pthis) {
  // TODO: time base is definitely buried in format context. unbury it.
  // Also: this whole class should have configuable time slices. If we need
  // to speed things up, this would be a good place to start.
  return pthis->out_codec_context->sample_rate / 1000;
}

static AVFrame* frame_for_pts(struct audio_mixer_s* pthis, int64_t pts) {
  AVFrame* result = NULL;
  auto it = pthis->frame_map.find(pts);
  if (it == pthis->frame_map.end()) {
    result = av_frame_alloc();
    result->format = pthis->sample_format;
    result->sample_rate = pthis->sample_rate;
    result->channel_layout = pthis->channel_layout;
    result->channels = pthis->num_channels;
    result->nb_samples = samples_per_timeslice(pthis);
    result->pts = pts;
    av_frame_get_buffer(result, 0);
    int size =
    av_samples_get_buffer_size(&size, pthis->num_channels,
                               samples_per_timeslice(pthis),
                               (enum AVSampleFormat)pthis->sample_format, 0);
    memset(result->data[0], 0, size);

    pthis->frame_map[pts] = result;
  } else {
    result = it->second;
  }
  return result;
}

int audio_mixer_load_config(struct audio_mixer_s* pthis,
                            struct audio_mixer_config_s* config)
{
  pthis->out_codec_context = config->output_codec;
  pthis->out_format_context = config->output_format;
  return 0;
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
  // assume all future input frames will have the same format and width.
  // this is necessary in order to align packets from different sources
  if (!pthis->channel_layout) {
    pthis->channel_layout = frame->channel_layout;
  }
  if (!pthis->sample_format) {
    pthis->sample_format = AV_SAMPLE_FMT_FLTP;
  }
  if (!pthis->sample_rate) {
    pthis->sample_rate = frame->sample_rate;
  }
  if (!pthis->num_channels) {
    pthis->num_channels = frame->channels;
  }
  // Chop input frame into as many slices as we need. Mix each slice together
  // based on the global (millisecond) time clock.
  // TODO: figure out if sample formats need to be dynamically typed. For now,
  // Assuming everything comes in as signed 16-bit integer
  assert(AV_SAMPLE_FMT_S16 == frame->format);
  assert(1000 * frame->nb_samples / frame->sample_rate == frame->pkt_duration);
  printf("audio mixer: mix down %lld ms / %lld samples "
         "(%lld - %lld)\n", frame->pkt_duration, frame->nb_samples, frame->pts,
         frame->pts + frame->pkt_duration);
  for (int64_t i = 0; i < frame->pkt_duration; i++) {
    AVFrame* mix_frame = frame_for_pts(pthis, (frame->pts) + i);
    assert(AV_SAMPLE_FMT_FLTP == mix_frame->format);
    int16_t** src_data = (int16_t**)frame->data;
    float** out_data = (float**)mix_frame->data;
    int64_t src_sample_offset = i * samples_per_timeslice(pthis);
    for (int channel_idx = 0; channel_idx < mix_frame->channels; channel_idx++)
    {
      for (int sample_idx = 0; sample_idx < mix_frame->nb_samples; sample_idx++)
      {
        out_data[channel_idx][sample_idx] +=
        (float)src_data[channel_idx][src_sample_offset + sample_idx]
        / INT16_MAX;
      }
    }
  }

  return 0;
}

int audio_mixer_get_next(struct audio_mixer_s* pthis, AVFrame** frame_out) {
  auto it = pthis->frame_map.begin();
  if (it == pthis->frame_map.end()) {
    *frame_out = NULL;
    return EAGAIN;
  }
  if (!pthis->last_pts_out) {
    *frame_out = it->second;
    pthis->last_pts_out = it->second->pts;
    pthis->frame_map.erase(it);
  } else if (pthis->last_pts_out + 1 != it->second->pts) {
    // gap in mixed frames vs. next output frame. generate silence i guess?
    // this might be an indication of a deeper issue. too early to tell.
    // As far as I can tell, Chrome just likes to make up approximately
    // accurate timestamps for webm audio pts.
    //
    // You can't make this stuff up, seriously. These are the extracted PTS
    // for a single MediaRecorder output webm, and their samples per packet &
    // self-declared packet durations. Either Chrome is insane or FFmpeg is not
    // agreeing about how to decode the container's frame headers. FROWNY FACE.
    // audio source: extracted pts 58 (58)
    // audio mixer: mix down 60 ms / 2880 samples (58 - 118)
    // audio source: extracted pts 116 (116)
    // audio mixer: mix down 60 ms / 2880 samples (116 - 176)
    // audio source: extracted pts 180 (180)
    // audio mixer: mix down 60 ms / 2880 samples (180 - 240)
    // audio source: extracted pts 237 (237)
    // audio mixer: mix down 60 ms / 2880 samples (237 - 297)
    // audio source: extracted pts 296 (296)
    // audio mixer: mix down 60 ms / 2880 samples (296 - 356)
    // audio source: extracted pts 359 (359)
    // audio mixer: mix down 60 ms / 2880 samples (359 - 419)
    // audio source: extracted pts 417 (417)
    // audio mixer: mix down 60 ms / 2880 samples (417 - 477)
    // audio source: extracted pts 475 (475)
    // audio mixer: mix down 60 ms / 2880 samples (475 - 535)
    pthis->last_pts_out++;
    printf("audio mixer: can't find contiguous mix frame for %lld: "
           "sending silence\n", pthis->last_pts_out);
    *frame_out = frame_for_pts(pthis, pthis->last_pts_out);
    assert((*frame_out)->pts == pthis->last_pts_out);
    pthis->frame_map.erase((*frame_out)->pts);
  } else {
    *frame_out = it->second;
    pthis->last_pts_out = it->second->pts;
    pthis->frame_map.erase(it);
  }
  return 0;
}

int64_t audio_mixer_get_current_ts(struct audio_mixer_s* pthis) {
  return pthis->frame_map.begin()->first;
}

int64_t audio_mixer_get_length(struct audio_mixer_s* pthis) {
  // assumption: timestamp slices are 1ms each. if that changes, this function
  // needs to consider the width of each frame in it's calculation. prefer
  // to keep it constant time if possible.
  return pthis->frame_map.size();
}
