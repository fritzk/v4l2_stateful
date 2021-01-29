/*
 * Copyright 2020 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
//https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-encoder.html
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char* kEncodeDevice = "/dev/video-enc";
static const int kInputbufferMaxSize = 4 * 1024 * 1024;
static const int kRequestBufferCount = 8;
static const uint32_t kIVFHeaderSignature = v4l2_fourcc('D', 'K', 'I', 'F');

struct mmap_buffers {
  void *start[VIDEO_MAX_PLANES];
  size_t length[VIDEO_MAX_PLANES];
  struct gbm_bo *bo;
};

struct queue {
  int v4lfd;
  enum v4l2_buf_type type;
  uint32_t fourcc;
  struct mmap_buffers *buffers;
  uint32_t raw_width;
  uint32_t raw_height;
  uint32_t encoded_width;
  uint32_t encoded_height;
  uint32_t cnt;
  uint32_t frame_cnt;
  uint32_t num_planes;
  uint32_t framerate;
};

struct encoder_cfg {
  uint32_t gop_size;
  uint32_t bitrate;
  enum v4l2_mpeg_video_h264_entropy_mode h264_entropy_mode;
  enum v4l2_mpeg_video_h264_level h264_level;
  enum v4l2_mpeg_video_h264_profile h264_profile;
  enum v4l2_mpeg_video_header_mode header_mode;
  enum v4l2_mpeg_video_bitrate_mode bitrate_mode;
};

struct ivf_file_header {
  uint32_t signature;
  uint16_t version;
  uint16_t header_length;
  uint32_t fourcc;
  uint16_t width;
  uint16_t height;
  uint32_t denominator;
  uint32_t numerator;
  uint32_t frame_cnt;
  uint32_t unused;
} __attribute__((packed));

struct ivf_frame_header {
  uint32_t size;
  uint64_t timestamp;
} __attribute__((packed));


void print_fourcc(uint32_t fourcc) {
  printf("%c%c%c%c\n", fourcc & 0xff,
                     fourcc >> 8 & 0xff,
                     fourcc >> 16 & 0xff,
                     fourcc >> 24 & 0xff);
}

int query_format(int v4lfd,
                 enum v4l2_buf_type type,
                 uint32_t fourcc) {
  struct v4l2_fmtdesc fmtdesc;
  memset(&fmtdesc, 0, sizeof(fmtdesc));

  fmtdesc.type = type;
  while (ioctl(v4lfd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
    if (fourcc == 0)
      print_fourcc(fmtdesc.pixelformat);
    else if (fourcc == fmtdesc.pixelformat)
      return 1;
    fmtdesc.index++;
  }

  return 0;
}

void enumerate_menu(int v4lfd, uint32_t id, uint32_t min, uint32_t max) {
  struct v4l2_querymenu querymenu;
  memset(&querymenu, 0, sizeof(querymenu));

  querymenu.id = id;
  for (querymenu.index = min; querymenu.index <= max; querymenu.index++){
    if (0 == ioctl(v4lfd, VIDIOC_QUERYMENU, &querymenu))
      fprintf(stderr, " %s\n", querymenu.name);
  }
}

int capabilities(int v4lfd,
                 uint32_t OUTPUT_format,
                 uint32_t CAPTURE_format,
                 int verbose_capabilities) {
  struct v4l2_capability cap;
  memset(&cap, 0, sizeof(cap));
  int ret = ioctl(v4lfd, VIDIOC_QUERYCAP, &cap);
  if (ret != 0)
    perror("VIDIOC_QUERYCAP failed");

  printf("driver=\"%s\" bus_info=\"%s\" card=\"%s\" fd=0x%x\n",
         cap.driver, cap.bus_info, cap.card, v4lfd);

  if (!query_format(v4lfd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, OUTPUT_format)) {
    printf("Supported OUTPUT formats:\n");
    query_format(v4lfd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0);
    ret = 1;
  }

  if (!query_format(v4lfd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, CAPTURE_format)) {
    printf("Supported CAPTURE formats:\n");
    query_format(v4lfd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0);
    ret = 1;
  }

  if (verbose_capabilities) {
    struct v4l2_query_ext_ctrl queryctrl;
    memset(&queryctrl, 0, sizeof(queryctrl));

    for (queryctrl.id = V4L2_CID_BASE;
         queryctrl.id < V4L2_CID_LASTP1;
         queryctrl.id++) {

      if (0 == ioctl(v4lfd, VIDIOC_QUERY_EXT_CTRL, &queryctrl)) {
        fprintf(stderr, "control %s : %s\n",
          queryctrl.name,
          queryctrl.flags & V4L2_CTRL_FLAG_DISABLED ? "disabled" : "enabled");
        if (queryctrl.type == V4L2_CTRL_TYPE_MENU)
          enumerate_menu(v4lfd, queryctrl.id, queryctrl.minimum, queryctrl.maximum);
      } else if (errno == EINVAL) {
        continue;
      }
    }

    for (queryctrl.id = V4L2_CID_PRIVATE_BASE;;
         queryctrl.id++) {
      if (0 == ioctl(v4lfd, VIDIOC_QUERY_EXT_CTRL, &queryctrl)) {
        if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
          continue;

        fprintf(stderr, "control %s\n", queryctrl.name);

        if (queryctrl.type == V4L2_CTRL_TYPE_MENU)
          enumerate_menu(v4lfd, queryctrl.id, queryctrl.minimum, queryctrl.maximum);
      } else if (errno == EINVAL) {
        break;
      }
    }

    memset(&queryctrl, 0, sizeof(queryctrl));
    queryctrl.id = V4L2_CTRL_CLASS_MPEG | V4L2_CTRL_FLAG_NEXT_CTRL;
    while (0 == ioctl(v4lfd, VIDIOC_QUERY_EXT_CTRL, &queryctrl)) {
        fprintf(stderr, "control %s\n", queryctrl.name);

        if (queryctrl.type == V4L2_CTRL_TYPE_MENU)
          enumerate_menu(v4lfd, queryctrl.id, queryctrl.minimum, queryctrl.maximum);

        if (V4L2_CTRL_ID2CLASS(queryctrl.id) != V4L2_CTRL_CLASS_MPEG)
            break;
            /* ... */
        queryctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
  }
  return ret;
}

int submit_raw_frame(FILE *fp, struct queue *queue, uint32_t index) {
  struct mmap_buffers *buffers = queue->buffers;

  // read y plane first
  if (queue->raw_width != queue->encoded_width) {
    // This means that the frame will need to be copied over line by line
    fprintf(stderr, "unable to load raw frame! %d != %d\n",
      queue->raw_width, queue->encoded_width);
    return -1;
  }

  // read y plane first
  size_t frame_size = queue->raw_width * queue->raw_height;
  uint8_t *buffer = buffers[index].start[0];

  if (fread(buffer, frame_size, 1, fp) != 1) {
    fprintf(stderr, "unable to read luma frame\n");
    return -1;
  }

  // now read uv
  frame_size >>= 1;
  if (queue->num_planes == 2)
    buffer = buffers[index].start[1];
  else if (queue->num_planes == 1)
    buffer += queue->encoded_width * queue->encoded_height;
  else
    return -1;

  if (fread(buffer, frame_size, 1, fp) != 1) {
    fprintf(stderr, "unable to read chroma frame\n");
    return -1;
  }

  // compute frame timestamp
  const float usec_per_frame = (1.0 / queue->framerate) * 1000000;
  const uint64_t usec_time_stamp = usec_per_frame * queue->frame_cnt;
  const uint64_t tv_sec = usec_time_stamp / 1000000;

  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[VIDEO_MAX_PLANES];
  memset(&v4l2_buffer, 0, sizeof(v4l2_buffer));

  v4l2_buffer.index = index;
  v4l2_buffer.type = queue->type;
  v4l2_buffer.memory = V4L2_MEMORY_MMAP;
  v4l2_buffer.length = queue->num_planes;
  v4l2_buffer.timestamp.tv_sec = tv_sec;
  v4l2_buffer.timestamp.tv_usec = usec_time_stamp - tv_sec;
  v4l2_buffer.sequence = queue->frame_cnt;
  v4l2_buffer.m.planes = planes;
  for (uint32_t i = 0; i < queue->num_planes; ++i) {
    v4l2_buffer.m.planes[i].length = buffers[index].length[i];
    v4l2_buffer.m.planes[i].bytesused = buffers[index].length[i];
    v4l2_buffer.m.planes[i].data_offset = 0;
  }

  int ret = ioctl(queue->v4lfd, VIDIOC_QBUF, &v4l2_buffer);
  if (ret != 0) {
    perror("VIDIOC_QBUF failed");
    return -1;
  }

  queue->frame_cnt++;

  return 0;
}

void cleanup_queue(struct queue *queue) {
  if (queue->cnt) {
    struct mmap_buffers *buffers = queue->buffers;

    for (uint32_t i = 0; i < queue->cnt; i++)
      for (uint32_t j = 0; j < queue->num_planes; j++) {
        munmap(buffers[i].start[j], buffers[i].length[j]);
      }

    free(queue->buffers);
    queue->cnt = 0;
  }
}

int request_mmap_buffers(struct queue *queue,
                         struct v4l2_requestbuffers *reqbuf) {
  const int v4lfd = queue->v4lfd;
  const uint32_t buffer_alloc = reqbuf->count * sizeof(struct mmap_buffers);
  struct mmap_buffers *buffers =
    (struct mmap_buffers *)malloc(buffer_alloc);
  memset(buffers, 0, buffer_alloc);
  queue->buffers = buffers;
  queue->cnt = reqbuf->count;

  int ret;
  for (uint32_t i = 0; i < reqbuf->count; i++) {
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buffer, 0, sizeof(buffer));
    buffer.type = reqbuf->type;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = i;
    buffer.length = queue->num_planes;
    buffer.m.planes = planes;
    ret = ioctl(v4lfd, VIDIOC_QUERYBUF, &buffer);
    if (ret != 0) {
      printf("VIDIOC_QUERYBUF failed: %d\n", ret);
      break;
    }

    for (uint32_t j = 0; j < queue->num_planes; j++) {
      buffers[i].length[j] = buffer.m.planes[j].length;
      buffers[i].start[j] = mmap(NULL, buffer.m.planes[j].length,
                                 PROT_READ | PROT_WRITE, MAP_SHARED,
                                 v4lfd, buffer.m.planes[j].m.mem_offset);
      if (MAP_FAILED == buffers[i].start[j]) {
        fprintf(stderr, "failed to mmap buffer of length(%d) and offset(0x%x)\n",
          buffer.m.planes[j].length, buffer.m.planes[j].m.mem_offset);
      }
    }
  }

  return ret;
}

int queue_CAPTURE_buffer(struct queue *queue, uint32_t index) {
  struct mmap_buffers *buffers = queue->buffers;
  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[VIDEO_MAX_PLANES];
  memset(&v4l2_buffer, 0, sizeof v4l2_buffer);
  memset(&planes, 0, sizeof planes);

  v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  v4l2_buffer.memory = V4L2_MEMORY_MMAP;
  v4l2_buffer.index = index;
  v4l2_buffer.m.planes = planes;
  v4l2_buffer.length = queue->num_planes;

  v4l2_buffer.m.planes[0].length = buffers[index].length[0];
  v4l2_buffer.m.planes[0].bytesused = buffers[index].length[0];
  v4l2_buffer.m.planes[0].data_offset = 0;

  int ret = ioctl(queue->v4lfd, VIDIOC_QBUF, &v4l2_buffer);
  if (ret != 0) {
    perror("VIDIOC_QBUF failed");
  }

  return ret;
}

// 4.5.2.5. Initialization
int Initialization(struct queue *OUTPUT_queue, struct queue *CAPTURE_queue) {
  int ret = 0;

  // 1. Set the coded format on the CAPTURE queue via VIDIOC_S_FMT().
  if (!ret) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.pixelformat = CAPTURE_queue->fourcc;
    fmt.fmt.pix_mp.width = CAPTURE_queue->raw_width;
    fmt.fmt.pix_mp.height = CAPTURE_queue->raw_height;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = kInputbufferMaxSize;
    fmt.fmt.pix_mp.num_planes = 1;

    int ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_S_FMT, &fmt);
    if (ret != 0)
      perror("VIDIOC_S_FMT failed");

    CAPTURE_queue->encoded_width = fmt.fmt.pix_mp.width;
    CAPTURE_queue->encoded_height = fmt.fmt.pix_mp.height;
  }

  // 3. Set the raw source format on the OUTPUT queue via VIDIOC_S_FMT().
  if (!ret) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    fmt.type = OUTPUT_queue->type;
    fmt.fmt.pix_mp.pixelformat = OUTPUT_queue->fourcc;
    fmt.fmt.pix_mp.width = OUTPUT_queue->raw_width;
    fmt.fmt.pix_mp.height = OUTPUT_queue->raw_height;
    fmt.fmt.pix_mp.num_planes = 1;

    int ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_S_FMT, &fmt);
    if (ret != 0)
      perror("VIDIOC_S_FMT failed");

    OUTPUT_queue->encoded_width = fmt.fmt.pix_mp.width;
    OUTPUT_queue->encoded_height = fmt.fmt.pix_mp.height;

    OUTPUT_queue->num_planes = fmt.fmt.pix_mp.num_planes;
  }

  // 4. Set the raw frame interval on the OUTPUT queue via VIDIOC_S_PARM()
  if (!ret) {
   struct v4l2_streamparm parms;
    memset(&parms, 0, sizeof(parms));
    parms.type = OUTPUT_queue->type;
    // Note that we are provided "frames per second" but V4L2 expects "time per
    // frame"; hence we provide the reciprocal of the framerate here.
    parms.parm.output.timeperframe.numerator = 1;
    parms.parm.output.timeperframe.denominator = OUTPUT_queue->framerate;

    ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_S_PARM, &parms);
    if (ret != 0)
      perror("VIDIOC_S_PARAM failed");
  }

  // 6. Optional. Set the visible resolution for the stream metadata via
  //    VIDIOC_S_SELECTION() on the OUTPUT queue if it is desired to be
  //    different than the full OUTPUT resolution.
  if (!ret) {
    struct v4l2_selection selection_arg;
    memset(&selection_arg, 0, sizeof(selection_arg));
    selection_arg.type = OUTPUT_queue->type;
    selection_arg.target = V4L2_SEL_TGT_CROP;
    selection_arg.r.left = 0;
    selection_arg.r.top = 0;
    selection_arg.r.width = OUTPUT_queue->raw_width;
    selection_arg.r.height = OUTPUT_queue->raw_height;

    ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_S_SELECTION, &selection_arg);

    if (ret != 0)
      perror("VIDIOC_S_SELECTION failed");

    // TODO(fritz) : check returned values are same as sent values
  }

  // 7. Allocate buffers for both OUTPUT and CAPTURE via VIDIOC_REQBUFS().
  //    This may be performed in any order.
  if (!ret) {
    struct v4l2_requestbuffers reqbuf;
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = kRequestBufferCount;
    reqbuf.type = OUTPUT_queue->type;
    reqbuf.memory = V4L2_MEMORY_MMAP;

    ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_REQBUFS, &reqbuf);
    if (ret != 0)
      perror("VIDIOC_REQBUFS failed");

    printf("%d buffers requested, %d buffers for uncompressed frames returned\n",
      kRequestBufferCount, reqbuf.count);

    ret = request_mmap_buffers(OUTPUT_queue, &reqbuf);
  }

  if (!ret) {
    struct v4l2_requestbuffers reqbuf;
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = kRequestBufferCount;
    reqbuf.type = CAPTURE_queue->type;
    reqbuf.memory = V4L2_MEMORY_MMAP;

    ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_REQBUFS, &reqbuf);
    if (ret != 0)
      perror("VIDIOC_REQBUFS failed");

    printf("%d buffers requested, %d buffers for compressed frames returned\n",
      kRequestBufferCount, reqbuf.count);

    ret = request_mmap_buffers(CAPTURE_queue, &reqbuf);
    for (uint32_t i = 0; i < reqbuf.count; i++) {
      queue_CAPTURE_buffer(CAPTURE_queue, i);
    }
  }

  return ret;
}

int configure_h264(int v4lfd, struct encoder_cfg *cfg) {
  int ret = 0;
  const int kH264CtrlCnt = 4;

  struct v4l2_ext_control ext_ctrl[kH264CtrlCnt];
  memset(&ext_ctrl, 0, sizeof(ext_ctrl));

  ext_ctrl[0].id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
  ext_ctrl[0].value = cfg->h264_profile;

  ext_ctrl[1].id = V4L2_CID_MPEG_VIDEO_H264_LEVEL;
  ext_ctrl[1].value = cfg->h264_level;

  ext_ctrl[2].id = V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE;
  ext_ctrl[2].value = cfg->h264_entropy_mode;

  ext_ctrl[3].id = V4L2_CID_MPEG_VIDEO_HEADER_MODE;
  ext_ctrl[3].value = cfg->header_mode;

  struct v4l2_ext_controls ext_ctrls;
  memset(&ext_ctrls, 0, sizeof(ext_ctrls));

  ext_ctrls.ctrl_class = V4L2_CTRL_CLASS_MPEG;
  ext_ctrls.count = kH264CtrlCnt;
  ext_ctrls.controls = ext_ctrl;

  ret = ioctl(v4lfd, VIDIOC_S_EXT_CTRLS, &ext_ctrls);

  if (ret != 0)
    perror("VIDIOC_S_EXT_CTRLS failed");

  for (uint32_t i = 0; i < kH264CtrlCnt; ++i)
    ext_ctrl[i].value = 0;

  ret = ioctl(v4lfd, VIDIOC_G_EXT_CTRLS, &ext_ctrls);
  if (ret != 0)
    perror("VIDIOC_G_EXT_CTRLS failed");

  if (ext_ctrl[0].value != cfg->h264_profile)
    fprintf(stderr, "requested profile(%d) was not used, using (%d) instead.\n",
      cfg->h264_profile, ext_ctrl[0].value);

  if (ext_ctrl[1].value != cfg->h264_level)
    fprintf(stderr, "requested level(%d) was not used, using (%d) instead.\n",
      cfg->h264_level, ext_ctrl[1].value);

  if (ext_ctrl[2].value != cfg->h264_entropy_mode)
    fprintf(stderr, "requested entropy mode(%d) was not used, using (%d) instead.\n",
      cfg->h264_entropy_mode, ext_ctrl[2].value);

  if (ext_ctrl[3].value != cfg->header_mode)
    fprintf(stderr, "requested entropy mode(%d) was not used, using (%d) instead.\n",
      cfg->header_mode, ext_ctrl[3].value);

  return ret;
}

int configure_common(int v4lfd, struct encoder_cfg *cfg) {
  int ret = 0;
  const int kCommonCtrlCnt = 5;

  struct v4l2_ext_control ext_ctrl[kCommonCtrlCnt];
  memset(&ext_ctrl, 0, sizeof(ext_ctrl));

  ext_ctrl[0].id = V4L2_CID_MPEG_VIDEO_BITRATE;
  ext_ctrl[0].value = cfg->bitrate;

  ext_ctrl[1].id = V4L2_CID_MPEG_VIDEO_BITRATE_PEAK;
  ext_ctrl[1].value = cfg->bitrate * 2;

  ext_ctrl[2].id = V4L2_CID_MPEG_VIDEO_GOP_SIZE;
  ext_ctrl[2].value = cfg->gop_size;

  ext_ctrl[3].id = V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE;
  ext_ctrl[3].value = 1;

  ext_ctrl[4].id = V4L2_CID_MPEG_VIDEO_BITRATE_MODE;
  ext_ctrl[4].value = cfg->bitrate_mode;

  struct v4l2_ext_controls ext_ctrls;
  memset(&ext_ctrls, 0, sizeof(ext_ctrls));

  ext_ctrls.ctrl_class = V4L2_CTRL_CLASS_MPEG;
  ext_ctrls.count = kCommonCtrlCnt;
  ext_ctrls.controls = ext_ctrl;

  ret = ioctl(v4lfd, VIDIOC_S_EXT_CTRLS, &ext_ctrls);

  if (ret != 0)
    perror("VIDIOC_S_EXT_CTRLS failed");

  for (uint32_t i = 0; i < kCommonCtrlCnt; ++i)
    ext_ctrl[i].value = 0;

  ret = ioctl(v4lfd, VIDIOC_G_EXT_CTRLS, &ext_ctrls);
  if (ret != 0)
    perror("VIDIOC_G_EXT_CTRLS failed");

  if (ext_ctrl[0].value != cfg->bitrate)
    fprintf(stderr, "requested bitrate(%d) was outside of the limit, using (%d) instead.\n",
      cfg->bitrate, ext_ctrl[0].value);

  if (ext_ctrl[1].value != cfg->bitrate * 2)
    fprintf(stderr, "requested bitrate peak(%d) was outside of the limit, using (%d) instead.\n",
      cfg->bitrate * 2, ext_ctrl[1].value);

  if (ext_ctrl[2].value != cfg->gop_size)
    fprintf(stderr, "requested gop size(%d) was not used, using (%d) instead.\n",
      cfg->gop_size, ext_ctrl[2].value);

  if (ext_ctrl[3].value != 1)
    fprintf(stderr, "requested frame rate control (%d) was not used, using (%d) instead.\n",
      1, ext_ctrl[3].value);

  if (ext_ctrl[4].value != cfg->bitrate_mode)
    fprintf(stderr, "requested bitrate mode(%d) was not used, using (%d) instead.\n",
      cfg->bitrate_mode, ext_ctrl[4].value);

  return ret;
}

int DQBUF(struct queue *queue, uint32_t *index, uint32_t *bytesused, uint32_t *data_offset, uint64_t *timestamp) {
  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
  memset(&v4l2_buffer, 0, sizeof(v4l2_buffer));
  v4l2_buffer.type = queue->type;
  v4l2_buffer.length = queue->num_planes;
  v4l2_buffer.m.planes = planes;
  v4l2_buffer.m.planes[0].bytesused = 0;
  int ret = ioctl(queue->v4lfd, VIDIOC_DQBUF, &v4l2_buffer);

  *index = v4l2_buffer.index;
  if (bytesused)
    *bytesused = v4l2_buffer.m.planes[0].bytesused;
  if (data_offset)
    *data_offset = v4l2_buffer.m.planes[0].data_offset;
  if (timestamp)
    *timestamp = v4l2_buffer.timestamp.tv_usec;
  return ret;
}

int encode(FILE *fp_input, struct queue *OUTPUT_queue, struct queue *CAPTURE_queue, uint32_t frames_to_decode){
  int ret = 0;
  char output_file_name[256];
  int use_ivf = 0;

  fprintf(stderr, "encoding\n");

  if (CAPTURE_queue->fourcc == v4l2_fourcc('V', 'P', '8', '0'))
    use_ivf = 1;

  sprintf(output_file_name, "output%s", use_ivf ? ".ivf" : ".h264");
  FILE *fp_output = fopen(output_file_name, "wb");
  if (!fp_output) {
    fprintf(stderr, "unable to write to file: %s\n", output_file_name);
    ret = 1;
  }

  // write header
  if (use_ivf) {
    struct ivf_file_header header;
    header.signature = kIVFHeaderSignature;
    header.version = 0;
    header.header_length = sizeof(struct ivf_file_header);
    header.fourcc = CAPTURE_queue->fourcc;
    header.width = CAPTURE_queue->raw_width;
    header.height = CAPTURE_queue->raw_height;
    // hard coded 30fps
    header.denominator = 30;
    header.numerator = 1;
    header.frame_cnt = frames_to_decode;
    header.unused = 0;

    if (fwrite(&header, sizeof(struct ivf_file_header), 1, fp_output) != 1) {
      fprintf(stderr, "unable to write ivf file header\n");
    }
  }

  if (!ret) {
    // prime input by filling up the OUTPUT queue with raw frames
    for (uint32_t i = 0; i < OUTPUT_queue->cnt; ++i) {
      if (submit_raw_frame(fp_input, OUTPUT_queue, i)) {
        fprintf(stderr, "unable to submit raw frame\n");
        ret = 1;
      }
    }
  }

  if (!ret) {
    ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_STREAMON, &OUTPUT_queue->type);
    if (ret != 0)
      perror("VIDIOC_STREAMON failed on OUTPUT");
  }

  if (!ret) {
    ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_STREAMON, &CAPTURE_queue->type);
    if (ret != 0)
      perror("VIDIOC_STREAMON failed on CAPTURE");
  }

  if (!ret) {
    uint32_t cnt = 0;
    while (cnt < frames_to_decode) {
      // handle CAPTURE queue first
      {
        uint32_t index = 0;
        uint32_t bytesused = 0;
        uint32_t data_offset = 0;
        uint64_t timestamp = 0;

        // first get the newly encoded frame
        ret = DQBUF(CAPTURE_queue, &index, &bytesused, &data_offset, &timestamp);
        if (ret != 0) {
          if (errno != EAGAIN)
            perror("VIDIOC_DQBUF failed");
          continue;
        }

        if (use_ivf) {
          struct ivf_frame_header header;
          header.size = bytesused - data_offset;
          header.timestamp = timestamp;

          if (fwrite(&header, sizeof(struct ivf_frame_header), 1, fp_output) != 1) {
            fprintf(stderr, "unable to write ivf frame header\n");
          }
        }
        fwrite(CAPTURE_queue->buffers[index].start[0] + data_offset, bytesused - data_offset, 1, fp_output);

        // done with the buffer, queue it back up
        queue_CAPTURE_buffer(CAPTURE_queue, index);
      }

      // handle OUTPUT queue second
      {
        uint32_t index = 0;

        ret = DQBUF(OUTPUT_queue, &index, 0, 0, 0);
        if (ret != 0) {
          if (errno != EAGAIN)
            perror("VIDIOC_DQBUF failed");
          continue;
        }

        if (submit_raw_frame(fp_input, OUTPUT_queue, index))
          break;
      }
      cnt++;
    }
  }

  if (fp_output) {
    fclose(fp_output);
  }
  return ret;
}

static void print_help(const char *argv0)
{
  printf("usage: %s [OPTIONS]\n", argv0);
  printf("  -f, --file        nv12 file to encode\n");
  printf("  -w, --width       width of image\n");
  printf("  -h, --height      height of image\n");
  printf("  -m, --max         max number of frames to decode\n");
  printf("  -r, --rate        frames per second\n");
  printf("  -b, --bitrate     bits per second\n");
  printf("  -g, --gop         gop length\n");
  printf("  -c, --codec       codec\n");
  printf("  -v, --verbose     verbose capabilities\n");
  printf("  -i, --buffer_fmt  OUTPUT queue format\n");
}

static const struct option longopts[] = {
  { "file", required_argument, NULL, 'f' },
  { "width", required_argument, NULL, 'w' },
  { "height", required_argument, NULL, 'h' },
  { "max", required_argument, NULL, 'm' },
  { "rate", required_argument, NULL, 'r' },
  { "bitrate", required_argument, NULL, 'b' },
  { "gop", required_argument, NULL, 'g' },
  { "codec", required_argument, NULL, 'c' },
  { "verbose", no_argument, NULL, 'v' },
  { "buffer_fmt", no_argument, NULL, 'i' },
  { 0, 0, 0, 0 },
};

int main(int argc, char *argv[]){
  uint32_t OUTPUT_format = v4l2_fourcc('N', 'V', '1', '2');
  uint32_t CAPTURE_format = v4l2_fourcc('H', '2', '6', '4');
  char *file_name = NULL;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t frames_to_decode = 0;
  uint32_t framerate = 30;
  int verbose_capabilities = 0;
  int c;

  struct encoder_cfg cfg = { .gop_size = 20,
                             .bitrate = 1000,
                             .h264_entropy_mode = V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC,
                             .h264_level = V4L2_MPEG_VIDEO_H264_LEVEL_4_0,
                             .h264_profile = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN,
                             .header_mode = V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE,
                             .bitrate_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_VBR};

  while ((c = getopt_long(argc, argv, "f:w:h:m:r:b:g:c:vzo:", longopts, NULL)) != -1) {
    switch (c) {
      case 'f':
        file_name = strdup(optarg);
        break;
      case 'w':
        width = atoi(optarg);
        break;
      case 'h':
        height = atoi(optarg);
        break;
      case 'm':
        frames_to_decode = atoi(optarg);
        break;
      case 'r':
        framerate = atoi(optarg);
        break;
      case 'b':
        cfg.bitrate = atoi(optarg);
        break;
      case 'g':
        cfg.gop_size = atoi(optarg);
        break;
      case 'c':
        if (strlen(optarg) == 4) {
          CAPTURE_format = v4l2_fourcc(toupper(optarg[0]),
                                       toupper(optarg[1]),
                                       toupper(optarg[2]),
                                       toupper(optarg[3]));
          printf("using (%s) as the codec\n", optarg);
        }
        break;
      case 'v':
          verbose_capabilities = 1;
        break;
      case 'o':
        if (strlen(optarg) == 4) {
          OUTPUT_format = v4l2_fourcc(toupper(optarg[0]),
                                      toupper(optarg[1]),
                                      toupper(optarg[2]),
                                      toupper(optarg[3]));
          printf("using (%s) as the OUTPUT queue buffer format\n", optarg);
        }
        break;
      default:
        break;
    }
  }

  if (!file_name || width == 0 || height == 0) {
    fprintf(stderr, "Invalid parameters!\n");
    print_help(argv[0]);
    exit(1);
  }

  FILE *fp = fopen(file_name, "rb");
  if (!fp) {
    fprintf(stderr, "%s: unable to open file.\n", file_name);
    exit(1);
  }

  if (!frames_to_decode) {
    fseek(fp, 0, SEEK_END);
    uint64_t length = ftell(fp);
    uint32_t frame_size = (3 * width * height) >> 1;
    frames_to_decode = length / frame_size;
    fseek(fp, 0, SEEK_SET);
  }

  fprintf(stderr, "encoding %d frames\n", frames_to_decode);

  int v4lfd = open(kEncodeDevice, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (v4lfd < 0) {
    fprintf(stderr, "Unable to open device file: %s\n", kEncodeDevice);
    exit(EXIT_FAILURE);
  }

  if (capabilities(v4lfd, OUTPUT_format, CAPTURE_format, verbose_capabilities) != 0) {
    fprintf(stderr, "Capabilities not present for encode.\n");
    exit(EXIT_FAILURE);
  }

  struct queue OUTPUT_queue = { .v4lfd = v4lfd,
                                .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                .fourcc =  OUTPUT_format,
                                .raw_width = width,
                                .raw_height = height,
                                .frame_cnt = 0,
                                .num_planes = 1,
                                .framerate = framerate,};

  struct queue CAPTURE_queue = { .v4lfd = v4lfd,
                                 .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                 .fourcc =  CAPTURE_format,
                                 .raw_width = width,
                                 .raw_height = height,
                                 .num_planes = 1,
                                 .framerate = framerate,};

  int ret = Initialization(&OUTPUT_queue, &CAPTURE_queue);

  // not all configurations are supported, so we don't need to track
  // the return value
  if (!ret) {
    configure_common(v4lfd, &cfg);

    if (v4l2_fourcc('H', '2', '6', '4') == CAPTURE_format)
      configure_h264(v4lfd, &cfg);
  }

  if (!ret)
    ret = encode(fp, &OUTPUT_queue, &CAPTURE_queue, frames_to_decode);

  cleanup_queue(&OUTPUT_queue);
  cleanup_queue(&CAPTURE_queue);
  close(v4lfd);

  return 0;
}
