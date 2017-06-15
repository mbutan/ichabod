//
//  audio_mixer.h
//  ichabod
//
//  Created by Charley Robinson on 6/7/17.
//

#ifndef audio_mixer_h
#define audio_mixer_h

#include <libavformat/avformat.h>
#include <libavutil/frame.h>

/**
 * A staggered input audio mixer. Inputs from multiple tracks with different
 * timelines are merged into a common timeline and made available as a queue.
 */

struct audio_mixer_s;
struct audio_mixer_config_s {
  AVCodecContext* output_codec;
  AVFormatContext* output_format;
  int64_t min_mixdown_delay;
};

void audio_mixer_alloc(struct audio_mixer_s** mixer_out);
int audio_mixer_load_config(struct audio_mixer_s* mixer,
                            struct audio_mixer_config_s* config);
void audio_mixer_free(struct audio_mixer_s* mixer);

int audio_mixer_consume(struct audio_mixer_s* mixer, AVFrame* frame);
int audio_mixer_get_next(struct audio_mixer_s* mixer, AVFrame** frame_out);
int64_t audio_mixer_get_current_ts(struct audio_mixer_s* mixer);
int64_t audio_mixer_get_length(struct audio_mixer_s* mixer);
void audio_mixer_set_frame_size_pts(struct audio_mixer_s* mixer, int size);

#endif /* audio_mixer_h */
