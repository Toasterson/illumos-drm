/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Lock validator types. illumos has no lockdep; the types are empty and
 * every annotation is a no-op (<linux/lockdep.h>).
 */
#ifndef _LINUX_LOCKDEP_TYPES_H
#define	_LINUX_LOCKDEP_TYPES_H

struct lock_class_key {
	char __unused_key;
};

struct lockdep_map {
	const char *name;
};

struct pin_cookie {
	unsigned int val;
};

#define	NIL_COOKIE			((struct pin_cookie){ .val = 0U, })
#define	STATIC_LOCKDEP_MAP_INIT(n, k)	{ .name = (n) }
#define	SINGLE_DEPTH_NESTING		1
#define	MAX_LOCKDEP_SUBCLASSES		8UL
#define	lockdep_match_class(lock, key)	1

#endif
