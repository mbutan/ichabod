//
//  ichabod.c
//  ichabod
//
//  Created by Charley Robinson on 6/15/17.
//

#include <assert.h>
#include <unistd.h>
#include "ichabod.h"
#include "media_queue.h"
#include "archive_mixer.h"
#include "frame_generator.h"
#include "file_writer.h"

struct ichabod_s {
  struct media_queue_s* media_queue;
  struct archive_mixer_s* mixer;
  struct file_writer_t* file_writer;

};

static int build_mixer(struct ichabod_s* pthis, AVFrame* first_video_frame,
                       double initial_timestamp)
{
  int ret = file_writer_open(pthis->file_writer, "output.mp4",
                             first_video_frame->width,
                             first_video_frame->height);
  if (ret) {
    printf("ichabod: cannot build file output\n");
    return ret;
  }
  struct archive_mixer_config_s mixer_config;
  mixer_config.min_buffer_time = 2; // does this need to be configurable?
  mixer_config.video_fps_out = 30; // this too?
  mixer_config.audio_ctx_out = pthis->file_writer->audio_ctx_out;
  mixer_config.audio_stream_out = pthis->file_writer->audio_stream;
  mixer_config.format_out = pthis->file_writer->format_ctx_out;
  mixer_config.video_ctx_out = pthis->file_writer->video_ctx_out;
  mixer_config.video_stream_out = pthis->file_writer->video_stream;
  mixer_config.initial_timestamp = initial_timestamp;
  ret = archive_mixer_create(&pthis->mixer, &mixer_config);
  if (ret) {
    printf("ichabod: cannot build mixer\n");
    return ret;
  }
  return ret;
}

static void on_video_msg(struct media_queue_s* queue,
                         struct media_queue_msg_s* msg, void* p)
{
  struct ichabod_s* pthis = (struct ichabod_s*)p;
  AVFrame* frame = NULL;
  int ret = generate_frame(msg->sz_data, &frame);
  if (ret) {
    printf("unable to extract frame from video message\n");
    return;
  }
  if (!pthis->mixer) {
    build_mixer(pthis, frame, msg->timestamp);
  }
  archive_mixer_consume_video(pthis->mixer, frame, msg->timestamp);
}

static void on_audio_msg(struct media_queue_s* queue,
                         struct media_queue_msg_s* msg, void* p)
{
  struct ichabod_s* pthis = (struct ichabod_s*)p;
  archive_mixer_consume_audio(pthis->mixer, msg->sz_data,
                              msg->timestamp, msg->sz_sid);
}

void ichabod_alloc(struct ichabod_s** pout) {
  struct ichabod_s* pthis = (struct ichabod_s*)
  calloc(1, sizeof(struct ichabod_s));
  file_writer_alloc(&pthis->file_writer);
  media_queue_alloc(&pthis->media_queue);
  struct media_queue_config_s media_queue_config;
  media_queue_config.on_audio_msg = on_audio_msg;
  media_queue_config.on_video_msg = on_video_msg;
  media_queue_config.p = pthis;
  media_queue_load_config(pthis->media_queue, &media_queue_config);
  *pout = pthis;
}

void ichabod_free(struct ichabod_s* pthis) {
  media_queue_free(pthis->media_queue);
  file_writer_free(pthis->file_writer);
  archive_mixer_free(pthis->mixer);
  free(pthis);
}

int ichabod_main(struct ichabod_s* pthis) {
  media_queue_start(pthis->media_queue);
  int quiet_cycles = 0;
  while (quiet_cycles < 2000) {
    while (pthis->mixer && archive_mixer_has_next(pthis->mixer)) {
      quiet_cycles = 0;
      AVFrame* frame = NULL;
      enum AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;
      int ret = archive_mixer_get_next(pthis->mixer, &frame, &media_type);
      if (ret || !frame) {
        continue;
      }
      if (AVMEDIA_TYPE_VIDEO == media_type) {
        file_writer_push_video_frame(pthis->file_writer, frame);
      } else if (AVMEDIA_TYPE_AUDIO == media_type) {
        file_writer_push_audio_frame(pthis->file_writer, frame);
      }
      av_frame_free(&frame);
    }
    quiet_cycles++;
    usleep(10000);
  }
  printf("ichabod main complete\n");
  media_queue_stop(pthis->media_queue);
  file_writer_close(pthis->file_writer);
  return 0;
}
