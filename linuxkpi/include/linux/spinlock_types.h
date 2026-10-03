/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's spinlock_t is the DRM facade's struct spinlock: an illumos
 * kmutex_t (<drm/drm_facade_types.h>). raw_spinlock_t is the same.
 * A zero-filled kmutex_t is a valid unlocked adaptive mutex, so the
 * static initialisers are empty.
 */
#ifndef _LINUX_SPINLOCK_TYPES_H
#define	_LINUX_SPINLOCK_TYPES_H

#include <linux/types.h>
#include <linux/lockdep_types.h>

typedef spinlock_t raw_spinlock_t;

typedef struct {
	krwlock_t rw;
} rwlock_t;

#define	__SPIN_LOCK_UNLOCKED(name)	{ }
#define	__RAW_SPIN_LOCK_UNLOCKED(name)	{ }
#define	__RW_LOCK_UNLOCKED(name)	{ }
#define	DEFINE_SPINLOCK(name)		spinlock_t name = { }
#define	DEFINE_RAW_SPINLOCK(name)	raw_spinlock_t name = { }
#define	DEFINE_RWLOCK(name)		rwlock_t name = { }

#endif
