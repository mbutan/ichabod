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

struct media_queue_s;

struct media_queue_config_s {
  AVFormatContext* format;
  AVCodecContext* audio;
  AVCodecContext* video;
};

int media_queue_create(struct media_queue_s** queue,
                       struct media_queue_config_s* config);
void media_queue_free(struct media_queue_s* queue);

int media_queue_start(struct media_queue_s* queue);
int media_queue_stop(struct media_queue_s* queue);

int media_queue_has_next(struct media_queue_s* queue);
int media_queue_get_next(struct media_queue_s* queue, AVFrame** frame);

#endif /* media_queue_h */
