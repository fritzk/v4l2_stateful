/*
 * Copyright 2020 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// As per https://www.kernel.org/doc/html/latest/media/uapi/v4l/dev-decoder.html
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dmabuf.h"

static const char* kDRMDevice = "/dev/dri/card1";
static const char* kDecodeDevice = "/dev/video-dec0";
static const int kInputbufferMaxSize = 4 * 1024 * 1024;
static const int kRequestBufferCount = 8;
static const uint32_t kIVFHeaderSignature = v4l2_fourcc('D', 'K', 'I', 'F');

struct mmap_buffers {
  void *start[VIDEO_MAX_PLANES];
  size_t length[VIDEO_MAX_PLANES];
  int fd;
};

struct queue {
  int v4lfd;
  enum v4l2_buf_type type;
  uint32_t fourcc;
  struct mmap_buffers *buffers;
  uint32_t image_width;
  uint32_t image_height;
  uint32_t cnt;
  uint32_t num_planes;
  uint32_t memory;
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

struct compressed_file {
  FILE *fp;
  struct ivf_file_header header;
  uint32_t submitted_frames;
};

void print_fourcc(uint32_t fourcc) {
  printf("%c%c%c%c\n", fourcc & 0xff,
                     fourcc >> 8 & 0xff,
                     fourcc >> 16 & 0xff,
                     fourcc >> 24 & 0xff);
}

struct compressed_file open_file(const char *file_name) {
  struct compressed_file file = {0};

  FILE *fp = fopen(file_name, "rb");
  if (fp) {
    if (fread(&file.header, sizeof(struct ivf_file_header), 1, fp) != 1) {
      fclose(fp);
      fprintf(stderr, "unable to read ivf file header\n");
    }

    if (file.header.signature != kIVFHeaderSignature) {
      fclose(fp);
      fprintf(stderr, "Incorrect header signature : 0x%0x != 0x%0x\n",
        file.header.signature, kIVFHeaderSignature);
    }

    file.fp = fp;
    print_fourcc(file.header.fourcc);
    printf("ivf file header: %d x %d\n", file.header.width, file.header.height);
  } else {
    fprintf(stderr, "unable to open file: %s\n", file_name);
  }

  return file;
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

int capabilities(int v4lfd,
                 uint32_t compressed_format,
                 uint32_t uncompressed_format) {
  struct v4l2_capability cap;
  memset(&cap, 0, sizeof(cap));
  int ret = ioctl(v4lfd, VIDIOC_QUERYCAP, &cap);
  if (ret != 0)
    perror("VIDIOC_QUERYCAP failed");

  printf("driver=\"%s\" bus_info=\"%s\" card=\"%s\" fd=0x%x\n",
         cap.driver, cap.bus_info, cap.card, v4lfd);

  if (!query_format(v4lfd,
                    V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                    compressed_format)) {
    printf("Supported compressed formats:\n");
    query_format(v4lfd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0);
    ret = 1;
  }

  if (!query_format(v4lfd,
                    V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                    uncompressed_format)) {
    printf("Supported uncompressed formats:\n");
    query_format(v4lfd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0);
    ret = 1;
  }

  return ret;
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
    buffer.memory = queue->memory;
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

// this is the input queue that will take compressed data
// 4.5.1.5
int setup_OUTPUT(struct queue *OUTPUT_queue) {
  int ret = 0;

  // 1. Set the coded format on OUTPUT via VIDIOC_S_FMT()
  if (!ret) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    fmt.type = OUTPUT_queue->type;
    fmt.fmt.pix_mp.pixelformat = OUTPUT_queue->fourcc;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = kInputbufferMaxSize;
    fmt.fmt.pix_mp.num_planes = 1;

    int ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_S_FMT, &fmt);
    if (ret != 0)
      perror("VIDIOC_S_FMT failed");
  }

  // 2. Allocate source (bytestream) buffers via VIDIOC_REQBUFS() on OUTPUT.
  if (!ret) {
    struct v4l2_requestbuffers reqbuf;
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = kRequestBufferCount;
    reqbuf.type = OUTPUT_queue->type;
    reqbuf.memory = OUTPUT_queue->memory;

    ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_REQBUFS, &reqbuf);
    if (ret != 0)
      perror("VIDIOC_REQBUFS failed");

    printf("%d buffers requested, %d buffers for compressed data returned\n",
      kRequestBufferCount, reqbuf.count);

    ret = request_mmap_buffers(OUTPUT_queue, &reqbuf);
  }

  // 3. Start streaming on the OUTPUT queue via VIDIOC_STREAMON().
  if (!ret) {
    ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_STREAMON, &OUTPUT_queue->type);
    if (ret != 0)
      perror("VIDIOC_STREAMON failed");
  }

  return ret;
}

int submit_compressed_frame(struct compressed_file *file,
                            struct queue *OUTPUT_queue,
                            uint32_t index) {
  const uint32_t num = file->header.numerator;
  const uint32_t den = file->header.denominator;

  struct ivf_frame_header frame_header = {0};
  if (fread(&frame_header, sizeof(struct ivf_frame_header), 1, file->fp) != 1) {
    if (!feof(file->fp))
      fprintf(stderr, "unable to read ivf frame header\n");
    return -1;
  }

  struct mmap_buffers *buffers = OUTPUT_queue->buffers;
  if (fread(buffers[index].start[0], sizeof(uint8_t), frame_header.size, file->fp) != frame_header.size) {
    fprintf(stderr, "unable to read ivf frame data\n");
    return -1;
  }

  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[VIDEO_MAX_PLANES];
  memset(&v4l2_buffer, 0, sizeof(v4l2_buffer));
  v4l2_buffer.index = index;
  v4l2_buffer.type = OUTPUT_queue->type;
  v4l2_buffer.memory = OUTPUT_queue->memory;
  v4l2_buffer.length = 1;
  v4l2_buffer.timestamp.tv_sec = 0;
  v4l2_buffer.timestamp.tv_usec = ((frame_header.timestamp * den) / num) * 100;
  v4l2_buffer.m.planes = planes;
  v4l2_buffer.m.planes[0].length = buffers[index].length[0];
  v4l2_buffer.m.planes[0].bytesused = frame_header.size;
  v4l2_buffer.m.planes[0].data_offset = 0;
  int ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_QBUF, &v4l2_buffer);
  if (ret != 0) {
    perror("VIDIOC_QBUF failed");
    return -1;
  }

  file->submitted_frames++;

  return 0;
}

int prime_OUTPUT(struct compressed_file *file,
                  struct queue *OUTPUT_queue) {
  int ret = 0;

  for (uint32_t i = 0; i < OUTPUT_queue->cnt; ++i) {
    ret = submit_compressed_frame(file, OUTPUT_queue, i);
    if (ret)
      break;
  }
  return ret;
}

void cleanup_queue(struct queue *queue) {
  if (queue->cnt) {
    struct mmap_buffers *buffers = queue->buffers;

    for (uint32_t i = 0; i < queue->cnt; i++)
      for (uint32_t j = 0; j < queue->num_planes; j++) {
        if (buffers[i].length[j])
          munmap(buffers[i].start[j], buffers[i].length[j]);
        if (buffers[i].fd > 0)
          close(buffers[i].fd);
      }

    free(queue->buffers);
    queue->cnt = 0;
  }
}

int queue_buffer_CAPTURE(struct queue *queue, uint32_t index) {
  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[VIDEO_MAX_PLANES];
  memset(&v4l2_buffer, 0, sizeof v4l2_buffer);
  memset(&planes, 0, sizeof planes);

  v4l2_buffer.type = queue->type;
  v4l2_buffer.memory = queue->memory;
  v4l2_buffer.index = index;
  v4l2_buffer.m.planes = planes;
  v4l2_buffer.length = queue->num_planes;

  for (uint32_t i = 0; i < queue->num_planes; ++i) {
    if (queue->memory == V4L2_MEMORY_DMABUF) {
      v4l2_buffer.m.planes[i].m.fd = queue->buffers[index].fd;
    } else if (queue->memory == V4L2_MEMORY_MMAP) {
      struct mmap_buffers *buffers = queue->buffers;

      v4l2_buffer.m.planes[i].length = buffers[index].length[i];
      v4l2_buffer.m.planes[i].bytesused = buffers[index].length[i];
      v4l2_buffer.m.planes[i].data_offset = 0;
    }
  }

  int ret = ioctl(queue->v4lfd, VIDIOC_QBUF, &v4l2_buffer);
  if (ret != 0) {
    perror("VIDIOC_QBUF failed");
  }

  return ret;
}

// this is the output queue that will produce uncompressed frames
// 4.5.1.6
int setup_CAPTURE(int drm_device_fd, struct queue *CAPTURE_queue, uint32_t use_ubwc) {
  int ret = 0;

  // 1. Call VIDIOC_G_FMT() on the CAPTURE queue to get format for the
  //    destination buffers parsed/decoded from the bytestream.
  if (!ret) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = CAPTURE_queue->type;

    int ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_G_FMT, &fmt);
    if (ret != 0)
      perror("VIDIOC_G_FMT failed");

    CAPTURE_queue->image_width = fmt.fmt.pix_mp.width;
    CAPTURE_queue->image_height = fmt.fmt.pix_mp.height;
    CAPTURE_queue->num_planes = fmt.fmt.pix_mp.num_planes;

    printf("CAPTURE: %d x %d\n", fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);
  }

  // 4. Optional. Set the CAPTURE format via VIDIOC_S_FMT() on the CAPTURE queue.
  //    The client may choose a different format than selected/suggested by the decoder in VIDIOC_G_FMT().
  if (!ret) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = CAPTURE_queue->type;
    fmt.fmt.pix_mp.pixelformat = CAPTURE_queue->fourcc;

    fmt.fmt.pix_mp.width = CAPTURE_queue->image_width;
    fmt.fmt.pix_mp.height = CAPTURE_queue->image_height;

    ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_S_FMT, &fmt);
    if (ret != 0)
      perror("VIDIOC_S_FMT failed");
  }

  // 10. Allocate CAPTURE buffers via VIDIOC_REQBUFS() on the CAPTURE queue.
  if (!ret) {
    struct v4l2_requestbuffers reqbuf;
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = kRequestBufferCount;
    reqbuf.type = CAPTURE_queue->type;
    reqbuf.memory = CAPTURE_queue->memory;

    ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_REQBUFS, &reqbuf);
    if (ret != 0)
      perror("VIDIOC_REQBUFS failed");

    printf("%d buffers requested, %d buffers for decoded data returned\n",
      kRequestBufferCount, reqbuf.count);

    if (CAPTURE_queue->memory == V4L2_MEMORY_DMABUF) {
      const uint32_t buffer_alloc = reqbuf.count * sizeof(struct mmap_buffers);
      struct mmap_buffers *buffers =
        (struct mmap_buffers *)malloc(buffer_alloc);
      memset(buffers, 0, buffer_alloc);
      CAPTURE_queue->buffers = buffers;
      CAPTURE_queue->cnt = reqbuf.count;

      for (uint32_t i = 0; i < CAPTURE_queue->cnt; ++i) {
        const uint32_t width = CAPTURE_queue->image_width;
        const uint32_t height = CAPTURE_queue->image_height;

        CAPTURE_queue->buffers[i].fd =
          bo_create_nv12(drm_device_fd, width, height, use_ubwc);
        if (CAPTURE_queue->buffers[i].fd > 0) {
          ret = queue_buffer_CAPTURE(CAPTURE_queue, i);
          if (ret != 0)
            break;
        } else {
          fprintf(stderr, "could not allocate a dmabuf %d x %d\n", width, height);
          ret = -1;
          break;
        }
      }
    } else if (CAPTURE_queue->memory == V4L2_MEMORY_MMAP) {
      ret = request_mmap_buffers(CAPTURE_queue, &reqbuf);
      for (uint32_t i = 0; i < reqbuf.count; i++) {
        queue_buffer_CAPTURE(CAPTURE_queue, i);
      }
    } else {
      ret = -1;
    }
  }

  // 11. Call VIDIOC_STREAMON() on the CAPTURE queue to start decoding frames.
  if (!ret) {
    ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_STREAMON, &CAPTURE_queue->type);
    if (ret != 0)
      perror("VIDIOC_STREAMON failed");
  }

  return ret;
}

void write_file_to_disk(struct queue *CAPTURE_queue,
                        uint32_t index,
                        uint32_t cnt) {

  char filename[256];
  sprintf(filename, "image_%dx%d_%d.yuv",
    CAPTURE_queue->image_width,
    CAPTURE_queue->image_height, cnt);
  FILE *fp = fopen(filename, "wb");
  if (fp) {
    if (V4L2_MEMORY_DMABUF == CAPTURE_queue->memory) {
      int bo_fd = CAPTURE_queue->buffers[index].fd;
      size_t buffer_size = lseek(bo_fd, 0, SEEK_END);
      lseek(bo_fd, 0, SEEK_SET);

      uint8_t *buffer = (uint8_t *)mmap(0, buffer_size, PROT_READ, MAP_SHARED, bo_fd, 0);

      fwrite(buffer, buffer_size, 1, fp);
      munmap(buffer, buffer_size);
    } else {
      if (CAPTURE_queue->num_planes == 1) {
        size_t buffer_size = (3 * CAPTURE_queue->image_width * CAPTURE_queue->image_height) >> 1;
        uint8_t *buffer = CAPTURE_queue->buffers[index].start[0];
        fwrite(buffer, buffer_size, 1, fp);
      } else {
        for (uint32_t i = 0; i < CAPTURE_queue->num_planes; ++i) {
          size_t buffer_size = (CAPTURE_queue->image_width * CAPTURE_queue->image_height) >> i;
          uint8_t *buffer = CAPTURE_queue->buffers[index].start[i];
          fwrite(buffer, buffer_size, 1, fp);
        }
      }
    }
    fclose(fp);
  } else {
    fprintf(stderr, "Unable to open file: %s\n", filename);
  }
}

int DQBUF(struct queue *queue, uint32_t *index) {
  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
  memset(&v4l2_buffer, 0, sizeof(v4l2_buffer));
  v4l2_buffer.type = queue->type;
  v4l2_buffer.length = queue->num_planes;
  v4l2_buffer.m.planes = planes;
  v4l2_buffer.m.planes[0].bytesused = 0;
  int ret = ioctl(queue->v4lfd, VIDIOC_DQBUF, &v4l2_buffer);

  *index = v4l2_buffer.index;
  return ret;
}

int decode(struct compressed_file *file,
           struct queue *CAPTURE_queue,
           struct queue *OUTPUT_queue,
           uint32_t write_out,
           uint32_t frames_to_decode) {
  int ret = 0;

  if (!ret) {
    uint32_t cnt = 0;
    while (cnt < frames_to_decode) {
      {
        uint32_t index = 0;
        ret = DQBUF(CAPTURE_queue, &index);
        if (ret != 0) {
          if (errno != EAGAIN)
            perror("VIDIOC_DQBUF failed");
          continue;
        }

        if (write_out)
          write_file_to_disk(CAPTURE_queue, index, cnt);

        // Done with buffer, queue it back up.
        ret = queue_buffer_CAPTURE(CAPTURE_queue, index);
      }

      // A frame was recieved on the CAPTURE queue, that means there should
      // now be a free OUTPUT buffer.
      {
        uint32_t index = 0;
        ret = DQBUF(OUTPUT_queue, &index);
        if (ret != 0) {
          if (errno != EAGAIN)
            perror("VIDIOC_DQBUF failed");
          continue;
        }

        if (submit_compressed_frame(file, OUTPUT_queue, index))
          break;
      }
      cnt++;
    }
    printf("%d frames decoded.\n", cnt);
  }

  return ret;
}

static void print_help(const char *argv0)
{
  printf("usage: %s [OPTIONS]\n", argv0);
  printf("  -f, --file        ivf file to decode\n");
  printf("  -w, --write       write out decompressed frames\n");
  printf("  -m, --max         max number of frames to decode\n");
  printf("  -b, --buffer      use mmap instead of dmabuf\n");
  printf("  -o, --output_fmt  fourcc of output format\n");
}

static const struct option longopts[] = {
  { "file", required_argument, NULL, 'f' },
  { "write", no_argument, NULL, 'w' },
  { "max", required_argument, NULL, 'm' },
  { "buffer", no_argument, NULL, 'b' },
  { "output_fmt", no_argument, NULL, 'o' },
  { 0, 0, 0, 0 },
};

int main(int argc, char *argv[]){
  printf("simple v4l2 decode\n");
  int c;
  char *file_name = NULL;
  uint32_t use_ubwc = 0;
  uint32_t write_out = 0;
  uint32_t frames_to_decode = UINT_MAX;
  uint32_t uncompressed_fourcc = v4l2_fourcc('N', 'V', '1', '2');
  uint32_t CAPTURE_memory = V4L2_MEMORY_DMABUF;
  while ((c = getopt_long(argc, argv, "wbm:f:o:", longopts, NULL)) != -1) {
    switch (c) {
      case 'f':
        file_name = strdup(optarg);
        break;
      case 'm':
        frames_to_decode = atoi(optarg);
        printf("only decoding a max of %d frames.\n", frames_to_decode);
        break;
      case 'w':
        write_out = 1;
        break;
      case 'b':
        CAPTURE_memory = V4L2_MEMORY_MMAP;
        break;
      case 'o':
          if (strlen(optarg) == 4) {
            uncompressed_fourcc = v4l2_fourcc(toupper(optarg[0]),
                                              toupper(optarg[1]),
                                              toupper(optarg[2]),
                                              toupper(optarg[3]));
            printf("using (%s) as the CAPTURE format\n", optarg);
            if (uncompressed_fourcc == v4l2_fourcc('Q', '1', '2', '8')) {
              printf("compressed format, setting modifier\n");
              use_ubwc = 1;
            }
          }
        break;
      default:
        break;
    }
  }

  if (!file_name) {
    print_help(argv[0]);
    exit(1);
  }

  struct compressed_file compressed_file = open_file(file_name);
  if (!compressed_file.fp) {
    fprintf(stderr, "Unable to open ivf file: %s\n", file_name);
    exit(EXIT_FAILURE);
  }

  int drm_device_fd = open(kDRMDevice, O_RDWR);

  int v4lfd = open(kDecodeDevice, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (v4lfd < 0) {
    fprintf(stderr, "Unable to open device file: %s\n", kDecodeDevice);
    exit(EXIT_FAILURE);
  }

  if (capabilities(v4lfd, compressed_file.header.fourcc, uncompressed_fourcc) != 0) {
    fprintf(stderr, "Capabilities not present for decode.\n");
    exit(EXIT_FAILURE);
  }

  struct queue OUTPUT_queue = { .v4lfd = v4lfd,
                                .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                .fourcc =  compressed_file.header.fourcc,
                                .num_planes = 1,
                                .memory = V4L2_MEMORY_MMAP};
  int ret = setup_OUTPUT(&OUTPUT_queue);

  if (!ret)
    ret = prime_OUTPUT(&compressed_file, &OUTPUT_queue);

  struct queue CAPTURE_queue = { .v4lfd = v4lfd,
                                 .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                 .fourcc =  uncompressed_fourcc,
                                 .num_planes = 1,
                                 .memory = CAPTURE_memory};
  if (!ret)
    ret = setup_CAPTURE(drm_device_fd, &CAPTURE_queue, use_ubwc);

  if (!ret)
    ret = decode(&compressed_file,
                 &CAPTURE_queue,
                 &OUTPUT_queue,
                 write_out, frames_to_decode);

  cleanup_queue(&OUTPUT_queue);
  cleanup_queue(&CAPTURE_queue);
  close(v4lfd);
  fclose(compressed_file.fp);
  close(drm_device_fd);
  free(file_name);

  return 0;
}
