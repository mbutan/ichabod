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
  double sample_rate;
  double ts_out;
  double ts_in;
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
  pthis->fifo = av_audio_fifo_alloc(config->output_format,
                                    config->num_channels,
                                    config->samples_per_frame);
  *converter_out = pthis;
}

void frame_converter_free(struct frame_converter_s* pthis) {
  av_audio_fifo_free(pthis->fifo);
  free(pthis);
}

int frame_converter_consume(struct frame_converter_s* pthis, AVFrame* frame) {
  assert(frame->pts > pthis->ts_in);
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
    return EAGAIN;
  }
  AVFrame* frame = av_frame_alloc();
  frame->format = pthis->format;
  frame->nb_samples = pthis->samples_per_frame;
  frame->channels = pthis->num_channels;
  int ret = av_frame_get_buffer(frame, 1);
  if (ret) {
    return ret;
  }
  ret = av_audio_fifo_read(pthis->fifo, (void**)frame->data,
                           pthis->samples_per_frame);
  if (ret == pthis->samples_per_frame) {
    pthis->ts_out += (double)pthis->samples_per_frame / pthis->sample_rate;
    *frame_out = frame;
  } else {
    av_frame_free(&frame);
  }
  return pthis->samples_per_frame == ret;
}
