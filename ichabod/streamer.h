//
//  streamer.h
//  ichabod
//
//  Created by Charley Robinson on 6/30/17.
//

#ifndef streamer_h
#define streamer_h

#include <uv.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>

/**
 * Wrapper for ffmpeg RTMP streaming
 */

struct streamer_s {
  AVFormatContext* format_context;
  AVCodecContext* audio_context;
  AVCodecContext* video_context;
  AVCodec* audio_codec;
  AVCodec* video_codec;
  AVStream* audio_stream;
  AVStream* video_stream;
  AVOutputFormat* output_format;
  const char* url;
  int width;
  int height;
  int64_t video_frame_ct;
  int64_t audio_frame_ct;
  uv_mutex_t write_lock;
};

struct streamer_config_s {
  const char* url;
  int width;
  int height;
};

void streamer_alloc(struct streamer_s** streamer_out);
void streamer_free(struct streamer_s* streamer);
int streamer_load_config(struct streamer_s* streamer,
                         struct streamer_config_s* config);

int streamer_start(struct streamer_s* streamer);
int streamer_stop(struct streamer_s* streamer);

int streamer_push_audio(struct streamer_s* streamer, AVFrame* frame);
int streamer_push_video(struct streamer_s* streamer, AVFrame* frame);

#endif /* streamer_h */
