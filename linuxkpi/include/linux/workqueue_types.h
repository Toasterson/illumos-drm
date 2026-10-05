/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's struct work_struct. `data` holds the layer's state bits
 * (pending, running); `wq` is where it was last queued.
 */
#ifndef _LINUX_WORKQUEUE_TYPES_H
#define	_LINUX_WORKQUEUE_TYPES_H

#include <linux/types.h>
#include <linux/lockdep_types.h>

struct work_struct;
struct workqueue_struct;
typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
	atomic_long_t data;
	struct list_head entry;
	work_func_t func;
	struct workqueue_struct *wq;
	struct lockdep_map lockdep_map;
};

#endif
