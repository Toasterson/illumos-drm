/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <video/videomode.h> and <video/display_timing.h> (GPL-2.0;
 * independent declarations), reduced to the types DRM's mode code names:
 * their users are compiled only with CONFIG_VIDEOMODE_HELPERS or CONFIG_OF,
 * both off on illumos.
 */
#ifndef _LKPI_VIDEO_VIDEOMODE_H
#define	_LKPI_VIDEO_VIDEOMODE_H

struct videomode;
struct display_timing;
struct display_timings;
struct device_node;

#endif
