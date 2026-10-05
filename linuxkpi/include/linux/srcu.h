/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's sleepable RCU (include/linux/srcu.h, GPL-2.0; reimplemented):
 * each srcu_struct counts its readers like the layer's RCU
 * (<linux/rcupdate.h>), and readers may sleep (src/lkpi_rcu.c). A
 * zero-filled srcu_struct (DEFINE_SRCU()) is valid.
 */
#ifndef _LINUX_SRCU_H
#define	_LINUX_SRCU_H

#include <linux/rcupdate.h>
#include <linux/mutex.h>

struct srcu_struct {
	volatile uint32_t readers;
	volatile uint32_t waiters;	/* synchronize_srcu() callers waiting */
	volatile uint32_t epoch;	/* reader count reached 0 (with waiters) */
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
