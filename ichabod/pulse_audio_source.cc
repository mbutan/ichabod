//
//  pulse_audio_source.c
//  ichabod
//
//  Created by Charley Robinson on 6/26/17.
//

extern "C" {
#include <assert.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <uv.h>
#include "pulse_audio_source.h"
}

#include <queue>

struct pulse_s {
  AVInputFormat* input_format;
  AVFormatContext* format_context;
  AVCodecContext* codec_context;
  AVCodec* codec;
  int stream_index;
  AVStream* stream;
  uv_thread_t worker_thread;
  uv_mutex_t queue_lock;
  std::queue<AVFrame*> queue;
  char is_interrupted;
  char is_running;
  int64_t initial_timestamp;
  int64_t last_pts_read;
};

static int pulse_worker_read_frame(struct pulse_s* pthis, AVFrame** frame_out) {
  int ret, got_frame = 0;
  AVPacket packet = { 0 };
  *frame_out = NULL;

  /* pump packet reader until fifo is populated, or file ends */
  while (!got_frame) {
    ret = av_read_frame(pthis->format_context, &packet);
    if (ret < 0) {
      printf("%s\n", av_err2str(ret));
      return ret;
    }

    AVFrame* frame = av_frame_alloc();
    got_frame = 0;
    if (packet.stream_index == pthis->stream_index)
    {
      ret = avcodec_decode_audio4(pthis->codec_context, frame,
                                  &got_frame, &packet);
      if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error decoding audio: %s\n",
               av_err2str(ret));
      }

      if (got_frame) {
        frame->pts = av_frame_get_best_effort_timestamp(frame);
        printf("audio source: extracted pts %lld (diff %lld)\n",
               frame->pts, frame->pts - pthis->last_pts_read);
        pthis->last_pts_read = frame->pts;
        *frame_out = frame;
      }
    } else {
      av_frame_free(&frame);
    }

    av_packet_unref(&packet);
  }

  return !got_frame;

}

static void pulse_worker_main(void* p) {
  int ret;
  AVFrame* frame;
  struct pulse_s* pthis = (struct pulse_s*)p;
  pthis->is_running = 1;
  while (!pthis->is_interrupted) {
    ret = pulse_worker_read_frame(pthis, &frame);
    if (!ret && frame) {
      uv_mutex_lock(&pthis->queue_lock);
      pthis->queue.push(frame);
      uv_mutex_unlock(&pthis->queue_lock);
    }
  }
}

void pulse_alloc(struct pulse_s** pulse_out) {
  struct pulse_s* pthis = (struct pulse_s*) calloc(1, sizeof(struct pulse_s));
  uv_mutex_init(&pthis->queue_lock);
  pthis->queue = std::queue<AVFrame*>();
  pthis->is_interrupted = 0;
  *pulse_out = pthis;
}

void pulse_free(struct pulse_s* pthis) {
  free(pthis);
}

int pulse_open(struct pulse_s* pthis) {
  int ret;
  pthis->input_format = av_find_input_format("pulse");
  if (!pthis->input_format) {
    printf("can't find pulse input format. is it registered?\n");
    return -1;
  }
  ret = avformat_open_input(&pthis->format_context, "4", pthis->input_format, NULL);
  if (ret) {
    printf("failed to open input %s\n", pthis->input_format->name);
    return ret;
  }
  ret = av_find_best_stream(pthis->format_context,
                            AVMEDIA_TYPE_AUDIO,
                            -1, -1,
                            &pthis->codec, 0);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR,
           "Cannot find a video stream in the input file\n");
    return ret;
  }
  pthis->stream_index = ret;
  pthis->stream = pthis->format_context->streams[pthis->stream_index];
//  pthis->codec = avcodec_find_decoder(pthis->stream->codecpar->codec_id);
//  if (!pthis->codec) {
//    printf("Failed to find audio codec\n");
//    return AVERROR(EINVAL);
//  }

  /* Allocate a codec context for the decoder */
  pthis->codec_context = avcodec_alloc_context3(pthis->codec);

  av_opt_set_int(pthis->codec_context, "refcounted_frames", 1, 0);

  assert(pthis->codec->sample_fmts[0] == AV_SAMPLE_FMT_S16);
  pthis->codec_context->request_sample_fmt = pthis->codec->sample_fmts[0];
  // TODO: There's no reason we shouldn't support stereo audio here
  pthis->codec_context->channels = 2;

  /* init the decoder */
  ret = avcodec_open2(pthis->codec_context, pthis->codec, NULL);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot open audio decoder\n");
  }

  uv_thread_create(&pthis->worker_thread, pulse_worker_main, pthis);

  return ret;
}

int pulse_stop(struct pulse_s* pthis) {
  pthis->is_interrupted = 1;
  int ret = uv_thread_join(&pthis->worker_thread);
  pthis->is_running = 0;
  return ret;
}

char pulse_is_running(struct pulse_s* pthis) {
  return pthis->is_running;
}


char pulse_has_next(struct pulse_s* pthis) {
  char ret = 0;
  uv_mutex_lock(&pthis->queue_lock);
  ret = pthis->queue.size() > 0;
  uv_mutex_unlock(&pthis->queue_lock);
  return ret;
}

int pulse_get_next(struct pulse_s* pthis, AVFrame** frame_out) {
  AVFrame* frame = NULL;
  int ret;
  uv_mutex_lock(&pthis->queue_lock);
  if (pthis->queue.empty()) {
    ret = EAGAIN;
  } else {
    frame = pthis->queue.front();
    pthis->queue.pop();
    ret = 0;
  }
  uv_mutex_unlock(&pthis->queue_lock);
  *frame_out = frame;
  return ret;
}

double pulse_get_initial_ts(struct pulse_s* pthis) {
  return (double)pthis->stream->start_time /
  (double)pthis->stream->time_base.den;
}

double pulse_convert_frame_pts(struct pulse_s* pthis, AVFrame* frame) {
  double pts = frame->pts;
  pts /= (double)pthis->stream->time_base.den;
  pts -= pulse_get_initial_ts(pthis);
  return pts;
}
