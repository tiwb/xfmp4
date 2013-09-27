#include <mp4v2/mp4v2.h>
#include <faac.h>

extern "C" {
#include <stdint.h>
#include <x264.h>
}

#include <Windows.h>
#include <Shlwapi.h>
#include <math.h>

#pragma comment(lib, "shlwapi.lib")

static void show_error(const char *msg) {
  fprintf(stderr, "%s\n", msg);
}

// color convert
#define rgbtoy(b, g, r, y) \
  y = (unsigned char)(((int)(30 * r) + (int)(59 * g) + (int)(11 * b)) / 100)

#define rgbtouv(b, g, r, u, v) \
  u = (unsigned char)(((int)(-17 * r) - (int)(33 * g) + (int)(50 * b) + 12800) / 100); \
  v = (unsigned char)(((int)(50 * r) - (int)(42 * g) - (int)(8 * b) + 12800) / 100)

typedef struct {
  struct {
    int size_min;
    int next;
    int cnt;
    int idx[17];
    int poc[17];
  } dpb;
  int cnt;
  int cnt_max;
  int *frame;
} h264_dpb_t;

static void dpb_init(h264_dpb_t *p) {
  p->dpb.cnt = 0;
  p->dpb.next = 0;
  p->dpb.size_min = 0;
  p->cnt = 0;
  p->cnt_max = 0;
  p->frame = NULL;
}

static void dpb_clean(h264_dpb_t *p) {
  free(p->frame);
}

static void dpb_update(h264_dpb_t *p, int is_forced) {
  int i;
  int pos;
  if (!is_forced && p->dpb.cnt < 16)
    return;
  /* find the lowest poc */
  pos = 0;
  for (i = 1; i < p->dpb.cnt; i++) {
    if (p->dpb.poc[i] < p->dpb.poc[pos])
      pos = i;
  }
  //fprintf(stderr, "lowest=%d\n", pos);
  /* save the idx */
  if (p->dpb.idx[pos] >= p->cnt_max) {
    int inc = 1000 + (p->dpb.idx[pos] - p->cnt_max);
    p->cnt_max += inc;
    p->frame = (int *)realloc(p->frame, sizeof(int) * p->cnt_max);
    for (i = 0; i < inc; i++)
      p->frame[p->cnt_max - inc + i] = -1;   /* To detect errors latter */
  }
  p->frame[p->dpb.idx[pos]] = p->cnt++;
  /* Update the dpb minimal size */
  if (pos > p->dpb.size_min)
    p->dpb.size_min = pos;
  /* update dpb */
  for (i = pos; i < p->dpb.cnt - 1; i++) {
    p->dpb.idx[i] = p->dpb.idx[i + 1];
    p->dpb.poc[i] = p->dpb.poc[i + 1];
  }
  p->dpb.cnt--;
}

static void dpb_flush(h264_dpb_t *p) {
  while (p->dpb.cnt > 0)
    dpb_update(p, true);
}

static void dpb_add(h264_dpb_t *p, int poc, int is_idr) {
  if (is_idr)
    dpb_flush(p);
  p->dpb.idx[p->dpb.cnt] = p->dpb.next;
  p->dpb.poc[p->dpb.cnt] = poc;
  p->dpb.cnt++;
  p->dpb.next++;

  dpb_update(p, false);
}

static int dpb_frame_offset(h264_dpb_t *p, int idx) {
  if (idx >= p->cnt)
    return 0;
  if (p->frame[idx] < 0)
    return p->dpb.size_min;     /* We have an error (probably broken/truncated bitstream) */
  return p->dpb.size_min + p->frame[idx] - idx;
}

struct mp4_convert_param_t {
  HANDLE  video_input;
  int     video_width;
  int     video_height;
  int     video_framerate;

  HANDLE  audio_input;
  int     audio_samplerate;
};

int mp4_convert(mp4_convert_param_t *convert_args, const char* filename) {

  // temp buffer for vsti process
  const int samples_per_sec = convert_args->audio_samplerate;
  const int frames_per_sec = convert_args->video_framerate;
  const int mp4_time_scale = 90000;

  unsigned int result = 0;
  unsigned long input_samples;
  unsigned long output_size;
  faacEncHandle faac_encoder = NULL;
  x264_t *x264_encoder = NULL;
  unsigned char *faac_buffer = NULL;
  float *input_buffer = NULL;
  MP4FileHandle file = NULL;
  MP4TrackId audio_track = 0;
  MP4TrackId video_track = 0;


  // x264 encoder param
  x264_param_t param;
  x264_param_default(&param);
  x264_param_default_preset(&param, "medium", NULL);
  param.i_csp = X264_CSP_I420;
  param.b_repeat_headers = 0;
  param.i_width = convert_args->video_width;
  param.i_height = convert_args->video_height;

  // create x264 encoder
  x264_picture_t picture;
  x264_picture_init(&picture);
  x264_picture_alloc(&picture, param.i_csp, param.i_width, param.i_height);
  picture.i_type = X264_TYPE_AUTO;
  picture.i_qpplus1 = 0;

  h264_dpb_t h264_dpb;
  dpb_init(&h264_dpb);

  x264_encoder = x264_encoder_open(&param);
  if (!x264_encoder) {
    show_error("Can't create x264 encoder.");
    result = 1;
  }

  if (x264_encoder) {
    x264_encoder_parameters(x264_encoder, &param);
  }

  // create faac encoder.
  faac_encoder = faacEncOpen(samples_per_sec, 2, &input_samples, &output_size);
  if (faac_encoder == NULL) {
    show_error("Failed create faac encoder.");
    result = 1;
  }

  // allocate output buffer
  faac_buffer = new unsigned char[output_size];
  if (faac_buffer == NULL) {
    show_error("Faild allocate buffer");
    result = 1;
  }

  // allocate input buffer
  const uint32_t input_buffer_count = 4;
  uint32_t input_buffer_id = 0;
  input_buffer = new float[input_buffer_count * input_samples];
  if (input_buffer == NULL) {
    show_error("Faild allocate buffer");
    result = 1;
  }

  if (result == 0) {
    if (input_buffer) {
      memset(input_buffer, 0, input_buffer_count * input_samples * sizeof(float));
    }
  }

  // get format.
  if (result == 0) {
    faacEncConfigurationPtr faac_format = faacEncGetCurrentConfiguration(faac_encoder);
    faac_format->inputFormat = FAAC_INPUT_FLOAT;
    faac_format->outputFormat = 0;   //0:RAW
    faac_format->mpegVersion = MPEG4;
    faac_format->aacObjectType = LOW;
    faac_format->allowMidside = 1;
    faac_format->useTns = 0;
    faac_format->useLfe = 0;
    faac_format->quantqual = 100;

    if (!faacEncSetConfiguration(faac_encoder, faac_format)) {
      show_error("Unsupported parameters!");
      result = 1;
    }
  }

  if (result == 0) {
    file = MP4Create(filename);
    if (file == MP4_INVALID_FILE_HANDLE) {
      show_error("Can't create file.");
      result = 1;
    }
  }

  if (result == 0) {
    MP4SetTimeScale(file, mp4_time_scale);
    audio_track = MP4AddAudioTrack(file, samples_per_sec, input_samples / 2, MP4_MPEG4_AUDIO_TYPE);
    MP4SetAudioProfileLevel(file, 0x0F);

    BYTE *ASC = 0;
    DWORD ASCLength = 0;
    faacEncGetDecoderSpecificInfo(faac_encoder, &ASC, &ASCLength);
    MP4SetTrackESConfiguration(file, audio_track, (unsigned __int8 *)ASC, ASCLength);
  }

  if (result == 0) {
    x264_nal_t *nal;
    int i_nal;
    x264_encoder_headers(x264_encoder, &nal, &i_nal);

    uint8_t *sps = nal[0].p_payload;
    video_track = MP4AddH264VideoTrack(file, mp4_time_scale, mp4_time_scale / frames_per_sec, param.i_width, param.i_height,
                                       sps[5], sps[6], sps[7], 3);
    MP4SetVideoProfileLevel(file, 0x7f);
    MP4AddH264SequenceParameterSet(file, video_track, nal[0].p_payload + 4, nal[0].i_payload - 4);
    MP4AddH264PictureParameterSet(file, video_track, nal[1].p_payload + 4, nal[0].i_payload - 4);
  }

  uint32_t frame_count = 0;

  if (result == 0) {
    uint32_t frame_size = input_samples / 2;
    uint32_t delay_samples = frame_size;
    uint32_t total_samples = 0;
    uint32_t encoded_samples = 0;
    uint32_t progress = 0;
    uint32_t finish_wait = frame_size * 100;

    double total_time = 0;
    double capture_time = 0;
    double capture_delta = 1.0 / (double)frames_per_sec;

    for (;; ) {
      float *input_position = input_buffer + input_samples * input_buffer_id;
      input_buffer_id = (input_buffer_id + 1) % input_buffer_count;

      // generate frame data
      for (int samples_left = frame_size; samples_left; ) {
        int samples = samples_left < 32 ? samples_left : 32;
        samples_left -= samples;

        double delta_time = (double)samples / (double)samples_per_sec;

        // clear data
        short temp_buffer[32 * 2];
        memset(input_position, 0, sizeof(temp_buffer));

        // read audio data
        if (convert_args->audio_input != INVALID_HANDLE_VALUE) {
          BOOL success = ReadFile(convert_args->video_input, input_position, samples * sizeof(short), NULL, NULL);
          if (!success) {
            CloseHandle(convert_args->audio_input);
            convert_args->audio_input = INVALID_HANDLE_VALUE;
          }
        }

        // convert audio data to float
        for (int i = 0; i < samples; i++) {
          input_position[0] = temp_buffer[i + 2 + 0] * 32767.0f;
          input_position[1] = temp_buffer[i * 2 + 1] * 32767.0f;
          input_position += 2;
        }

        total_time += delta_time;
        if (total_time > capture_time) {
          x264_nal_t *nal;
          int i_nal;
          int i_frame_size;
          x264_picture_t pic_out;

          // capture image from display
          int width = param.i_width;
          int height = param.i_height;
          size_t size = width * height * 3;
          byte* data = new byte[size];
          memset(data, 0, size);

          // read video data
          if (convert_args->video_input != INVALID_HANDLE_VALUE) {
            BOOL success = ReadFile(convert_args->video_input, data, size, NULL, NULL);
            if (!success) {
              CloseHandle(convert_args->video_input);
              convert_args->video_input = INVALID_HANDLE_VALUE;
            }
          }

          // convert from RGB to yuv
          for (int y = 0; y < height / 2; y++) {
            uint32_t pitch = size / height;
            BYTE *yline = picture.img.plane[0] + y * 2 * picture.img.i_stride[0];
            BYTE *uline = picture.img.plane[1] + y * picture.img.i_stride[1];
            BYTE *vline = picture.img.plane[2] + y * picture.img.i_stride[2];
            BYTE *rgb = (BYTE *)data + pitch * y * 2;

            for (int x = 0; x < width / 2; x++) {
              uint32_t sr = 0, sg = 0, sb = 0;
              rgbtoy(rgb[0], rgb[1], rgb[2], yline[0]); sr += rgb[0]; sg += rgb[1]; sb += rgb[2];
              rgb += pitch; yline += picture.img.i_stride[0];
              rgbtoy(rgb[0], rgb[1], rgb[2], yline[0]); sr += rgb[0]; sg += rgb[1]; sb += rgb[2];
              rgb += 4; yline++;
              rgbtoy(rgb[0], rgb[1], rgb[2], yline[0]); sr += rgb[0]; sg += rgb[1]; sb += rgb[2];
              rgb -= pitch; yline -= picture.img.i_stride[0];
              rgbtoy(rgb[0], rgb[1], rgb[2], yline[0]); sr += rgb[0]; sg += rgb[1]; sb += rgb[2];

              rgb += 4; yline++;
              sr /= 4; sg /= 4; sb /= 4;
              rgbtouv(sr, sg, sb, *uline, *vline);
              uline++;
              vline++;
            }
          }

          delete[] data;

          // encode image to H.264
          i_frame_size = x264_encoder_encode(x264_encoder, &nal, &i_nal, &picture, &pic_out);
          picture.i_pts++;

          for (int nal_id = 0; nal_id < i_nal; ++nal_id) {
            uint32_t size = nal[nal_id].i_payload;
            uint8_t *nalu = nal[nal_id].p_payload;

            nalu[0] = ((size - 4) >> 24) & 0xff;
            nalu[1] = ((size - 4) >> 16) & 0xff;
            nalu[2] = ((size - 4) >> 8) & 0xff;
            nalu[3] = ((size - 4) >> 0) & 0xff;

            // write samples
            MP4Duration dur = mp4_time_scale / frames_per_sec;
            if (!MP4WriteSample(file, video_track, nalu, size, dur, 0, pic_out.b_keyframe != 0)) {
              show_error("Encode mp4 error.");
              goto done;
            }

            bool slice_is_idr = nal[nal_id].i_type == 5;
            dpb_add(&h264_dpb, pic_out.i_pts, slice_is_idr);
            frame_count++;
          }

          capture_time += capture_delta;
        }
      }

      total_samples += frame_size;

      // call the actual encoding routine
      int32_t *src = (int32_t *)input_buffer + input_buffer_id * input_samples;
      int bytes_encoded = faacEncEncode(faac_encoder, src, input_samples, faac_buffer, output_size);
      if (bytes_encoded < 0) {
        show_error("Encode aac error.");
        goto done;
      }

      // write to mp4 stream
      if (bytes_encoded > 0) {
        uint32_t samples_left = total_samples - encoded_samples + delay_samples;
        MP4Duration dur = samples_left > frame_size ? frame_size : samples_left;
        MP4Duration ofs = encoded_samples > 0 ? 0 : delay_samples;

        if (!MP4WriteSample(file, audio_track, faac_buffer, (DWORD)bytes_encoded, dur, ofs, 1)) {
          show_error("Encode mp4 error.");
          goto done;
        }

        encoded_samples += dur;
      }

      // export finished
      if (convert_args->video_input == INVALID_HANDLE_VALUE && 
        convert_args->audio_input == INVALID_HANDLE_VALUE)
      {
        if (finish_wait > frame_size) {
          finish_wait -= frame_size;
        }
        else {
          result = 0;
          break;
        }
      }
    }
  }

done:
  dpb_flush(&h264_dpb);
  if (h264_dpb.dpb.size_min > 0) {
    for (uint32_t ix = 0; ix < frame_count; ix++) {
      const int offset = dpb_frame_offset(&h264_dpb, ix);
      const uint32_t frame_duration = mp4_time_scale / frames_per_sec;
      MP4SetSampleRenderingOffset(file, video_track, 1 + ix, offset * frame_duration);
    }
  }
  dpb_clean(&h264_dpb);

  x264_picture_clean(&picture);
  if (file) {
    MP4Close(file);
    MP4Optimize(filename);
  }
  if (faac_encoder) faacEncClose(faac_encoder);
  if (faac_buffer) delete[] faac_buffer;
  if (input_buffer) delete[] input_buffer;
  if (x264_encoder) x264_encoder_close(x264_encoder);

  return result;
}

static void show_usage() {
  char buff[MAX_PATH];
  GetModuleFileName(NULL, buff, sizeof(buff));
  fprintf(stderr, "Raw to x264-faac-mp4 encoder.\n");
  fprintf(stderr, "  this free program converts raw video input (in RGB format)"
                  "  and audio input (in 16 bit PCM format) to a MP4 file.\n\n");
  fprintf(stderr, "Usage: %s flags\n", PathFindFileName(buff));
  fprintf(stderr, "  --video_input video_filename\n");
  fprintf(stderr, "  --video_width width\n");
  fprintf(stderr, "  --video_height height\n");
  fprintf(stderr, "  --video_framerate framerate. [default: 30]\n");
  fprintf(stderr, "  --audio_input audio_filename\n");
  fprintf(stderr, "  --audio_samplerate samplerate [default: 44100]\n");
  fprintf(stderr, "  --output output_filename\n");
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    show_usage();
    return 0;
  }

  int result = 1;
  const char* output_filename = NULL;

  mp4_convert_param_t param;
  param.audio_input = INVALID_HANDLE_VALUE;
  param.video_input = INVALID_HANDLE_VALUE;
  param.video_width = 640;
  param.video_height = 480;
  param.video_framerate = 30;
  param.audio_samplerate = 44100;

  // prase arguments
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      show_usage();
      goto cleanup;
    }

    if (strcmp(argv[i], "--output") == 0) {
      if (++i >= argc) {
        show_usage();
        goto cleanup;
      }

      output_filename = argv[i];
    }

    if (strcmp(argv[i], "--video_input") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Missing argument for video_input.\n");
        goto cleanup;
      }

      // open file
      const char* filename = argv[i];
      param.video_input = CreateFile(filename, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
      if (param.video_input == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to open video input: %s\n", filename);
        goto cleanup;
      }
    }

    if (strcmp(argv[i], "--audio_input") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Missing argument for audio_input.\n");
        goto cleanup;
      }

      // open file
      const char* filename = argv[i];
      param.audio_input = CreateFile(filename, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
      if (param.audio_input == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to open audio input: %s\n", filename);
        goto cleanup;
      }
    }

    if (strcmp(argv[i], "--video_width") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Missing argument for video_width.\n");
        goto cleanup;
      }
      if (sscanf(argv[i], "%d", &param.video_width) != 1) {
        fprintf(stderr, "Invalid argument for video_width\n");
        goto cleanup;
      }
      if (param.video_width == 0) {
        fprintf(stderr, "video width can not be 0\n");
        goto cleanup;
      }
      if (param.video_width % 4 != 0) {
        fprintf(stderr, "Video width must be muliply of 4\n");
        goto cleanup;
      }
    }

    if (strcmp(argv[i], "--video_height") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Missing argument for video_height.\n");
        goto cleanup;
      }
      if (sscanf(argv[i], "%d", &param.video_height) != 1) {
        fprintf(stderr, "Invalid argument for video_height\n");
        goto cleanup;
      }
      if (param.video_height == 0) {
        fprintf(stderr, "video height can not be 0\n");
        goto cleanup;
      }
      if (param.video_height % 4 != 0) {
        fprintf(stderr, "Video height must be muliply of 4\n");
        goto cleanup;
      }
    }

    if (strcmp(argv[i], "--video_framerate") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Missing argument for video_framerate.\n");
        goto cleanup;
      }
      if (sscanf(argv[i], "%d", &param.video_framerate) != 1) {
        fprintf(stderr, "Missing argument for video_framerate.\n");
        goto cleanup;
      }
      if (param.video_framerate == 0) {
        fprintf(stderr, "Video framerate can not be 0\n");
        goto cleanup;
      }
    }

    if (strcmp(argv[i], "--audio_samplerate") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Missing argument for audio_samplerate.\n");
        goto cleanup;
      }
      if (sscanf(argv[i], "%d", &param.audio_samplerate) != 1) {
        fprintf(stderr, "Invalid argument for audio_samplerate\n");
        goto cleanup;
      }
      if (param.audio_samplerate == 0) {
        fprintf(stderr, "Audio samplerate can not be 0\n");
        goto cleanup;
      }
    }
  }

  if (output_filename == NULL) {
    fprintf(stderr, "No output filename");
    goto cleanup;
  }

  // do convertion
  result = mp4_convert(&param, output_filename);

cleanup:
  // free resources
  if (param.audio_input != INVALID_HANDLE_VALUE) {
    CloseHandle(param.audio_input);
  }
  if (param.video_input != INVALID_HANDLE_VALUE) {
    CloseHandle(param.video_input);
  }

  return result;
}