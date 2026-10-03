/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's timers over illumos callouts (include/linux/timer.h, GPL-2.0;
 * reimplemented in src/lkpi_timer.c).
 *
 * mod_timer() arms a timeout(9F) callout for the remaining jiffies
 * (rounded up to illumos' tick) with a separately allocated arm record;
 * the timer function runs in callout context and must not sleep, as on
 * Linux. timer_delete() (del_timer()) disarms without waiting: the
 * callout, if it still fires, finds its record cancelled and does
 * nothing. timer_delete_sync() also waits until the function is not
 * running. The timer structure is never touched after its function
 * returns, so a timer function may free its own timer.
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
