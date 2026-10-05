/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's sleepable RCU (include/linux/srcu.h, GPL-2.0; reimplemented
 * in src/lkpi_rcu.c): two reader counts and a phase index per
 * srcu_struct, as for the layer's RCU (<linux/rcupdate.h>).
 * srcu_read_lock() counts the reader in the current phase and returns the
 * phase, which srcu_read_unlock() takes back; synchronize_srcu() flips
 * the phase and waits for the readers of the old one. Readers may sleep.
 * A zero-filled srcu_struct (DEFINE_SRCU()) is valid.
 */
#ifndef _LINUX_SRCU_H
#define	_LINUX_SRCU_H

#include <linux/rcupdate.h>
#include <linux/mutex.h>

struct srcu_struct {
	volatile uint32_t readers[2];	/* readers per phase */
	volatile uint32_t idx;		/* current phase (bit 0) */
	volatile uint32_t waiters;	/* a grace period waits on cv */
	kmutex_t lock;			/* cv */
	kcondvar_t cv;			/* a phase's last reader left */
	kmutex_t gp_lock;		/* serialises grace periods */
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
