/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's struct timer_list, with the layer's state (src/lkpi_timer.c)
 * embedded so that arming a timer never allocates: lkpi_node links a
 * pending timer into the layer's tree of pending timers (ordered by
 * expires, then lkpi_seq); lkpi_seq is non-zero exactly while the timer
 * is pending; lkpi_state holds the shut-down flag. `entry` is unused (kept
 * for Linux's layout); `expires` is in jiffies. A zero-filled timer_list
 * (DEFINE_TIMER()) is a valid idle timer.
 */
#ifndef _LINUX_TIMER_TYPES_H
#define	_LINUX_TIMER_TYPES_H

#include <linux/types.h>
#include <sys/avl.h>

struct timer_list {
	struct hlist_node entry;
	unsigned long expires;
	void (*function)(struct timer_list *);
	u32 flags;
	u32 lkpi_state;
	u64 lkpi_seq;
	avl_node_t lkpi_node;
};

#endif
