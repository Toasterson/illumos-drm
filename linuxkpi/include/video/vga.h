/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <video/vga.h> (no licence notice; independent declarations of
 * the standard VGA register ports and bits, which are hardware facts),
 * reduced to what i915's VGA plane and CRT code use.
 */
#ifndef __linux_video_vga_h__
#define	__linux_video_vga_h__

#include <linux/types.h>

#define	VGA_MIS_W	0x3c2	/* miscellaneous output, write */
#define	VGA_IS0_R	0x3c2	/* input status 0, read */
#define	VGA_SEQ_I	0x3c4	/* sequencer index */
#define	VGA_SEQ_D	0x3c5	/* sequencer data */
#define	VGA_MIS_R	0x3cc	/* miscellaneous output, read */

#define	VGA_MIS_COLOR		0x01	/* colour (0x3dx) I/O addresses */
#define	VGA_MIS_ENB_MEM_ACCESS	0x02	/* CPU access to the frame buffer */

#define	VGA_SR01_SCREEN_OFF	0x20	/* clocking mode: screen off */

#endif
