//
//  main.c
//  ichabod
//
//  Created by Charley Robinson on 6/1/17.
//

#include <unistd.h>
#include <signal.h>

#include "ichabod.h"
#include "pulse_audio_source.h"

static char is_interrupted = 0;

void on_signal(int sig) {
  printf("received interrupt\n");
  is_interrupted = 1;
}

int main(int argc, const char * argv[]) {
  int ret;
  signal(SIGINT, on_signal);
  av_register_all();
  avdevice_register_all();
  avfilter_register_all();
  MagickWandGenesis();

//  struct pulse_s* pulse;
//  pulse_alloc(&pulse);
//  pulse_open(pulse);
//  AVFrame* frame = NULL;
//  while (1) {
//    if (pulse_has_next(pulse)) {
//      ret = pulse_get_next(pulse, &frame);
//      printf("yay %lld: num_samples=%d duration=%lld\n",
//             frame->pts, frame->nb_samples, frame->pkt_duration);
//    }
//    usleep(1000);
//  }

  struct ichabod_s* ichabod;
  ichabod_alloc(&ichabod);
  ret = ichabod_main(ichabod);
  if (ret) {
    printf("uh oh! %d\n", ret);
  }
  char cwd[1024];
  printf("%s\n", getcwd(cwd, sizeof(cwd)));
  return 0;
}
