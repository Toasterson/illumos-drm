/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's struct ratelimit_state. The layer does not rate-limit:
 * __ratelimit() always allows the message (documented in linux/printk.h).
 */
#ifndef _LINUX_RATELIMIT_TYPES_H
#define	_LINUX_RATELIMIT_TYPES_H
#include <linux/types.h>
#define	DEFAULT_RATELIMIT_INTERVAL	(5 * HZ)
#define	DEFAULT_RATELIMIT_BURST		10
struct ratelimit_state {
	int interval;
	int burst;
	int printed;
	int missed;
	unsigned long begin;
	unsigned long flags;
};
#define	RATELIMIT_STATE_INIT(name, i, b)	{ .interval = (i), .burst = (b) }
#define	DEFINE_RATELIMIT_STATE(name, i, b) \
	struct ratelimit_state name = RATELIMIT_STATE_INIT(name, i, b)
#endif
