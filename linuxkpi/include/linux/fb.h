/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <linux/fb.h>, reduced: illumos-drm has no fbdev (no
 * CONFIG_FB/CONFIG_DRM_FBDEV_EMULATION). Only the pixel-clock conversion
 * DRM's mode code uses: KHZ2PICOS(k) and PICOS2KHZ(p) convert between a
 * clock in kHz and a period in picoseconds (10^9 / x, both ways).
 */
#ifndef _LINUX_FB_H
#define	_LINUX_FB_H

#include <linux/types.h>

#define	KHZ2PICOS(a)	(1000000000UL / (a))
#define	PICOS2KHZ(a)	(1000000000UL / (a))

struct fb_info;
struct fb_var_screeninfo;
struct fb_videomode;

#endif
