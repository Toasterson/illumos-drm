/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's polling helpers (include/linux/iopoll.h, GPL-2.0; independent
 * implementation of the same interface).
 *
 * poll_timeout_us(op, cond, sleep_us, timeout_us, sleep_before_op)
 *	evaluates op, then cond, until cond is true (0) or timeout_us
 *	microseconds have passed (-ETIMEDOUT; op and cond are evaluated
 *	once more after the time ran out, so a condition that became true
 *	at the last moment still counts). Between tries it sleeps sleep_us
 *	(usleep_range; 0: busy loop). timeout_us 0 polls forever. Thread
 *	context when sleep_us is not 0.
 * poll_timeout_us_atomic(op, cond, delay_us, timeout_us, delay_before_op)
 *	the same with udelay() between tries and the time counted from the
 *	delays (no clock reads), for any context.
 * read_poll_timeout(op, val, cond, sleep_us, timeout_us, sleep_before_read,
 *	args...) assigns val = op(args) as the operation; the _atomic and
 *	readx_ variants likewise.
 */
#ifndef _LINUX_IOPOLL_H
#define	_LINUX_IOPOLL_H

#include <linux/types.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <asm/barrier.h>

#define	poll_timeout_us(op, cond, sleep_us, timeout_us, sleep_before_op) ({ \
	u64 __pt_to = (timeout_us); \
	unsigned long __pt_sl = (sleep_us); \
	ktime_t __pt_end = ktime_add_us(ktime_get(), __pt_to); \
	int __pt_ret; \
	if ((sleep_before_op) && __pt_sl != 0) \
		usleep_range((__pt_sl >> 2) + 1, __pt_sl); \
	for (;;) { \
		bool __pt_exp = __pt_to != 0 && \
		    ktime_compare(ktime_get(), __pt_end) > 0; \
		barrier(); \
		op; \
		if (cond) { \
			__pt_ret = 0; \
			break; \
		} \
		if (__pt_exp) { \
			__pt_ret = -ETIMEDOUT; \
			break; \
		} \
		if (__pt_sl != 0) \
			usleep_range((__pt_sl >> 2) + 1, __pt_sl); \
		cpu_relax(); \
	} \
	__pt_ret; \
})

#define	poll_timeout_us_atomic(op, cond, delay_us, timeout_us, \
    delay_before_op) ({ \
	u64 __pa_to = (timeout_us); \
	s64 __pa_left = (s64)__pa_to * NSEC_PER_USEC; \
	unsigned long __pa_dl = (delay_us); \
	int __pa_ret; \
	if ((delay_before_op) && __pa_dl != 0) { \
		udelay(__pa_dl); \
		__pa_left -= (s64)__pa_dl * NSEC_PER_USEC; \
	} \
	for (;;) { \
		bool __pa_exp = __pa_to != 0 && __pa_left < 0; \
		barrier(); \
		op; \
		if (cond) { \
			__pa_ret = 0; \
			break; \
		} \
		if (__pa_exp) { \
			__pa_ret = -ETIMEDOUT; \
			break; \
		} \
		if (__pa_dl != 0) { \
			udelay(__pa_dl); \
			__pa_left -= (s64)__pa_dl * NSEC_PER_USEC; \
		} \
		cpu_relax(); \
		__pa_left--; \
	} \
	__pa_ret; \
})

#define	read_poll_timeout(op, val, cond, sleep_us, timeout_us, \
    sleep_before_read, args...) \
	poll_timeout_us((val) = op(args), cond, sleep_us, timeout_us, \
	    sleep_before_read)
#define	read_poll_timeout_atomic(op, val, cond, delay_us, timeout_us, \
    delay_before_read, args...) \
	poll_timeout_us_atomic((val) = op(args), cond, delay_us, timeout_us, \
	    delay_before_read)
#define	readx_poll_timeout(op, addr, val, cond, sleep_us, timeout_us) \
	read_poll_timeout(op, val, cond, sleep_us, timeout_us, false, addr)
#define	readx_poll_timeout_atomic(op, addr, val, cond, delay_us, timeout_us) \
	read_poll_timeout_atomic(op, val, cond, delay_us, timeout_us, false, \
	    addr)
#define	readl_poll_timeout(addr, val, cond, delay_us, timeout_us) \
	readx_poll_timeout(readl, addr, val, cond, delay_us, timeout_us)
#define	readl_poll_timeout_atomic(addr, val, cond, delay_us, timeout_us) \
	readx_poll_timeout_atomic(readl, addr, val, cond, delay_us, timeout_us)

#endif
