/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's mmap protection and flag values (MAP_*/PROT_* are illumos' from
 * <sys/mman.h>, which the DRM uAPI does not depend on).
 */
#ifndef _LINUX_MMAN_H
#define	_LINUX_MMAN_H
#include <linux/mm.h>
#ifndef	PROT_READ
#define	PROT_READ	0x1
#define	PROT_WRITE	0x2
#define	PROT_EXEC	0x4
#define	PROT_NONE	0x0
#endif
#ifndef	MAP_SHARED
#define	MAP_SHARED	0x01
#define	MAP_PRIVATE	0x02
#endif
#endif
