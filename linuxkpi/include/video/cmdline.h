/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <video/cmdline.h> (GPL-2.0; independent declarations): the
 * video= options of the kernel command line. illumos has none:
 * video_get_options() returns NULL for every output name (no forced modes
 * or connector states), video_firmware_drivers_only() false.
 */
#ifndef _VIDEO_CMDLINE_H
#define	_VIDEO_CMDLINE_H

#include <linux/types.h>

static inline const char *
video_get_options(const char *name)
{
	return (NULL);
}

static inline bool
video_firmware_drivers_only(void)
{
	return (false);
}

#endif
