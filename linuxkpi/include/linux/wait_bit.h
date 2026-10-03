/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's waits on a variable or a bit (include/linux/wait_bit.h,
 * GPL-2.0; reimplemented): a table of shared wait queues hashed by
 * address (src/lkpi_wait.c).
 */
#ifndef _LINUX_WAIT_BIT_H
#define	_LINUX_WAIT_BIT_H

#include <linux/wait.h>

extern struct wait_queue_head *__var_waitqueue(void *p);
extern struct wait_queue_head *bit_waitqueue(void *word, int bit);
extern void wake_up_var(void *var);
extern void wake_up_bit(void *word, int bit);

#define	wait_var_event(var, condition) \
	wait_event(*__var_waitqueue(var), condition)
#define	wait_var_event_killable(var, condition) \
	wait_event_killable(*__var_waitqueue(var), condition)
#define	wait_var_event_interruptible(var, condition) \
	wait_event_interruptible(*__var_waitqueue(var), condition)
#define	wait_var_event_timeout(var, condition, timeout) \
	wait_event_timeout(*__var_waitqueue(var), condition, timeout)
#define	wait_on_bit(word, bit, mode) ({ \
	wait_event(*bit_waitqueue((void *)(word), (bit)), \
	    !test_bit((bit), (word))); 0; })
#define	wait_on_bit_timeout(word, bit, mode, timeout) ({ \
	wait_event_timeout(*bit_waitqueue((void *)(word), (bit)), \
	    !test_bit((bit), (word)), (timeout)) ? 0 : -EAGAIN; })
#define	clear_and_wake_up_bit(bit, word) do { \
	clear_bit_unlock((bit), (word)); smp_mb__after_atomic(); \
	wake_up_bit((word), (bit)); } while (0)

#endif
