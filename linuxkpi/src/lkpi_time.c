/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The real-time clock and sleeping delays (<linux/ktime.h>,
 * <linux/delay.h>). Busy-waiting delays are drv_usecwait(9F) macros in
 * the header.
 *
 * lkpi_usleep() (usleep_range(), fsleep()) busy-waits below one illumos
 * clock tick and sleeps with delay(9F) otherwise: drivers poll registers
 * with sleeps of tens of microseconds, which a tick-granular sleep would
 * stretch to a whole tick (10 ms at hz=100). Sleeping is never done in
 * an interrupt handler; there the delays busy-wait.
 */

#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

/* gethrestime() (<sys/time.h>, not part of the DDI): wall-clock time. */
ktime_t
lkpi_ktime_get_real(void)
{
	timespec_t ts;

	gethrestime(&ts);
	return ((ktime_t)ts.tv_sec * NANOSEC + ts.tv_nsec);
}

/* The length of an illumos clock tick in microseconds (hz from 9F). */
static clock_t
lkpi_tick_usec(void)
{
	static clock_t tick_us;
	clock_t hz_now;

	if (tick_us == 0) {
		hz_now = drv_usectohz(MICROSEC);
		tick_us = hz_now > 0 ? MICROSEC / hz_now : 10000;
	}
	return (tick_us);
}

void
lkpi_usleep(unsigned long usecs)
{
	if (usecs == 0)
		return;
	if (usecs < (unsigned long)lkpi_tick_usec() || servicing_interrupt()) {
		drv_usecwait((clock_t)usecs);
		return;
	}
	if (usecs > (unsigned long)LONG_MAX / 2)
		usecs = LONG_MAX / 2;
	delay(drv_usectohz((clock_t)usecs));
}

void
lkpi_msleep(unsigned int msecs)
{
	clock_t ticks;

	if (servicing_interrupt()) {
		WARN_ONCE(1, "linuxkpi: msleep() in an interrupt");
		drv_usecwait((clock_t)msecs * 1000);
		return;
	}
	/* Linux's msleep(0) still gives up the processor. */
	ticks = drv_usectohz((clock_t)msecs * 1000);
	delay(ticks > 0 ? ticks : 1);
}

/* Sleeps unless a signal arrives; the milliseconds left (0: slept all). */
unsigned long
lkpi_msleep_interruptible(unsigned int msecs)
{
	long left = (long)msecs_to_jiffies(msecs) + 1;

	if (servicing_interrupt()) {
		WARN_ONCE(1, "linuxkpi: msleep() in an interrupt");
		drv_usecwait((clock_t)msecs * 1000);
		return (0);
	}
	while (left > 0 && !signal_pending(current))
		left = schedule_timeout_interruptible(left);
	return (jiffies_to_msecs((unsigned long)left));
}
