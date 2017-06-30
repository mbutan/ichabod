//
//  streamer.c
//  ichabod
//
//  Created by Charley Robinson on 6/30/17.
//

#include <assert.h>
#include <uv.h>
#include "streamer.h"

static int streamer_open(struct streamer_s* pthis) {
  int ret;

  if (!(pthis->output_format->flags & AVFMT_NOFILE)) {
    ret = avio_open(&pthis->format_context->pb,
                    pthis->url, AVIO_FLAG_WRITE);
    if (ret < 0) {
      printf("Could not open '%s': %s\n", pthis->url,
              av_err2str(ret));
      return 1;
    }
  }

  pthis->audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  assert(pthis->audio_codec);
  pthis->video_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
  assert(pthis->video_codec);
  pthis->audio_stream = avformat_new_stream(pthis->format_context,
                                            pthis->audio_codec);
  assert(pthis->audio_stream);
  pthis->video_stream = avformat_new_stream(pthis->format_context,
                                            pthis->video_codec);
  assert(pthis->video_stream);

  pthis->video_context = pthis->video_stream->codec;
  pthis->audio_context = pthis->audio_stream->codec;

//  pthis->audio_context = avcodec_alloc_context3(pthis->audio_codec);
//  assert(pthis->audio_context);
//  ret = avcodec_parameters_to_context(pthis->audio_context,
//                                      pthis->audio_stream->codecpar);
//  assert(!ret);
//
//  pthis->video_context = avcodec_alloc_context3(pthis->video_codec);
//  assert(pthis->video_context);
//  ret = avcodec_parameters_to_context(pthis->video_context,
//                                      pthis->video_stream->codecpar);
//  assert(!ret);

  ret = av_opt_set_int(pthis->audio_context, "refcounted_frames", 1, 0);
  ret = av_opt_set_int(pthis->video_context, "refcounted_frames", 1, 0);

  // configure audio codec
  pthis->audio_context->bit_rate = 96000;
  pthis->audio_context->sample_fmt = AV_SAMPLE_FMT_FLTP;
  pthis->audio_context->sample_rate = 48000;
  pthis->audio_context->channels = 1;
  pthis->audio_context->channel_layout = AV_CH_LAYOUT_MONO;

  // configure video codec
  pthis->video_context->qmin = 20;
  /* resolution must be a multiple of two */
  pthis->video_context->width = pthis->width;
  pthis->video_context->height = pthis->height;
  pthis->video_context->pix_fmt = AV_PIX_FMT_YUV420P;
  pthis->video_context->time_base.num = 1;
  pthis->video_context->time_base.den = 1000;

  av_opt_set(pthis->video_context->priv_data, "preset", "veryfast", 0);

  if (pthis->format_context->oformat->flags & AVFMT_GLOBALHEADER) {
    pthis->video_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    pthis->audio_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  av_dump_format(pthis->format_context, 0, pthis->url, 1);

  /* open codec contexts */
  ret = avcodec_open2(pthis->video_context, pthis->video_codec, NULL);
  if (ret < 0) {
    printf("Could not open video codec\n");
    exit(1);
  }

  ret = avcodec_open2(pthis->audio_context, pthis->audio_codec, NULL);
  if (ret < 0) {
    printf("Could not open audio codec\n");
    exit(1);
  }

  /* Write the stream header, if any. */
  ret = avformat_write_header(pthis->format_context, NULL);
  if (ret < 0) {
    fprintf(stderr, "streamer: failed to open output stream: %s\n",
            av_err2str(ret));
    exit(1);
  }

  printf("Ready to stream %s\n", pthis->url);

  return ret;
}

static void streamer_main(void* p) {
  struct streamer_s* pthis = (struct streamer_s*)p;
}

void streamer_alloc(struct streamer_s** streamer_out) {
  struct streamer_s* pthis = (struct streamer_s*)
  calloc(1, sizeof(struct streamer_s));
  int ret;
  ret = avformat_alloc_output_context2(&pthis->format_context, NULL, "flv",
                                       NULL);
  if (ret) {
    printf("streamer: failed to allocate format context\n");
  }
  pthis->output_format = pthis->format_context->oformat;
  uv_mutex_init(&pthis->write_lock);

  *streamer_out = pthis;
}

void streamer_free(struct streamer_s* pthis) {
  uv_mutex_destroy(&pthis->write_lock);
  avformat_free_context(pthis->format_context);
  free(pthis);
}

int streamer_load_config(struct streamer_s* pthis,
                         struct streamer_config_s* config)
{
  pthis->url = config->url;
  pthis->width = config->width;
  pthis->height = config->height;
  return 0;
}

int streamer_start(struct streamer_s* pthis) {
  int ret = streamer_open(pthis);
  return ret;
}

int streamer_stop(struct streamer_s* pthis) {
  return 0;
}

static int safe_write_packet(struct streamer_s* pthis,
                             AVPacket* packet)
{
  uv_mutex_lock(&pthis->write_lock);
  int ret = av_interleaved_write_frame(pthis->format_context, packet);
  uv_mutex_unlock(&pthis->write_lock);
  return ret;
}

int streamer_push_audio(struct streamer_s* pthis, AVFrame* frame) {
  int got_packet, ret;
  AVPacket pkt = { 0 };
  av_init_packet(&pkt);

  /* encode the frame */
  ret = avcodec_encode_audio2(pthis->audio_context,
                              &pkt, frame, &got_packet);
  if (ret < 0) {
    fprintf(stderr, "Error encoding audio frame: %s\n", av_err2str(ret));
    return 1;
  }

  if (got_packet) {
    /* rescale output packet timestamp values from codec to stream timebase */
    av_packet_rescale_ts(&pkt, pthis->audio_context->time_base,
                         pthis->audio_stream->time_base);
    pkt.stream_index = pthis->audio_stream->index;

    /* Write the compressed frame to the media file. */
    printf("streamer: Write audio frame %lld, size=%d pts=%lld "
           "duration=%lld\n",
           pthis->audio_frame_ct, pkt.size, pkt.pts, pkt.duration);
    pthis->audio_frame_ct++;
    ret = safe_write_packet(pthis, &pkt);
    if (ret) {
      printf("failed to write audio packet\n");
    }
  } else {
    ret = 0;
  }
  av_free_packet(&pkt);
  return ret;
}

int streamer_push_video(struct streamer_s* pthis, AVFrame* frame) {
  printf("streamer: push video pts %lld\n", frame->pts);

  int got_packet, ret;
  AVPacket pkt = { 0 };
  av_init_packet(&pkt);

  /* encode the image */
  ret = avcodec_encode_video2(pthis->video_context,
                              &pkt, frame, &got_packet);
  if (ret < 0) {
    fprintf(stderr, "Error encoding video frame: %s\n", av_err2str(ret));
    exit(1);
  }

  if (got_packet) {
    /*
     rescale output packet timestamp values from codec to stream timebase
     */
//    AVRational global_time_base = { 1, 1000 };
//    av_packet_rescale_ts(&pkt, global_time_base,
//                         pthis->video_stream->time_base);
//    pkt.pts *= 96000;
    pkt.stream_index = pthis->video_stream->index;

    /* Write the compressed frame to the media file. */
    printf("file writer: Write video frame %lld, size=%d pts=%lld\n",
           pthis->video_frame_ct, pkt.size, pkt.pts);
    pthis->video_frame_ct++;
    ret = safe_write_packet(pthis, &pkt);
    if (ret) {
      printf("streamer: packet write failure\n");
    }

  } else {
    ret = 0;
  }

  av_packet_unref(&pkt);
  return ret;

}
