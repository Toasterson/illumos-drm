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
 *
 * Written for illumos: one loop (lkpi_poll_until()) over a small poll
 * state whose functions decide when the time is up and how to pause; a
 * sleeping pause takes between half of sleep_us and sleep_us.
 */
#ifndef _LINUX_IOPOLL_H
#define	_LINUX_IOPOLL_H

#include <linux/types.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/math.h>
#include <asm/barrier.h>

/* The state of one poll (lkpi_poll_until()). */
struct lkpi_poll {
	u64		lp_timeout_us;	/* 0: no limit */
	unsigned long	lp_pause_us;	/* 0: spin between tries */
	bool		lp_atomic;	/* udelay(), time counted from pauses */
	ktime_t		lp_end;		/* sleeping polls: the deadline */
	u64		lp_spent_ns;	/* atomic polls: time spent pausing */
};

static inline void
lkpi_poll_pause(struct lkpi_poll *p)
{
	if (p->lp_pause_us != 0) {
		if (p->lp_atomic)
			udelay(p->lp_pause_us);
		else
			usleep_range(DIV_ROUND_UP(p->lp_pause_us, 2),
			    p->lp_pause_us);
	}
	cpu_relax();
	/* A spin counts as a nanosecond, so a spinning poll ends too. */
	p->lp_spent_ns += (u64)p->lp_pause_us * NSEC_PER_USEC + 1;
}

static inline void
lkpi_poll_start(struct lkpi_poll *p, u64 timeout_us, unsigned long pause_us,
    bool atomic, bool pause_first)
{
	p->lp_timeout_us = timeout_us;
	p->lp_pause_us = pause_us;
	p->lp_atomic = atomic;
	p->lp_spent_ns = 0;
	p->lp_end = atomic ? 0 : ktime_add_us(ktime_get(), timeout_us);
	if (pause_first && pause_us != 0)
		lkpi_poll_pause(p);
}

/* Whether the time is up; read before a try, which still counts. */
static inline bool
lkpi_poll_out_of_time(const struct lkpi_poll *p)
{
	if (p->lp_timeout_us == 0)
		return (false);
	if (p->lp_atomic)
		return (p->lp_spent_ns > p->lp_timeout_us * NSEC_PER_USEC);
	return (ktime_after(ktime_get(), p->lp_end));
}

/*
 * Tries op and cond until cond holds (0) or a try that began after the
 * time was up failed too (-ETIMEDOUT), pausing between tries.
 */
#define	lkpi_poll_until(op, cond, pause, limit, first, atomic) ({ \
	struct lkpi_poll __lp; \
	bool __lp_last; \
	int __lp_ret = -ETIMEDOUT; \
	lkpi_poll_start(&__lp, (limit), (pause), (atomic), (first)); \
	do { \
		__lp_last = lkpi_poll_out_of_time(&__lp); \
		barrier(); \
		op; \
		if (cond) { \
			__lp_ret = 0; \
			break; \
		} \
		if (!__lp_last) \
			lkpi_poll_pause(&__lp); \
	} while (!__lp_last); \
	__lp_ret; \
})

#define	poll_timeout_us(op, cond, sleep_us, timeout_us, sleep_before_op) \
	lkpi_poll_until(op, cond, sleep_us, timeout_us, sleep_before_op, false)
#define	poll_timeout_us_atomic(op, cond, delay_us, timeout_us, \
    delay_before_op) \
	lkpi_poll_until(op, cond, delay_us, timeout_us, delay_before_op, true)

/* The read_ forms: the operation stores op(args) in val. */
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
