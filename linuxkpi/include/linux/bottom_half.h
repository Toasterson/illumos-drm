/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. No softirqs on illumos: bottom-half masking is a no-op. */
#ifndef _LINUX_BOTTOM_HALF_H
#define	_LINUX_BOTTOM_HALF_H
#define	local_bh_disable()	((void)0)
#define	local_bh_enable()	((void)0)
#define	in_softirq()		0
#define	in_serving_softirq()	0
#endif
