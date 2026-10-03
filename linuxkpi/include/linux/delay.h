/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's delays (include/linux/delay.h, GPL-2.0; reimplemented).
 * udelay()/ndelay()/mdelay() busy-wait with drv_usecwait(9F). msleep()
 * sleeps with delay(9F), rounded up to illumos' tick. usleep_range() and
 * fsleep() busy-wait when the requested time is below one tick (register
 * polling loops in drivers ask for tens of microseconds, which a
 * tick-granular sleep would stretch to a tick) and sleep otherwise.
 */
#ifndef _LINUX_DELAY_H
#define	_LINUX_DELAY_H

#include <linux/types.h>
#include <linux/math.h>

extern void lkpi_usleep(unsigned long usecs);
extern void lkpi_msleep(unsigned int msecs);
extern unsigned long lkpi_msleep_interruptible(unsigned int msecs);

#define	udelay(n)		drv_usecwait((clock_t)(n))
#define	ndelay(n)		drv_usecwait((clock_t)DIV_ROUND_UP((n), 1000))
#define	mdelay(n)		drv_usecwait((clock_t)(n) * 1000)
#define	msleep(n)		lkpi_msleep(n)
#define	ssleep(n)		lkpi_msleep((n) * 1000)
#define	msleep_interruptible(n)	lkpi_msleep_interruptible(n)
#define	usleep_range(min, max)	lkpi_usleep(min)
#define	usleep_range_idle(min, max)	lkpi_usleep(min)
#define	fsleep(us)		lkpi_usleep(us)

#endif
