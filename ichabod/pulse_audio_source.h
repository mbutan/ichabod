//
//  pulse_audio_source.h
//  ichabod
//
//  Created by Charley Robinson on 6/26/17.
//

#ifndef pulse_audio_source_h
#define pulse_audio_source_h

#include <libavutil/frame.h>

/**
 * An audio stream source from pulse audio.
 */
struct pulse_s;

void pulse_alloc(struct pulse_s** pulse_out);
void pulse_free(struct pulse_s* pulse);

int pulse_open(struct pulse_s* pulse);
int pulse_close(struct pulse_s* pulse);
char pulse_is_running(struct pulse_s* pulse);

char pulse_has_next(struct pulse_s* pulse);
int pulse_get_next(struct pulse_s* pulse, AVFrame** frame_out);
double pulse_get_initial_ts(struct pulse_s* pulse);
/** Give a real TS (floating point unixtime in seconds) from a frame
 * generated by this instance.
 */
double pulse_convert_frame_pts(struct pulse_s* pulse, AVFrame* frame);

#endif /* pulse_audio_source_h */
