/*
 * Copyright 2020 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stdint.h>

int bo_create_nv12(int drm_fd, uint32_t width, uint32_t height, uint32_t use_ubwc);
