/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's reference tracker. Not supported (debugging): the trackers
 * record nothing.
 */
#ifndef _LINUX_REF_TRACKER_H
#define	_LINUX_REF_TRACKER_H
#include <linux/types.h>
#include <linux/gfp.h>
struct ref_tracker;
struct ref_tracker_dir {
	char name[32];
};
#define	ref_tracker_dir_init(d, q, n)	((void)(d))
#define	ref_tracker_dir_exit(d)		((void)(d))
#define	ref_tracker_dir_print(d, n)	((void)(d))
#define	ref_tracker_alloc(d, t, g)	((void)(t), 0)
#define	ref_tracker_free(d, t)		((void)(t), 0)
#endif
