//
//  main_offline.c
//  ichabod
//
//  Created by Charley Robinson on 6/12/17.
//

#include "growing_file_audio_source.h"
#include "audio_mixer.h"
#include "file_writer.h"
#include "audio_frame_converter.h"

int main(void) {
  av_register_all();
  av_register_all();
  avfilter_register_all();

  struct audio_source_s* source;
  audio_source_alloc(&source);
  struct audio_source_config_s source_config;
  source_config.initial_timestamp = 0;
  source_config.path = "/Users/charley/src/horseman/uploads/c91e3082-a898-4dd7-a299-088bceba21a8.webm";
  audio_source_load_config(source, &source_config);
  struct file_writer_t* file_writer;
  file_writer_alloc(&file_writer);
  file_writer_open(file_writer, "/tmp/output.mp4", 640, 480);
  struct audio_mixer_s* mixer;
  audio_mixer_alloc(&mixer);
  struct audio_mixer_config_s mixer_config;
  mixer_config.output_codec = file_writer->audio_ctx_out;
  mixer_config.output_format = file_writer->format_ctx_out;
  audio_mixer_load_config(mixer, &mixer_config);
  AVFrame* frame;
  while (!audio_source_next_frame(source, &frame)) {
    audio_mixer_consume(mixer, frame);
  }
  struct frame_converter_s* aconv;
  struct frame_converter_config_s aconv_config;
  aconv_config.channel_layout = file_writer->audio_ctx_out->channel_layout;
  aconv_config.num_channels = file_writer->audio_ctx_out->channels;
  aconv_config.output_format = file_writer->audio_ctx_out->sample_fmt;
  aconv_config.sample_rate = file_writer->audio_ctx_out->sample_rate;
  aconv_config.samples_per_frame = file_writer->audio_ctx_out->frame_size;
  frame_converter_create(&aconv, &aconv_config);
  while (!audio_mixer_get_next(mixer, &frame)) {
    printf("push mixed frame to converter pts=%lld\n", frame->pts);
    frame_converter_consume(aconv, frame);
  }
  while (!frame_converter_get_next(aconv, &frame)) {
    file_writer_push_audio_frame(file_writer, frame);
  }
  file_writer_close(file_writer);
  return 0;
}
