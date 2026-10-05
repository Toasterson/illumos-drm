/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's RCU (include/linux/rcupdate.h, GPL-2.0; reimplemented in
 * src/lkpi_rcu.c).
 *
 * Grace periods use two reader counts and a phase index (src/lkpi_rcu.c).
 * The outermost rcu_read_lock() of a thread counts it in the current
 * phase and remembers that phase for its rcu_read_unlock(): in the
 * thread's task (`current`) in thread context, in a small table keyed by
 * the interrupt thread in an interrupt handler. Read sections nest, may
 * run in interrupt handlers and stay correct when the reader moves to
 * another CPU; as with Linux's non-preemptible RCU they must not sleep
 * (blocking on a spinlock, an adaptive mutex here, is fine).
 * synchronize_rcu() flips the phase and waits only for the readers of the
 * old one, so readers that start meanwhile do not hold it up: a grace
 * period is bounded by the longest read section that overlaps it.
 * call_rcu() and kfree_rcu() queue the callback for a layer task queue
 * that waits for a grace period and then runs the callbacks in thread
 * context; rcu_barrier() waits for all queued callbacks.
 *
 * The DRM core frees GEM objects without a grace period (see the facade's
 * README, "R5b: GEM reference counts"): lockless RCU lookups of GEM
 * objects are not safe on illumos and must be locked lookups.
 */
#ifndef _LINUX_RCUPDATE_H
#define	_LINUX_RCUPDATE_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/lockdep.h>
#include <linux/cleanup.h>
#include <linux/preempt.h>
#include <asm/barrier.h>

extern void rcu_read_lock(void);
extern void rcu_read_unlock(void);

#define	rcu_read_lock_bh()		rcu_read_lock()
#define	rcu_read_unlock_bh()		rcu_read_unlock()
#define	rcu_read_lock_sched()		rcu_read_lock()
#define	rcu_read_unlock_sched()		rcu_read_unlock()
#define	rcu_read_lock_sched_notrace()	rcu_read_lock()
#define	rcu_read_unlock_sched_notrace()	rcu_read_unlock()
#define	rcu_read_lock_held()		1
#define	rcu_read_lock_bh_held()		1
#define	rcu_read_lock_sched_held()	1
#define	rcu_read_lock_any_held()	1
#define	rcu_is_watching()		1
#define	RCU_LOCKDEP_WARN(c, s)		((void)0)
#define	rcu_lockdep_assert(c, s)	((void)0)
#define	rcu_sleep_check()		((void)0)

#define	rcu_dereference_raw(p)		READ_ONCE(p)
#define	rcu_dereference(p)		READ_ONCE(p)
#define	rcu_dereference_bh(p)		READ_ONCE(p)
#define	rcu_dereference_sched(p)	READ_ONCE(p)
#define	rcu_dereference_check(p, c)	READ_ONCE(p)
#define	rcu_dereference_bh_check(p, c)	READ_ONCE(p)
#define	rcu_dereference_protected(p, c)	(p)
#define	rcu_dereference_raw_check(p)	READ_ONCE(p)
#define	srcu_dereference(p, ssp)	READ_ONCE(p)
#define	srcu_dereference_check(p, ssp, c)	READ_ONCE(p)
#define	rcu_access_pointer(p)		READ_ONCE(p)
#define	rcu_pointer_handoff(p)		(p)
#define	unrcu_pointer(p)		(p)
#define	RCU_INITIALIZER(v)		(v)
#define	rcu_assign_pointer(p, v)	smp_store_release(&(p), (v))
#define	RCU_INIT_POINTER(p, v)		WRITE_ONCE(p, v)
#define	RCU_POINTER_INITIALIZER(p, v)	.p = (v)
#define	rcu_replace_pointer(rp, p, c) ({ \
	typeof (rp) __tmp = (rp); rcu_assign_pointer((rp), (p)); __tmp; })

extern void synchronize_rcu(void);
extern void call_rcu(struct rcu_head *head, rcu_callback_t func);
extern void rcu_barrier(void);
extern void lkpi_kvfree_rcu(void *ptr, struct rcu_head *head);
extern unsigned long get_state_synchronize_rcu(void);
extern bool poll_state_synchronize_rcu(unsigned long oldstate);
extern void cond_synchronize_rcu(unsigned long oldstate);

#define	synchronize_rcu_expedited()	synchronize_rcu()
#define	synchronize_rcu_tasks()		synchronize_rcu()
#define	synchronize_sched()		synchronize_rcu()
#define	synchronize_net()		synchronize_rcu()
#define	call_rcu_hurry(h, f)		call_rcu(h, f)
#define	start_poll_synchronize_rcu()	get_state_synchronize_rcu()
#define	init_rcu_head(h)		((void)(h))
#define	destroy_rcu_head(h)		((void)(h))
#define	init_rcu_head_on_stack(h)	((void)(h))
#define	destroy_rcu_head_on_stack(h)	((void)(h))
#define	rcu_head_init(h)		((void)(h))
#define	rcu_head_after_call_rcu(h, f)	false
#define	kfree_rcu(ptr, field) \
	lkpi_kvfree_rcu((ptr), &(ptr)->field)
#define	kvfree_rcu(ptr, field)		kfree_rcu(ptr, field)
#define	kfree_rcu_mightsleep(ptr)	do { synchronize_rcu(); kvfree(ptr); } while (0)
#define	kvfree_rcu_mightsleep(ptr)	kfree_rcu_mightsleep(ptr)
#define	cond_resched_rcu()		((void)0)

DEFINE_LOCK_GUARD_0(rcu, rcu_read_lock(), rcu_read_unlock())

#endif
