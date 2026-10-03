/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The layer's fixed configuration. The driver's own CONFIG_* choices come
 * from its configuration header (for i915: drivers/i915/config.h), which
 * is included before the prologue. The layer provides the "not configured"
 * variant of every Linux facility illumos lacks (debugfs, sysfs, runtime
 * PM, lockdep, tracepoints, perf, ACPI, MMU notifiers), so a driver
 * configuration must not enable those.
 */

#ifndef _LKPI_CONFIG_H
#define	_LKPI_CONFIG_H

/* The layer's tick: jiffies count milliseconds (gethrtime(9F) / 1e6). */
#undef	HZ
#undef	PAGE_SHIFT
#undef	PAGE_SIZE
#undef	PAGE_MASK
#define	HZ		1000
#define	USER_HZ		100

/* Linux's x86-64 page size, which is illumos' MMU page size. */
#define	PAGE_SHIFT	12
#define	PAGE_SIZE	(1UL << PAGE_SHIFT)
#define	PAGE_MASK	(~(PAGE_SIZE - 1))

#define	BITS_PER_LONG		64
#define	BITS_PER_LONG_LONG	64
#define	BITS_PER_BYTE		8
#define	L1_CACHE_SHIFT		6
#define	L1_CACHE_BYTES		(1 << L1_CACHE_SHIFT)
#define	SMP_CACHE_BYTES		L1_CACHE_BYTES
#define	NR_CPUS			256

#if defined(CONFIG_DEBUG_FS) || defined(CONFIG_PM) || \
	defined(CONFIG_LOCKDEP) || defined(CONFIG_TRACEPOINTS) || \
	defined(CONFIG_PERF_EVENTS) || defined(CONFIG_ACPI) || \
	defined(CONFIG_MMU_NOTIFIER) || defined(CONFIG_COMPAT)
#error "the driver configuration enables a facility the LinuxKPI layer does not provide"
#endif

#endif	/* _LKPI_CONFIG_H */
