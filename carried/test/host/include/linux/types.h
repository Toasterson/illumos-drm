/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Host-test override of linuxkpi's linux/types.h: the same names on the
 * C library instead of the facade and the illumos kernel headers, so the
 * carried helpers and their self-tests compile into a userland program
 * (carried/host-test.sh). Test scaffolding only; never in a module.
 */

#ifndef _LINUXKPI_LINUX_TYPES_H
#define	_LINUXKPI_LINUX_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef	container_of
#define	container_of(ptr, type, member) \
	((type *)(void *)((char *)(ptr) - offsetof(type, member)))
#endif

struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

typedef unsigned int gfp_t;

typedef int8_t		s8;
typedef uint8_t		u8;
typedef int16_t		s16;
typedef uint16_t	u16;
typedef int32_t		s32;
typedef uint32_t	u32;
typedef int64_t		s64;
typedef uint64_t	u64;

#endif	/* _LINUXKPI_LINUX_TYPES_H */
