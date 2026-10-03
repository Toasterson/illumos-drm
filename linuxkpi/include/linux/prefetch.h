/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's prefetch hints (gcc's __builtin_prefetch).
 */
#ifndef _LINUX_PREFETCH_H
#define	_LINUX_PREFETCH_H
#define	prefetch(x)		__builtin_prefetch(x)
#define	prefetchw(x)		__builtin_prefetch(x, 1)
#endif
