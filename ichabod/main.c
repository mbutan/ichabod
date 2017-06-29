//
//  main.c
//  ichabod
//
//  Created by Charley Robinson on 6/1/17.
//

#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <getopt.h>

#include "ichabod.h"
#include "pulse_audio_source.h"

static struct ichabod_s* ichabod;

void on_signal(int sig) {
  printf("received interrupt\n");
  if (ichabod) {
    ichabod_interrupt(ichabod);
  }
}

int main(int argc, char* const* argv) {
  int ret, c;
  signal(SIGINT, on_signal);

  char* output_path = NULL;
  static struct option long_options[] =
  {
    /* These options set a flag. */
    //{"repairmode", no_argument, &repairmode_flag, 0},
    /* These options don’t set a flag.
     We distinguish them by their indices. */
    {"output", optional_argument,       0, 'o'},
    {0, 0, 0, 0}
  };
  /* getopt_long stores the option index here. */
  int option_index = 0;

  while ((c = getopt_long(argc, argv, "o:",
                          long_options, &option_index)) != -1)
  {
    switch (c)
    {
      case 'o':
        output_path = optarg;
        break;
      case '?':
        if (isprint(optopt))
          fprintf (stderr, "Unknown option `-%c'.\n", optopt);
        else
          fprintf (stderr,
                   "Unknown option character `\\x%x'.\n",
                   optopt);
        return 1;
      default:
        abort ();
    }
  }

  if (!output_path) {
    output_path = "output.mp4";
  }

  ichabod_initialize();
  ichabod_alloc(&ichabod);
  struct ichabod_config_s config;
  config.output_path = output_path;
  ichabod_load_config(ichabod, &config);
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
  usleep(10000);
  return ret;
}
