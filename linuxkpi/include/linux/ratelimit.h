/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Rate limiting allows every event (see ratelimit_types.h). */
#ifndef _LINUX_RATELIMIT_H
#define	_LINUX_RATELIMIT_H
#include <linux/ratelimit_types.h>
static inline void
ratelimit_state_init(struct ratelimit_state *rs, int interval, int burst)
{
	rs->interval = interval;
	rs->burst = burst;
}
#define	ratelimit_set_flags(rs, f)	((void)(rs), (void)(f))
#define	__ratelimit(rs)			((void)(rs), 1)
#define	ratelimit_state_exit(rs)	((void)(rs))
#define	RATELIMIT_MSG_ON_RELEASE	1
#endif
