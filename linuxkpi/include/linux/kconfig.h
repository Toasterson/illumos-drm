/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's IS_ENABLED() family (include/linux/kconfig.h, GPL-2.0; this is
 * an independent implementation of the same preprocessor test: an option
 * is "defined" when CONFIG_X expands to 1). Options come from the driver's
 * configuration header.
 */
#ifndef _LINUX_KCONFIG_H
#define	_LINUX_KCONFIG_H

#define	__LKPI_ONE_1			0,
#define	__lkpi_second(a, b, ...)	b
#define	__lkpi_defined3(junk)		__lkpi_second(junk 1, 0, 0)
#define	__lkpi_defined2(v)		__lkpi_defined3(__LKPI_ONE_##v)
#define	__lkpi_defined(x)		__lkpi_defined2(x)

#define	__is_defined(x)			__lkpi_defined(x)
#define	IS_BUILTIN(option)		__lkpi_defined(option)
#define	IS_MODULE(option)		__lkpi_defined(option##_MODULE)
#define	IS_ENABLED(option)		(IS_BUILTIN(option) || IS_MODULE(option))
#define	IS_REACHABLE(option)		IS_ENABLED(option)

#endif
