/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Host-test override of linuxkpi's linux/bug.h: warnings go to stderr and
 * are counted in lkpi_host_warnings (carried/test/host/selftest_host.c),
 * so a test can check that a warning was or was not raised. Test
 * scaffolding only; never in a module.
 */

#ifndef _LINUXKPI_LINUX_BUG_H
#define	_LINUXKPI_LINUX_BUG_H

#include <stdio.h>
#include <linux/compiler.h>

extern unsigned long lkpi_host_warnings;

#define	BUILD_BUG_ON(cond)	((void)sizeof (char[1 - 2 * !!(cond)]))
#define	BUILD_BUG_ON_INVALID(e)	((void)(sizeof ((long)(e))))

#define	WARN(cond, fmt, ...) __extension__ ({				\
	int __lkpi_warn_c = !!(cond);					\
	if (unlikely(__lkpi_warn_c)) {					\
		lkpi_host_warnings++;					\
		fprintf(stderr, "WARNING: %s:%d: " fmt "\n",		\
		    __FILE__, __LINE__, ##__VA_ARGS__);			\
	}								\
	unlikely(__lkpi_warn_c);					\
})
#define	WARN_ON(cond)		WARN((cond), "WARN_ON(%s)", #cond)
#define	WARN_ONCE(cond, fmt, ...)	WARN((cond), fmt, ##__VA_ARGS__)
#define	WARN_ON_ONCE(cond)	WARN_ON(cond)

#endif	/* _LINUXKPI_LINUX_BUG_H */
