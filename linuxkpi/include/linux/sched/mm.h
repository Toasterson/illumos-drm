/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's allocation-context scopes (include/linux/sched/mm.h, GPL-2.0).
 * illumos has no per-task allocation flags: memalloc_noreclaim/nofs/noio
 * scopes are no-ops (KM_SLEEP allocations in the layer never re-enter the
 * driver), and fs_reclaim annotations are lockdep-only. mmgrab/mmget on
 * the always-NULL mm of a task do nothing.
 */
#ifndef _LINUX_SCHED_MM_H
#define	_LINUX_SCHED_MM_H

#include <linux/sched.h>
#include <linux/gfp.h>

#define	memalloc_noreclaim_save()	0U
#define	memalloc_noreclaim_restore(f)	((void)(f))
#define	memalloc_nofs_save()		0U
#define	memalloc_nofs_restore(f)	((void)(f))
#define	memalloc_noio_save()		0U
#define	memalloc_noio_restore(f)	((void)(f))
#define	memalloc_pin_save()		0U
#define	memalloc_pin_restore(f)		((void)(f))
#define	fs_reclaim_acquire(g)		((void)(g))
#define	fs_reclaim_release(g)		((void)(g))
#define	might_alloc(g)			((void)(g))
#define	current_gfp_context(g)		(g)
#define	mmgrab(mm)			((void)(mm))
#define	mmdrop(mm)			((void)(mm))
#define	mmget(mm)			((void)(mm))
#define	mmget_not_zero(mm)		0
#define	mmput(mm)			((void)(mm))
#define	mmput_async(mm)			((void)(mm))

#endif
