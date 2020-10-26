/*
 * Copyright 2020 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <errno.h>
#include <fcntl.h>
#include <msm_drm.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <xf86drm.h>

#include "dmabuf.h"

#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define ALIGN(A, B) (((A) + (B)-1) & ~((B)-1))
#define DIV_ROUND_UP(n, d) (((n) + (d)-1) / (d))

#define BUFFER_SIZE_ALIGN 4096
#define VENUS_STRIDE_ALIGN 128
#define VENUS_SCANLINE_ALIGN 16
#define NV12_LINEAR_PADDING (12 * 1024)
#define NV12_UBWC_PADDING(y_stride) (MAX(16 * 1024, y_stride * 48))
#define MACROTILE_WIDTH_ALIGN 64
#define MACROTILE_HEIGHT_ALIGN 16
#define PLANE_SIZE_ALIGN 4096

/*
 * Each macrotile consists of m x n (mostly 4 x 4) tiles.
 * Pixel data pitch/stride is aligned with macrotile width.
 * Pixel data height is aligned with macrotile height.
 * Entire pixel data buffer is aligned with 4k(bytes).
 */
static uint32_t get_ubwc_meta_size(uint32_t width, uint32_t height,
                                   uint32_t tile_width, uint32_t tile_height) {
  uint32_t macrotile_width, macrotile_height;

  macrotile_width = DIV_ROUND_UP(width, tile_width);
  macrotile_height = DIV_ROUND_UP(height, tile_height);

  // Align meta buffer width to 64 blocks
  macrotile_width = ALIGN(macrotile_width, MACROTILE_WIDTH_ALIGN);

  // Align meta buffer height to 16 blocks
  macrotile_height = ALIGN(macrotile_height, MACROTILE_HEIGHT_ALIGN);

  return ALIGN(macrotile_width * macrotile_height, PLANE_SIZE_ALIGN);
}

static uint32_t msm_calculate_size(uint32_t width, uint32_t height, uint32_t use_ubwc) {
  uint32_t y_stride, uv_stride, y_scanline, uv_scanline, y_plane, uv_plane, size,
      extra_padding;

  y_stride = ALIGN(width, VENUS_STRIDE_ALIGN);
  uv_stride = ALIGN(width, VENUS_STRIDE_ALIGN);
  y_scanline = ALIGN(height, VENUS_SCANLINE_ALIGN * 2);
  uv_scanline = ALIGN(DIV_ROUND_UP(height, 2),
          VENUS_SCANLINE_ALIGN * (use_ubwc ? 2 : 1));
  y_plane = y_stride * y_scanline;
  uv_plane = uv_stride * uv_scanline;

  if (use_ubwc) {
    y_plane = ALIGN(y_plane, PLANE_SIZE_ALIGN);
    uv_plane = ALIGN(uv_plane, PLANE_SIZE_ALIGN);
    y_plane += get_ubwc_meta_size(width, height, 32, 8);
    uv_plane += get_ubwc_meta_size(width >> 1, height >> 1, 16, 8);
    extra_padding = NV12_UBWC_PADDING(y_stride);
  } else {
    extra_padding = NV12_LINEAR_PADDING;
  }

  size = y_plane + uv_plane + extra_padding;
  return ALIGN(size, BUFFER_SIZE_ALIGN);
}

#ifndef DRM_RDWR
#define DRM_RDWR O_RDWR
#endif

int bo_create_nv12(int drm_fd, uint32_t width, uint32_t height, uint32_t use_ubwc) {
  struct drm_msm_gem_new req = { 0 };
  int ret;
  int fd = -1;

  req.flags = MSM_BO_WC | MSM_BO_SCANOUT;
  req.size = msm_calculate_size(width, height, use_ubwc);

  ret = ioctl(drm_fd, DRM_IOCTL_MSM_GEM_NEW, &req);
  if (ret) {
    fprintf(stderr, "DRM_IOCTL_MSM_GEM_NEW failed with %s\n", strerror(errno));
    return -errno;
  }

  ret = drmPrimeHandleToFD(drm_fd, req.handle, DRM_CLOEXEC | DRM_RDWR, &fd);

  return (ret) ? ret : fd;
}
