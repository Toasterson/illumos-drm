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
#define	ref_tracker_dir_snprint(d, b, s)	((void)(d), 0)
static inline int
ref_tracker_alloc(struct ref_tracker_dir *dir, struct ref_tracker **tracker,
    gfp_t gfp)
{
	return (0);
}

static inline int
ref_tracker_free(struct ref_tracker_dir *dir, struct ref_tracker **tracker)
{
	return (0);
}
#endif
