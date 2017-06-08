//
//  main.c
//  ichabod
//
//  Created by Charley Robinson on 6/1/17.
//

#include <stdio.h>
#include <unistd.h>

#include "media_queue.h"
#include "file_writer.h"

int main(int argc, const char * argv[]) {
  av_register_all();
  av_register_all();
  avfilter_register_all();
  MagickWandGenesis();
  struct media_queue_s* queue;
  // todo: pass this in from horseman
  struct file_writer_t* file_writer;
  int ret = file_writer_alloc(&file_writer);
  ret = file_writer_open(file_writer, "output.mp4", 1280, 720);
  if (ret) {
    printf("unable to open output file\n");
    return ret;
  }
  struct media_queue_config_s config;
  config.format = file_writer->format_ctx_out;
  config.audio = file_writer->audio_ctx_out;
  config.video = file_writer->video_ctx_out;
  ret = media_queue_create(&queue, &config);
  if (ret) {
    printf("could not allocate media queue\n");
    return ret;
  }
  media_queue_start(queue);
  int64_t init_ts = 0;
  int64_t frames_written = 0;
  while (frames_written < 300) {
    while (media_queue_has_next(queue)) {
      AVFrame* frame = NULL;
      ret = media_queue_get_next(queue, &frame);
      if (ret) {
        printf("queue pop failure\n");
        break;
      }
      if (!init_ts) {
        init_ts = frame->pts;
      }
      frame->pts -= init_ts;
      printf("push frame %lld\n", frame->pts);
      file_writer_push_video_frame(file_writer, frame);
      av_frame_free(&frame);
      frames_written++;
    }
    // TODO: move this to an epoll or similar
    usleep(10000);
  }

  file_writer_close(file_writer);
  file_writer_free(file_writer);
  media_queue_stop(queue);
  media_queue_free(queue);

  char cwd[1024];
  printf("%s\n", getcwd(cwd, sizeof(cwd)));
  return 0;
}
