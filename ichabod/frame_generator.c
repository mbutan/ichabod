//
//  frame_generator.c
//  ichabod
//
//  Created by Charley Robinson on 6/1/17.
//

#include <unistd.h>

#include "frame_generator.h"
#include "base64.h"
#include "yuv_rgb.h"
#include "growing_file_audio_source.h"
#include "audio_mixer.h"

#include <libavutil/channel_layout.h>
#include <MagickWand/MagickWand.h>
#include <MagickWand/magick-image.h>

#define RGB_BYTES_PER_PIXEL 3

int generate_frame(const char* png_base64, AVFrame** frame_out) {
  size_t b_length = 0;
  const uint8_t* b_img =
  base64_decode((const unsigned char*)png_base64,
                strlen(png_base64),
                &b_length);
  MagickWand* wand = NewMagickWand();
  MagickBooleanType res = MagickReadImageBlob(wand, b_img, b_length);
  if (!res) {
    printf("unable to read image blob\n");
    return -1;
  }
  size_t width = MagickGetImageWidth(wand);
  size_t height = MagickGetImageHeight(wand);

  AVFrame* frame = av_frame_alloc();
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = (int)width;
  frame->height = (int)height;
  int ret = av_frame_get_buffer(frame, 1);
  if (ret) {
    printf("unable to allocate new avframe\n");
    return ret;
  }

  uint8_t* rgb_buf_out = malloc(RGB_BYTES_PER_PIXEL * width * height);

  // push modified wand back to rgb buffer
  res = MagickExportImagePixels(wand, 0, 0,
                                width,
                                height,
                                "RGB", CharPixel,
                                rgb_buf_out);
  if (!res) {
    printf("unable to export converted image pixels");
    return -1;
  }

  // send contrast_wand off to the frame buffer
  rgb24_yuv420_std(frame->width, frame->height,
                   rgb_buf_out, frame->width * RGB_BYTES_PER_PIXEL,
                   frame->data[0],
                   frame->data[1],
                   frame->data[2],
                   frame->linesize[0],
                   frame->linesize[1], YCBCR_709);
  free(rgb_buf_out);

  *frame_out = frame;

  DestroyMagickWand(wand);
  free((void*)b_img);
  return 0;
}

int extract_audio(struct audio_source_s* audio_source,
                  struct audio_mixer_s* audio_mixer)
{
  int ret = 0;
  AVFrame* frame = NULL;
  while (!ret) {
    ret = audio_source_next_frame(audio_source, &frame);
    if (frame) {
      audio_mixer_consume(audio_mixer, frame);
    }
  }

  return ret;
}
