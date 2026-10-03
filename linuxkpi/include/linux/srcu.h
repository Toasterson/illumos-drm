/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's sleepable RCU (include/linux/srcu.h, GPL-2.0; reimplemented):
 * each srcu_struct counts its readers like the layer's RCU
 * (<linux/rcupdate.h>), and readers may sleep.
 */
#ifndef _LINUX_SRCU_H
#define	_LINUX_SRCU_H

#include <linux/rcupdate.h>
#include <linux/mutex.h>

struct srcu_struct {
	volatile uint32_t readers;
	kmutex_t lock;
	kcondvar_t cv;
};

#define	DEFINE_SRCU(name)		struct srcu_struct name = { 0 }
#define	DEFINE_STATIC_SRCU(name)	static struct srcu_struct name = { 0 }

extern int init_srcu_struct(struct srcu_struct *ssp);
extern void cleanup_srcu_struct(struct srcu_struct *ssp);
extern int srcu_read_lock(struct srcu_struct *ssp);
extern void srcu_read_unlock(struct srcu_struct *ssp, int idx);
extern void synchronize_srcu(struct srcu_struct *ssp);
#define	synchronize_srcu_expedited(ssp)	synchronize_srcu(ssp)
#define	srcu_read_lock_held(ssp)	1
#define	srcu_barrier(ssp)		synchronize_srcu(ssp)

#endif
