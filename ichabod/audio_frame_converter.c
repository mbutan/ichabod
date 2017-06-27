//
//  audio_frame_converter.c
//  ichabod
//
//  Created by Charley Robinson on 6/8/17.
//

#include <assert.h>
#include <libavutil/audio_fifo.h>

#include "audio_frame_converter.h"

struct frame_converter_s {
  AVAudioFifo* fifo;
  /* output configuration */
  enum AVSampleFormat format;
  int num_channels;
  int samples_per_frame;
  uint64_t channel_layout;
  double sample_rate;
  double next_pts_out;
  double ts_in;
  double ts_offset;
};

void frame_converter_create(struct frame_converter_s** converter_out,
                            struct frame_converter_config_s* config)
{
  struct frame_converter_s* pthis =
  (struct frame_converter_s*)calloc(1, sizeof(struct frame_converter_s));
  pthis->format = config->output_format;
  pthis->num_channels = config->num_channels;
  pthis->samples_per_frame = config->samples_per_frame;
  pthis->sample_rate = config->sample_rate;
  pthis->channel_layout = config->channel_layout;
  pthis->ts_offset = config->pts_offset;
  pthis->fifo = av_audio_fifo_alloc(config->output_format,
                                    config->num_channels,
                                    config->samples_per_frame * 4);
  *converter_out = pthis;
}

void frame_converter_free(struct frame_converter_s* pthis) {
  av_audio_fifo_free(pthis->fifo);
  free(pthis);
}

int frame_converter_consume(struct frame_converter_s* pthis, AVFrame* frame) {
  assert(frame->pts >= pthis->ts_in);
  assert(frame->format == pthis->format);
  pthis->ts_in = frame->pts;
  return av_audio_fifo_write(pthis->fifo,
                             (void**)frame->data,
                             frame->nb_samples);
}

int frame_converter_get_next(struct frame_converter_s* pthis,
                             AVFrame** frame_out)
{
  int buffer_size = av_audio_fifo_size(pthis->fifo);
  if (buffer_size < pthis->samples_per_frame) {
    *frame_out = NULL;
    return EAGAIN;
  }
  AVFrame* frame = av_frame_alloc();
  frame->format = pthis->format;
  frame->nb_samples = pthis->samples_per_frame;
  frame->channels = pthis->num_channels;
  frame->channel_layout = pthis->channel_layout;
  frame->sample_rate = pthis->sample_rate;
  int ret = av_frame_get_buffer(frame, 1);
  if (ret) {
    return ret;
  }
  ret = av_audio_fifo_read(pthis->fifo, (void**)frame->data,
                           pthis->samples_per_frame);
  if (ret == pthis->samples_per_frame) {
    double new_pts = pthis->ts_offset + pthis->next_pts_out;
    new_pts *= pthis->sample_rate;
    frame->pts = new_pts;
    double interval = (double)pthis->samples_per_frame / pthis->sample_rate;
    pthis->next_pts_out += interval;
    *frame_out = frame;
  } else {
    av_frame_free(&frame);
  }
  return pthis->samples_per_frame != ret;
}
