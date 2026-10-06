/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The stand-alone wound/wait mutexes (src/lkpi_ww_mutex.c), which
 * src/lkpi_resv.c's ww_mutex_*() dispatch to for the locks
 * ww_mutex_init() marked. A header of its own (rather than
 * lkpi_internal.h) so that the host tests can include it.
 */
#ifndef _LKPI_WW_MUTEX_H
#define	_LKPI_WW_MUTEX_H

#include <linux/types.h>

struct ww_mutex;
struct ww_acquire_ctx;

extern int lkpi_ww_init(void);
extern void lkpi_ww_fini(void);
extern bool lkpi_ww_standalone(const struct ww_mutex *lock);
extern int lkpi_ww_sa_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx,
    bool slow, bool intr);
extern int lkpi_ww_sa_trylock(struct ww_mutex *lock,
    struct ww_acquire_ctx *ctx);
extern void lkpi_ww_sa_unlock(struct ww_mutex *lock);
extern bool lkpi_ww_sa_is_locked(const struct ww_mutex *lock);

#endif
