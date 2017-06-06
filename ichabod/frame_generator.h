//
//  frame_generator.h
//  ichabod
//
//  Created by Charley Robinson on 6/1/17.
//

#ifndef frame_generator_h
#define frame_generator_h

#include <libavutil/frame.h>

int generate_frame(const char* png_base64, AVFrame** frame_out);

#endif /* frame_generator_h */
