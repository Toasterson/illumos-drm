/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's ktime_t helpers (include/linux/ktime.h, GPL-2.0; reimplemented;
 * ktime_t is the facade's int64 of nanoseconds). The monotonic, raw and
 * boot-time clocks are gethrtime(9F); the real-time clock is
 * gethrestime() (src/lkpi_time.c).
 */
#ifndef _LINUX_KTIME_H
#define	_LINUX_KTIME_H

#include <linux/types.h>
#include <linux/time64.h>
#include <linux/jiffies.h>

#define	ktime_set(s, ns)	((ktime_t)((s64)(s) * NSEC_PER_SEC + (s64)(ns)))
#define	ktime_add(a, b)		((ktime_t)((a) + (b)))
#define	ktime_sub(a, b)		((ktime_t)((a) - (b)))
#define	ktime_add_ns(k, ns)	((ktime_t)((k) + (s64)(ns)))
#define	ktime_sub_ns(k, ns)	((ktime_t)((k) - (s64)(ns)))
#define	ktime_add_us(k, us)	ktime_add_ns(k, (u64)(us) * NSEC_PER_USEC)
#define	ktime_add_ms(k, ms)	ktime_add_ns(k, (u64)(ms) * NSEC_PER_MSEC)
#define	ktime_sub_us(k, us)	ktime_sub_ns(k, (u64)(us) * NSEC_PER_USEC)
#define	ktime_add_safe(a, b)	ktime_add(a, b)
#define	ktime_to_ns(k)		((s64)(k))
#define	ns_to_ktime(ns)		((ktime_t)(ns))
#define	us_to_ktime(us)		((ktime_t)((s64)(us) * NSEC_PER_USEC))
#define	ms_to_ktime(ms)		((ktime_t)((s64)(ms) * NSEC_PER_MSEC))
#define	ktime_to_us(k)		((s64)(k) / NSEC_PER_USEC)
#define	ktime_to_ms(k)		((s64)(k) / NSEC_PER_MSEC)
#define	ktime_us_delta(a, b)	ktime_to_us(ktime_sub(a, b))
#define	ktime_ms_delta(a, b)	ktime_to_ms(ktime_sub(a, b))
#define	ktime_compare(a, b)	((a) < (b) ? -1 : ((a) > (b) ? 1 : 0))
#define	ktime_after(a, b)	((a) > (b))
#define	ktime_before(a, b)	((a) < (b))
#define	ktime_equal(a, b)	((a) == (b))
#define	ktime_divns(k, d)	((s64)(k) / (s64)(d))
#define	ktime_to_timespec64(k)	ns_to_timespec64(k)
#define	timespec64_to_ktime(ts)	ktime_set((ts).tv_sec, (ts).tv_nsec)
#define	KTIME_MAX_LKPI		((s64)~((u64)1 << 63))

extern ktime_t lkpi_ktime_get_real(void);

static inline ktime_t ktime_get(void) { return ((ktime_t)gethrtime()); }
static inline ktime_t ktime_get_raw(void) { return ((ktime_t)gethrtime()); }
static inline ktime_t ktime_get_boottime(void) { return ((ktime_t)gethrtime()); }
static inline ktime_t ktime_get_real(void) { return (lkpi_ktime_get_real()); }
static inline u64 ktime_get_ns(void) { return ((u64)gethrtime()); }
static inline u64 ktime_get_raw_ns(void) { return ((u64)gethrtime()); }
static inline u64 ktime_get_raw_fast_ns(void) { return ((u64)gethrtime()); }
static inline u64 ktime_get_mono_fast_ns(void) { return ((u64)gethrtime()); }
static inline u64 ktime_get_boottime_ns(void) { return ((u64)gethrtime()); }
static inline u64 ktime_get_real_ns(void) { return ((u64)lkpi_ktime_get_real()); }
static inline time64_t ktime_get_seconds(void) { return (gethrtime() / NSEC_PER_SEC); }
static inline time64_t ktime_get_real_seconds(void) { return (lkpi_ktime_get_real() / NSEC_PER_SEC); }
static inline time64_t ktime_get_boottime_seconds(void) { return (gethrtime() / NSEC_PER_SEC); }

static inline void
ktime_get_ts64(struct timespec64 *ts)
{
	*ts = ns_to_timespec64(gethrtime());
}

static inline void
ktime_get_real_ts64(struct timespec64 *ts)
{
	*ts = ns_to_timespec64(lkpi_ktime_get_real());
}

#define	ktime_get_raw_ts64(ts)	ktime_get_ts64(ts)
#define	ktime_get_clocktai()	ktime_get_real()

#endif
