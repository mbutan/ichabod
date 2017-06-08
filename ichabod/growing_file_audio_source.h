//
//  growing_file_audio_source.h
//  ichabod
//
//  Created by Charley Robinson on 6/7/17.
//

#ifndef growing_file_audio_source_h
#define growing_file_audio_source_h

/**
 * AVFrame source from a file being continuously written.
 */

#include <libavutil/frame.h>

struct audio_source_s;

struct audio_source_config_s {
  const char* path;
  double initial_timestamp;
};

void audio_source_alloc(struct audio_source_s** audio_source_out);
void audio_source_free(struct audio_source_s* audio_source);
int audio_source_load_config(struct audio_source_s* audio_source,
                             struct audio_source_config_s* config);
/** Caller is responsible for freeing frame_out */
int audio_source_next_frame(struct audio_source_s* audio_source,
                            AVFrame** frame_out);

#endif /* growing_file_audio_source_h */
