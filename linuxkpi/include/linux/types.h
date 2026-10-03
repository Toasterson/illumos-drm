/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's basic kernel types for the carried helpers (carried/), in the
 * illumos kernel.
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e (carried
 * helpers) before R5c's compatibility layer existed. R5c's header of the
 * same name replaces it; the reconciliation is a file replacement.
 *
 * Origin: the names follow Linux v7.2's include/linux/types.h (GPL-2.0).
 * This file is a reimplementation for illumos, not a copy (decision D7).
 * struct list_head, atomic_t, refcount_t, struct kref, gfp_t, ERR_PTR()
 * and container_of() come from the facade (drm/drm_facade_types.h), so a
 * file may include both.
 */

#ifndef _LINUXKPI_LINUX_TYPES_H
#define	_LINUXKPI_LINUX_TYPES_H

#include <drm/drm_facade_types.h>

typedef int8_t		s8;
typedef uint8_t		u8;
typedef int16_t		s16;
typedef uint16_t	u16;
typedef int32_t		s32;
typedef uint32_t	u32;
typedef int64_t		s64;
typedef uint64_t	u64;

#endif	/* _LINUXKPI_LINUX_TYPES_H */
