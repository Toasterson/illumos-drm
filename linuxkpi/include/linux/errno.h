/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's error numbers for the carried helpers (carried/): illumos' own
 * (sys/errno.h). Facade functions return 0 or a negative illumos errno
 * (drm/drm_facade_types.h), and so do the carried helpers; the numbers
 * the helpers carried so far return (ENOSPC) are the same on both
 * systems.
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e before
 * R5c's compatibility layer existed; R5c's header of the same name
 * replaces it.
 */

#ifndef _LINUXKPI_LINUX_ERRNO_H
#define	_LINUXKPI_LINUX_ERRNO_H

#include <sys/errno.h>

#endif	/* _LINUXKPI_LINUX_ERRNO_H */
