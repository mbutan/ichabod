//
//  video_factory.h
//  ichabod
//
//  Created by Charley Robinson on 6/28/17.
//

#ifndef video_factory_h
#define video_factory_h

#include <libavutil/frame.h>

// Concurrent processor for video frames
struct video_factory_s;
struct video_job_s {
  const char* image_base64;
  void (*video_job_callback)(AVFrame* frame);
  void* cb_p;
};

void video_factory_alloc(struct video_factory_s** factory_out);
void video_factory_free(struct video_factory_s* factory);

int video_factory_start(struct video_factory_s* factory);
int video_factory_stop(struct video_factory_s* factory);

void video_factory_consume(struct video_factory_s* factory,
                           struct video_job_s* job);

#endif /* video_factory_h */
