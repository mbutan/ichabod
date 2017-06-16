//
//  media_queue.h
//  ichabod
//
//  Created by Charley Robinson on 6/1/17.
//

#ifndef media_queue_h
#define media_queue_h

#include <libavformat/avformat.h>
#include <libavutil/frame.h>

/**
 * Message broker between this process and horseman, via ZMQ.
 */

struct media_queue_s;

struct media_queue_msg_s {
  char* sz_data;
  double timestamp;
  char* sz_sid;
};

struct media_queue_config_s {
  void (*on_video_msg)(struct media_queue_s* queue,
                       struct media_queue_msg_s* msg,
                       void* p);
  void (*on_audio_msg)(struct media_queue_s* queue,
                       struct media_queue_msg_s* msg,
                       void* p);
  void* p;
};

int media_queue_alloc(struct media_queue_s** queue);
void media_queue_load_config(struct media_queue_s* queue,
                             struct media_queue_config_s* config);
void media_queue_free(struct media_queue_s* queue);

int media_queue_start(struct media_queue_s* queue);
int media_queue_stop(struct media_queue_s* queue);

int64_t media_queue_get_quiet_cycles(struct media_queue_s* queue);

#endif /* media_queue_h */
