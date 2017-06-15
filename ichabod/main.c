//
//  main.c
//  ichabod
//
//  Created by Charley Robinson on 6/1/17.
//

#include <unistd.h>

#include "media_queue.h"
#include "file_writer.h"

static char is_interrupted = 0;

void on_signal(int sig) {
  is_interrupted = 1;
}

int main(int argc, const char * argv[]) {
  signal(SIGINT, on_signal);
  av_register_all();
  av_register_all();
  avfilter_register_all();
  MagickWandGenesis();
  struct media_queue_s* queue;
  // todo: pass this in from horseman
  struct file_writer_t* file_writer;
  int ret = file_writer_alloc(&file_writer);
  ret = file_writer_open(file_writer, "output.mp4", 640, 480);
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
  int64_t frames_written = 0;
  while (!is_interrupted) {
    while (media_queue_has_next(queue)) {
      AVFrame* frame = NULL;
      ret = media_queue_get_next(queue, &frame);
      if (ret) {
        printf("queue pop failure\n");
        break;
      }
      if (frame->width && frame->height) {
        file_writer_push_video_frame(file_writer, frame);
      } else if (frame->nb_samples) {
        file_writer_push_audio_frame(file_writer, frame);
      }
      av_frame_free(&frame);
      frames_written++;
    }
    // TODO: move this to an epoll or similar
    usleep(10000);
  }
  media_queue_stop(queue);
  file_writer_close(file_writer);
  file_writer_free(file_writer);
  media_queue_free(queue);

  char cwd[1024];
  printf("%s\n", getcwd(cwd, sizeof(cwd)));
  return 0;
}
