/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's jiffies (include/linux/jiffies.h, GPL-2.0; reimplemented).
 * The layer's HZ is 1000 and jiffies is gethrtime(9F) in milliseconds,
 * independent of illumos' own clock tick (hz, 100 or 1000). Sleeps and
 * timers convert to illumos ticks with drv_usectohz(9F), so their
 * resolution is illumos' tick.
 */
#ifndef _LINUX_JIFFIES_H
#define	_LINUX_JIFFIES_H

#include <linux/types.h>
#include <linux/math.h>
#include <linux/time64.h>
#include <linux/seqlock.h>

static inline unsigned long
lkpi_jiffies(void)
{
	return ((unsigned long)(gethrtime() / (NANOSEC / HZ)));
}

#define	jiffies			lkpi_jiffies()
#define	jiffies_64		((u64)lkpi_jiffies())
#define	get_jiffies_64()	((u64)lkpi_jiffies())
#define	INITIAL_JIFFIES		0UL
#define	MAX_JIFFY_OFFSET	((LONG_MAX >> 1) - 1)
#define	MAX_SCHEDULE_TIMEOUT	LONG_MAX
#define	TICK_NSEC		(NSEC_PER_SEC / HZ)
#define	TICK_USEC		(USEC_PER_SEC / HZ)

#define	time_after(a, b)	((long)((b) - (a)) < 0)
#define	time_before(a, b)	time_after(b, a)
#define	time_after_eq(a, b)	((long)((a) - (b)) >= 0)
#define	time_before_eq(a, b)	time_after_eq(b, a)
#define	time_in_range(a, b, c)	(time_after_eq(a, b) && time_before_eq(a, c))
#define	time_in_range_open(a, b, c)	(time_after_eq(a, b) && time_before(a, c))
#define	time_after32(a, b)	((s32)((u32)(b) - (u32)(a)) < 0)
#define	time_before32(b, a)	time_after32(a, b)
#define	time_after64(a, b)	((s64)((b) - (a)) < 0)
#define	time_before64(a, b)	time_after64(b, a)
#define	time_after_eq64(a, b)	((s64)((a) - (b)) >= 0)
#define	time_before_eq64(a, b)	time_after_eq64(b, a)
#define	time_is_before_jiffies(a)	time_after(jiffies, a)
#define	time_is_after_jiffies(a)	time_before(jiffies, a)
#define	time_is_before_eq_jiffies(a)	time_after_eq(jiffies, a)
#define	time_is_after_eq_jiffies(a)	time_before_eq(jiffies, a)

static inline unsigned long
msecs_to_jiffies(const unsigned int m)
{
	return ((unsigned long)m);
}

static inline unsigned long
usecs_to_jiffies(const unsigned int u)
{
	return ((unsigned long)DIV_ROUND_UP(u, 1000));
}

static inline unsigned long
nsecs_to_jiffies(u64 n)
{
	return ((unsigned long)(n / (NSEC_PER_SEC / HZ)));
}

static inline u64
nsecs_to_jiffies64(u64 n)
{
	return (n / (NSEC_PER_SEC / HZ));
}

static inline unsigned int
jiffies_to_msecs(const unsigned long j)
{
	return ((unsigned int)j);
}

static inline unsigned int
jiffies_to_usecs(const unsigned long j)
{
	return ((unsigned int)(j * 1000));
}

static inline u64
jiffies_to_nsecs(const unsigned long j)
{
	return ((u64)j * (NSEC_PER_SEC / HZ));
}

static inline u64
jiffies64_to_nsecs(u64 j)
{
	return (j * (NSEC_PER_SEC / HZ));
}

static inline u64
jiffies64_to_msecs(u64 j)
{
	return (j);
}

static inline unsigned long
timespec64_to_jiffies(const struct timespec64 *ts)
{
	return ((unsigned long)(ts->tv_sec * HZ + ts->tv_nsec / (NSEC_PER_SEC / HZ)));
}

static inline void
jiffies_to_timespec64(unsigned long j, struct timespec64 *ts)
{
	ts->tv_sec = j / HZ;
	ts->tv_nsec = (j % HZ) * (NSEC_PER_SEC / HZ);
}

static inline unsigned long
round_jiffies_up(unsigned long j)
{
	return (j);
}

static inline unsigned long
round_jiffies_up_relative(unsigned long j)
{
	return (j);
}

static inline unsigned long
round_jiffies(unsigned long j)
{
	return (j);
}

static inline unsigned long
round_jiffies_relative(unsigned long j)
{
	return (j);
}

#define	jiffies_delta_to_msecs(d)	jiffies_to_msecs(max(0L, (long)(d)))
#define	jiffies_delta_to_clock_t(d)	((d) / (HZ / USER_HZ))

/* illumos ticks for a jiffy count (at least one tick unless zero). */
static inline clock_t
lkpi_jiffies_to_ticks(unsigned long j)
{
	clock_t t;

	if (j == 0)
		return (0);
	if (j >= (unsigned long)MAX_JIFFY_OFFSET)
		return ((clock_t)LONG_MAX / 2);
	t = drv_usectohz((clock_t)j * 1000);
	return (t > 0 ? t : 1);
}

#endif
