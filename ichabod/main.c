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

static struct ichabod_s* ichabod;

void on_signal(int sig) {
  printf("received interrupt\n");
  if (ichabod) {
    ichabod_interrupt(ichabod);
  }
}

int main(int argc, const char * argv[]) {
  int ret;
  signal(SIGINT, on_signal);
  av_register_all();
  avdevice_register_all();
  avfilter_register_all();
  MagickWandGenesis();

  ichabod_alloc(&ichabod);
  ret = ichabod_start(ichabod);
  if (ret) {
    printf("uh oh! %d\n", ret);
  }
  while (ichabod_is_running(ichabod)) {
    usleep(10000);
  }
  ichabod_free(ichabod);
  ichabod = NULL;
  char cwd[1024];
  printf("%s\n", getcwd(cwd, sizeof(cwd)));
  return ret;
}
