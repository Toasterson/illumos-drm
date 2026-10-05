/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's timers (include/linux/timer.h, GPL-2.0; reimplemented in
 * src/lkpi_timer.c).
 *
 * Pending timers are kept in one tree ordered by expiry, served by one
 * layer thread (a single-thread ddi_taskq(9F)) that sleeps with
 * cv_reltimedwait(9F) until the earliest expiry, so a timer fires at the
 * first illumos clock tick at or after its expiry. Arming, re-arming and
 * deleting only take the layer's timer lock: they never allocate and
 * never wait, so they work in interrupt handlers and under spinlocks.
 * Timer functions run one at a time on that thread (thread context, but
 * like Linux they must not sleep: they delay every other timer).
 * timer_delete() (del_timer()) disarms without waiting;
 * timer_delete_sync() also waits until the function is not running. The
 * timer structure is never touched after its function returns, so a
 * timer function may free its own timer.
 *
 * Deviation from the first design (one timeout(9F) callout per arming
 * with an allocated arm record): a callout cannot be cancelled without
 * possibly waiting for it (untimeout(9F)), so every disarmed callout
 * would have needed a record that outlives the timer and a way to cancel
 * it at module unload; the timer thread has neither problem.
 */
#ifndef _LINUX_TIMER_H
#define	_LINUX_TIMER_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/jiffies.h>
#include <linux/timer_types.h>

#define	TIMER_DEFERRABLE	0x00080000
#define	TIMER_PINNED		0x00100000
#define	TIMER_IRQSAFE		0x00200000

extern void lkpi_timer_setup(struct timer_list *t,
    void (*fn)(struct timer_list *), unsigned int flags);
extern int mod_timer(struct timer_list *t, unsigned long expires);
extern int mod_timer_pending(struct timer_list *t, unsigned long expires);
extern int timer_reduce(struct timer_list *t, unsigned long expires);
extern void add_timer(struct timer_list *t);
extern int timer_delete(struct timer_list *t);
extern int timer_delete_sync(struct timer_list *t);
extern int timer_shutdown_sync(struct timer_list *t);
extern int timer_pending(const struct timer_list *t);

#define	timer_setup(t, fn, flags)	lkpi_timer_setup(t, fn, flags)
#define	timer_setup_on_stack(t, fn, f)	lkpi_timer_setup(t, fn, f)
#define	destroy_timer_on_stack(t)	((void)(t))
#define	del_timer(t)			timer_delete(t)
#define	del_timer_sync(t)		timer_delete_sync(t)
#define	timer_shutdown(t)		timer_delete(t)
#define	add_timer_on(t, cpu)		add_timer(t)
#define	from_timer(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, typeof (*var), timer_fieldname)
#define	timer_container_of(var, t, field) \
	container_of(t, typeof (*var), field)
#define	DEFINE_TIMER(name, fn) \
	struct timer_list name = { .function = (fn) }
#define	__init_timer(t, fn, flags)	lkpi_timer_setup(t, fn, flags)

#endif
