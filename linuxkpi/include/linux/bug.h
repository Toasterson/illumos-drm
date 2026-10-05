/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's run-time warnings and compile-time assertions for the carried
 * helpers (carried/): WARN(), WARN_ON(), WARN_ONCE(), WARN_ON_ONCE(),
 * BUILD_BUG_ON() and BUILD_BUG_ON_INVALID().
 *
 * A warning is a CE_WARN message through cmn_err(9F) naming the file and
 * line; the macro's value is the condition, as in Linux. There is no
 * BUG()/BUG_ON() here: the helpers carried so far (drm_mm without
 * CONFIG_DRM_DEBUG_MM) do not use them, and R5c decides what they become.
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e before
 * R5c's compatibility layer existed; R5c's header of the same name
 * replaces it.
 *
 * Origin: the names and meaning follow Linux v7.2's include/linux/bug.h,
 * include/asm-generic/bug.h and include/linux/build_bug.h (GPL-2.0). This
 * file is a reimplementation for illumos, not a copy (decision D7).
 */

#ifndef _LINUXKPI_LINUX_BUG_H
#define	_LINUXKPI_LINUX_BUG_H

#include <linux/compiler.h>
#include <sys/cmn_err.h>
#include <sys/atomic.h>

/* Compile-time: a negative array size when `cond` is true. */
#define	BUILD_BUG_ON(cond)	((void)sizeof (char[1 - 2 * !!(cond)]))
/* Type-checks `e` without evaluating it (the disabled-assertion form). */
#define	BUILD_BUG_ON_INVALID(e)	((void)(sizeof ((long)(e))))

#define	WARN(cond, fmt, ...) __extension__ ({				\
	int __lkpi_warn_c = !!(cond);					\
	if (unlikely(__lkpi_warn_c))					\
		cmn_err(CE_WARN, "WARNING: %s:%d: " fmt,		\
		    __FILE__, __LINE__, ##__VA_ARGS__);			\
	unlikely(__lkpi_warn_c);					\
})

#define	WARN_ON(cond)	WARN((cond), "WARN_ON(%s)", #cond)

/* Warns at the first true evaluation of this call site only. */
#define	WARN_ONCE(cond, fmt, ...) __extension__ ({			\
	static volatile uint32_t __lkpi_warned;				\
	int __lkpi_warn_c = !!(cond);					\
	if (unlikely(__lkpi_warn_c) &&					\
	    atomic_cas_32(&__lkpi_warned, 0, 1) == 0)			\
		cmn_err(CE_WARN, "WARNING: %s:%d: " fmt,		\
		    __FILE__, __LINE__, ##__VA_ARGS__);			\
	unlikely(__lkpi_warn_c);					\
})

#define	WARN_ON_ONCE(cond)	WARN_ONCE((cond), "WARN_ON_ONCE(%s)", #cond)

#endif	/* _LINUXKPI_LINUX_BUG_H */
