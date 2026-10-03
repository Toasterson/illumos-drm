/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's high-resolution timers (include/linux/hrtimer.h, GPL-2.0;
 * reimplemented in src/lkpi_timer.c on the layer's timer_list). The
 * resolution is illumos' clock tick, not nanoseconds: an hrtimer fires
 * at the first tick at or after its expiry. HRTIMER_RESTART re-arms at
 * the timer's (forwarded) expiry, as on Linux.
 */
#ifndef _LINUX_HRTIMER_H
#define	_LINUX_HRTIMER_H

#include <linux/types.h>
#include <linux/ktime.h>
#include <linux/timer.h>
#include <linux/rbtree.h>

enum hrtimer_mode {
	HRTIMER_MODE_ABS = 0x00,
	HRTIMER_MODE_REL = 0x01,
	HRTIMER_MODE_PINNED = 0x02,
	HRTIMER_MODE_SOFT = 0x04,
	HRTIMER_MODE_HARD = 0x08,
	HRTIMER_MODE_ABS_PINNED = 0x02,
	HRTIMER_MODE_REL_PINNED = 0x03,
	HRTIMER_MODE_ABS_SOFT = 0x04,
	HRTIMER_MODE_REL_SOFT = 0x05,
	HRTIMER_MODE_ABS_HARD = 0x08,
	HRTIMER_MODE_REL_HARD = 0x09
};

enum hrtimer_restart {
	HRTIMER_NORESTART,
	HRTIMER_RESTART
};

#define	CLOCK_REALTIME		0
#define	CLOCK_MONOTONIC		1
#define	CLOCK_BOOTTIME		7

struct hrtimer {
	struct timer_list timer;
	ktime_t expires;
	enum hrtimer_restart (*function)(struct hrtimer *);
	int clock;
	volatile int active;
};

struct hrtimer_sleeper {
	struct hrtimer timer;
	struct task_struct *task;
};

extern void hrtimer_setup(struct hrtimer *t,
    enum hrtimer_restart (*fn)(struct hrtimer *), int clock,
    enum hrtimer_mode mode);
extern void hrtimer_start_range_ns(struct hrtimer *t, ktime_t tim,
    u64 delta_ns, const enum hrtimer_mode mode);
extern int hrtimer_cancel(struct hrtimer *t);
extern int hrtimer_try_to_cancel(struct hrtimer *t);
extern bool hrtimer_active(const struct hrtimer *t);
extern u64 hrtimer_forward(struct hrtimer *t, ktime_t now, ktime_t interval);

#define	hrtimer_init(t, clock, mode)	hrtimer_setup(t, NULL, clock, mode)
#define	hrtimer_setup_on_stack(t, fn, c, m)	hrtimer_setup(t, fn, c, m)
#define	destroy_hrtimer_on_stack(t)	((void)(t))
#define	hrtimer_start(t, tim, mode)	hrtimer_start_range_ns(t, tim, 0, mode)
#define	hrtimer_is_queued(t)		hrtimer_active(t)
#define	hrtimer_callback_running(t)	0
#define	hrtimer_get_expires(t)		((t)->expires)
#define	hrtimer_get_expires_ns(t)	ktime_to_ns((t)->expires)
#define	hrtimer_set_expires(t, tim)	((t)->expires = (tim))
#define	hrtimer_add_expires_ns(t, ns)	((t)->expires += (ns))
#define	hrtimer_get_remaining(t)	ktime_sub((t)->expires, ktime_get())
#define	hrtimer_forward_now(t, i)	hrtimer_forward(t, ktime_get(), i)
#define	hrtimer_cb_get_time(t)		ktime_get()

#endif
