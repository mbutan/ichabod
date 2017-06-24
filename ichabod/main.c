//
//  main.c
//  ichabod
//
//  Created by Charley Robinson on 6/1/17.
//

#include <unistd.h>
#include <signal.h>

#include "ichabod.h"

static char is_interrupted = 0;

void on_signal(int sig) {
  printf("received interrupt\n");
  is_interrupted = 1;
}

int main(int argc, const char * argv[]) {
  signal(SIGINT, on_signal);
  av_register_all();
  av_register_all();
  avfilter_register_all();
  MagickWandGenesis();

  struct ichabod_s* ichabod;
  ichabod_alloc(&ichabod);
  int ret = ichabod_main(ichabod);
  if (ret) {
    printf("uh oh! %d\n", ret);
  }
  char cwd[1024];
  printf("%s\n", getcwd(cwd, sizeof(cwd)));
  return 0;
}
