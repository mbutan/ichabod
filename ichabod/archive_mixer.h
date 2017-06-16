//
//  archive_mixer.h
//  ichabod
//
//  Created by Charley Robinson on 6/15/17.
//

#ifndef archive_mixer_h
#define archive_mixer_h

#include <libavutil/frame.h>
#include <libavutil/avutil.h>
#include <libavformat/avformat.h>

/**
 * multi-format mixer generates archive media from horseman data pipe.
 * Inputs are:
 * 1) base64-encoded images with timestamps
 * 2) webm file paths that continuously grow and contain audio needing mixdown
 *
 * Outputs:
 * 1) Constant frame rate video
 * 2) Audio mixdown from multiple sources
 */
struct archive_mixer_s;

struct archive_mixer_config_s {
  double initial_timestamp;
  double min_buffer_time;
  double video_fps_out;
  AVFormatContext* format_out;
  AVCodecContext* audio_ctx_out;
  AVStream* audio_stream_out;
  AVCodecContext* video_ctx_out;
  AVStream* video_stream_out;
};

int archive_mixer_create(struct archive_mixer_s** mixer_out,
                         struct archive_mixer_config_s* config);
void archive_mixer_free(struct archive_mixer_s* mixer);

void archive_mixer_consume_video(struct archive_mixer_s* mixer,
                                 AVFrame* frame, double timestamp);
void archive_mixer_consume_audio(struct archive_mixer_s* mixer,
                                 const char* file_path, double timestamp,
                                 const char* subscriber_id);
char archive_mixer_has_next(struct archive_mixer_s* mixer);
int archive_mixer_get_next(struct archive_mixer_s* mixer, AVFrame** frame_out,
                           enum AVMediaType* media_type);

#endif /* archive_mixer_h */
