//
//  frame_generator.h
//  ichabod
//
//  Created by Charley Robinson on 6/1/17.
//

#ifndef frame_generator_h
#define frame_generator_h

#include <libavutil/frame.h>

struct audio_mixer_s;
struct audio_source_s;

int generate_frame(const char* png_base64, AVFrame** frame_out);
int extract_audio(struct audio_source_s* audio_source,
                  struct audio_mixer_s* audio_mixer);

#endif /* frame_generator_h */
