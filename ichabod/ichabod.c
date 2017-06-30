//
//  ichabod.c
//  ichabod
//
//  Created by Charley Robinson on 6/15/17.
//

#include <assert.h>
#include <unistd.h>
#include <libavdevice/avdevice.h>
#include <MagickWand/MagickWand.h>
#include "ichabod.h"
#include "horseman.h"
#include "archive_mixer.h"
#include "frame_generator.h"
#include "file_writer.h"
#include "pulse_audio_source.h"
#include "streamer.h"

struct ichabod_s {
  struct horseman_s* horseman;
  struct archive_mixer_s* mixer;
  struct file_writer_t* file_writer;
  struct pulse_s* pulse_audio;
  uv_thread_t thread;
  char is_running;
  char is_interrupted;
  uv_mutex_t mixer_lock;
  const char* output_path;
  struct streamer_s* streamer;
  char use_streamer;
  int width, height;
};

static int build_output(struct ichabod_s* pthis) {
  int ret;
  if (pthis->use_streamer) {
    struct streamer_config_s streamer_config;
    streamer_config.url = pthis->output_path;
    streamer_config.width = pthis->width;
    streamer_config.height = pthis->height;
    streamer_load_config(pthis->streamer, &streamer_config);
    ret = streamer_start(pthis->streamer);
  } else {
    ret = file_writer_open(pthis->file_writer, pthis->output_path,
                           pthis->width, pthis->height);
  }
  if (ret) {
    printf("ichabod: cannot build output\n");
  }
  return ret;
}

static int build_mixer(struct ichabod_s* pthis, AVFrame* first_video_frame,
                       double initial_timestamp)
{
  int ret;
  pthis->width = first_video_frame->width;
  pthis->height = first_video_frame->height;

  ret = build_output(pthis);
  if (ret) {
    return ret;
  }
  struct archive_mixer_config_s mixer_config;
  mixer_config.min_buffer_time = 2; // does this need to be configurable?
  mixer_config.video_fps_out = 30; // this too?
  if (pthis->use_streamer) {
    mixer_config.audio_ctx_out = pthis->streamer->audio_context;
    mixer_config.audio_stream_out = pthis->streamer->audio_stream;
    mixer_config.format_out = pthis->streamer->format_context;
    mixer_config.video_ctx_out = pthis->streamer->video_context;
    mixer_config.video_stream_out = pthis->streamer->video_stream;
  } else {
    mixer_config.audio_ctx_out = pthis->file_writer->audio_ctx_out;
    mixer_config.audio_stream_out = pthis->file_writer->audio_stream;
    mixer_config.format_out = pthis->file_writer->format_ctx_out;
    mixer_config.video_ctx_out = pthis->file_writer->video_ctx_out;
    mixer_config.video_stream_out = pthis->file_writer->video_stream;
  }
  mixer_config.initial_timestamp = initial_timestamp;
  mixer_config.pulse_audio = pthis->pulse_audio;
  ret = archive_mixer_create(&pthis->mixer, &mixer_config);
  if (ret) {
    printf("ichabod: cannot build mixer\n");
    return ret;
  }
  ret = pulse_start(pthis->pulse_audio);
  if (ret) {
    printf("failed to open pulse audio! ichabod will be silent.\n");
  }
  return ret;
}

static void on_audio_data(struct pulse_s* pulse, void* p) {
  struct ichabod_s* pthis = (struct ichabod_s*)p;
  // wait for video callback to create the mixer
  if (!pthis->mixer) {
    return;
  }
  archive_mixer_drain_audio(pthis->mixer);
}

static void on_video_msg(struct horseman_s* queue,
                         struct horseman_msg_s* msg, void* p)
{
  struct ichabod_s* pthis = (struct ichabod_s*)p;
  AVFrame* frame = NULL;
  int ret = generate_frame(msg->sz_data, &frame);
  if (ret) {
    printf("unable to extract frame from video message\n");
    return;
  }
  // This function runs on many threads, concurrently. The archive mixer
  // knows how to reorder frames, but video_frame_buffer definitely does not.
  // If you notice out-of-order video frames arriving in the archive, this
  // needs to be fixed to enforce ordering. (tip: the consume video function
  // can run on a single thread without any hassle).
  uv_mutex_lock(&pthis->mixer_lock);
  if (!pthis->mixer) {
    ret = build_mixer(pthis, frame, msg->timestamp);
    assert(!ret);
  }
  archive_mixer_consume_video(pthis->mixer, frame, msg->timestamp);
  uv_mutex_unlock(&pthis->mixer_lock);
}

static void on_audio_msg(struct horseman_s* queue,
                         struct horseman_msg_s* msg, void* p)
{
  struct ichabod_s* pthis = (struct ichabod_s*)p;
  archive_mixer_consume_audio(pthis->mixer, msg->sz_data,
                              msg->timestamp, msg->sz_sid);
}

void ichabod_initialize() {
  av_register_all();
  avformat_network_init();
  avdevice_register_all();
  avfilter_register_all();
  MagickWandGenesis();
}

void ichabod_alloc(struct ichabod_s** pout) {
  struct ichabod_s* pthis = (struct ichabod_s*)
  calloc(1, sizeof(struct ichabod_s));
  file_writer_alloc(&pthis->file_writer);
  horseman_alloc(&pthis->horseman);
  struct horseman_config_s horseman_config;
  horseman_config.on_audio_msg = on_audio_msg;
  horseman_config.on_video_msg = on_video_msg;
  horseman_config.p = pthis;
  horseman_load_config(pthis->horseman, &horseman_config);

  streamer_alloc(&pthis->streamer);

  pulse_alloc(&pthis->pulse_audio);
  struct pulse_config_s pulse_config = {0};
  pulse_config.on_audio_data = on_audio_data;
  pulse_config.audio_data_cb_p = pthis;
  pulse_load_config(pthis->pulse_audio, &pulse_config);
  uv_mutex_init(&pthis->mixer_lock);
  *pout = pthis;
}

void ichabod_free(struct ichabod_s* pthis) {
  horseman_free(pthis->horseman);
  uv_mutex_destroy(&pthis->mixer_lock);
  file_writer_free(pthis->file_writer);
  archive_mixer_free(pthis->mixer);
  pthis->mixer = NULL;
  free(pthis);
}

void ichabod_load_config(struct ichabod_s* pthis,
                         struct ichabod_config_s* config)
{
  pthis->output_path = config->output_path;
  if (!strncmp(pthis->output_path, "rtmp", 4)) {
    printf("output path looks like an rtmp url. will attempt to stream\n");
    pthis->use_streamer = 1;
  } else {
    pthis->use_streamer = 0;
  }
}


// Run ichabod_main cycles so long as data is queued to write AND 
static inline char should_try_cycle(struct ichabod_s* pthis) {
  return (pthis->mixer && archive_mixer_has_next(pthis->mixer)) ||
  !pthis->is_interrupted;
}

static void ichabod_main(void* p) {
  struct ichabod_s* pthis = (struct ichabod_s*)p;
  horseman_start(pthis->horseman);
  int quiet_cycles = 0;
  while (should_try_cycle(pthis) && quiet_cycles < 1000) {
    while (pthis->mixer && archive_mixer_has_next(pthis->mixer)) {
      quiet_cycles = 0;
      AVFrame* frame = NULL;
      enum AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;
      int ret = archive_mixer_get_next(pthis->mixer, &frame, &media_type);
      if (ret || !frame) {
        continue;
      }
      if (AVMEDIA_TYPE_VIDEO == media_type) {
        //file_writer_push_video_frame(pthis->file_writer, frame);
        streamer_push_video(pthis->streamer, frame);
      } else if (AVMEDIA_TYPE_AUDIO == media_type) {
        //file_writer_push_audio_frame(pthis->file_writer, frame);
        streamer_push_audio(pthis->streamer, frame);
      }
      av_frame_free(&frame);

      printf("ichabod: %zu frames estimated remaining in queue\n",
             archive_mixer_get_size(pthis->mixer));
    }
    quiet_cycles++;
    usleep(10000);
  }
  printf("ichabod main complete\n");
  horseman_stop(pthis->horseman);
  if (pthis->use_streamer) {

  } else {
    file_writer_close(pthis->file_writer);
  }
  pthis->is_running = 0;
}

int ichabod_start(struct ichabod_s* pthis) {
  int ret = uv_thread_create(&pthis->thread, ichabod_main, pthis);
  if (!ret) {
    pthis->is_running = 1;
  }
  return ret;
}

void ichabod_interrupt(struct ichabod_s* pthis) {
  // cut off flows of audio and video. This begins the flush of all queued media
  // out to archive before ichabod_main exits.
  pulse_stop(pthis->pulse_audio);
  pthis->is_interrupted = 1;
}

char ichabod_is_running(struct ichabod_s* pthis) {
  return pthis->is_running;
}
