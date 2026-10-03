/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's lock validator interface (include/linux/lockdep.h, GPL-2.0).
 * Not supported on illumos: every assertion and annotation is a no-op
 * that evaluates nothing at run time, and lockdep_is_held() is true, so
 * rcu_dereference_protected() conditions pass. illumos' own lock
 * debugging (DEBUG kernels, mdb ::mutex) still applies to the kmutexes
 * underneath.
 */
#ifndef _LINUX_LOCKDEP_H
#define	_LINUX_LOCKDEP_H

#include <linux/lockdep_types.h>

#define	lockdep_init_map(m, n, k, s)		((void)(m))
#define	lockdep_init_map_type(m, n, k, s, ...)	((void)(m))
#define	lockdep_set_class(l, k)			((void)(l))
#define	lockdep_set_class_and_name(l, k, n)	((void)(l))
#define	lockdep_set_class_and_subclass(l, k, s)	((void)(l))
#define	lockdep_set_subclass(l, s)		((void)(l))
#define	lockdep_set_novalidate_class(l)		((void)(l))
#define	lockdep_set_notrack_class(l)		((void)(l))
#define	lockdep_register_key(k)			((void)(k))
#define	lockdep_unregister_key(k)		((void)(k))
#define	lockdep_assert_held(l)			((void)(l))
#define	lockdep_assert_held_write(l)		((void)(l))
#define	lockdep_assert_held_read(l)		((void)(l))
#define	lockdep_assert_held_once(l)		((void)(l))
#define	lockdep_assert_not_held(l)		((void)(l))
#define	lockdep_assert_none_held_once()		((void)0)
#define	lockdep_assert(c)			((void)0)
#define	lockdep_assert_once(c)			((void)0)
#define	lockdep_assert_irqs_enabled()		((void)0)
#define	lockdep_assert_irqs_disabled()		((void)0)
#define	lockdep_assert_in_irq()			((void)0)
#define	lockdep_assert_preemption_enabled()	((void)0)
#define	lockdep_assert_preemption_disabled()	((void)0)
#define	lockdep_assert_in_softirq()		((void)0)
#define	lockdep_is_held(l)			1
#define	lockdep_is_held_type(l, r)		1
#define	lockdep_pin_lock(l)			({ (void)(l); NIL_COOKIE; })
#define	lockdep_repin_lock(l, c)		((void)(l))
#define	lockdep_unpin_lock(l, c)		((void)(l))
#define	lockdep_off()				((void)0)
#define	lockdep_on()				((void)0)
#define	debug_locks				0
#define	debug_locks_off()			0
#define	lock_acquire(l, s, t, r, c, n, i)	((void)0)
#define	lock_release(l, i)			((void)0)
#define	lock_acquire_exclusive(l, s, t, n, i)	((void)0)
#define	lock_acquire_shared(l, s, t, n, i)	((void)0)
#define	lock_map_acquire(l)			((void)(l))
#define	lock_map_acquire_try(l)			((void)(l))
#define	lock_map_acquire_read(l)		((void)(l))
#define	lock_map_acquire_shared_recursive(l)	((void)(l))
#define	lock_map_release(l)			((void)(l))
#define	lock_is_held(l)				1
#define	might_lock(l)				((void)(l))
#define	might_lock_nested(l, s)			((void)(l))
#define	might_lock_read(l)			((void)(l))
#define	mutex_acquire(l, s, t, i)		((void)0)
#define	mutex_release(l, i)			((void)0)
#define	DEFINE_WAIT_OVERRIDE_MAP(name, type)	struct lockdep_map name = { #name }
#define	lockdep_depth(t)			0
#define	print_irqtrace_events(t)		((void)0)
#define	LOCK_STATE_UNKNOWN			-1
#define	LOCK_STATE_NOT_HELD			0
#define	LOCK_STATE_HELD				1

#endif
