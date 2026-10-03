/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. signal_pending() is in <linux/sched.h>. */
#ifndef _LINUX_SCHED_SIGNAL_H
#define	_LINUX_SCHED_SIGNAL_H
#include <linux/sched.h>
#define	send_sig(sig, t, priv)	((void)(sig), (void)(t), 0)
#define	kill_pid(pid, sig, priv)	0
#endif
