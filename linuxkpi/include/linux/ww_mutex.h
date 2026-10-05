/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's wound/wait mutexes (include/linux/ww_mutex.h, GPL-2.0).
 *
 * The only wound/wait mutexes drivers lock are the locks of reservation
 * objects, which live in the DRM core: struct ww_mutex is the facade's
 * public placeholder inside struct dma_resv. The layer's ww_mutex_*()
 * therefore operate on the dma_resv containing the mutex and go through
 * the facade's dma_resv_lock()/dma_resv_unlock(), which apply the core's
 * wait-die rule to the transaction (struct ww_acquire_ctx, stamped by
 * ww_acquire_init()). A ww_mutex that is not inside a dma_resv is not
 * supported (ww_mutex_init() of one panics in DEBUG kernels and its lock
 * functions return -EINVAL).
 */
#ifndef _LINUX_WW_MUTEX_H
#define	_LINUX_WW_MUTEX_H

#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/lockdep.h>

struct ww_class {
	atomic64_t stamp;
	const char *acquire_name;
	const char *mutex_name;
	unsigned int is_wait_die;
};

struct ww_acquire_ctx {
	u64 stamp;
	unsigned int acquired;
	unsigned short wounded;
	unsigned short is_wait_die;
	const struct ww_class *ww_class;
	void *contending_lock;
	u64 __core[2];
};

#define	__WW_CLASS_INITIALIZER(name, wd) \
	{ .stamp = ATOMIC64_INIT(0), .acquire_name = #name "_acquire", \
	.mutex_name = #name "_mutex", .is_wait_die = (wd) }
#define	DEFINE_WW_CLASS(classname) \
	struct ww_class classname = __WW_CLASS_INITIALIZER(classname, 0)
#define	DEFINE_WD_CLASS(classname) \
	struct ww_class classname = __WW_CLASS_INITIALIZER(classname, 1)

extern void ww_acquire_init(struct ww_acquire_ctx *ctx,
    struct ww_class *ww_class);
extern void ww_acquire_done(struct ww_acquire_ctx *ctx);
extern void ww_acquire_fini(struct ww_acquire_ctx *ctx);
extern void lkpi_ww_mutex_init(struct ww_mutex *lock, struct ww_class *c);
extern int ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
extern int ww_mutex_lock_interruptible(struct ww_mutex *lock,
    struct ww_acquire_ctx *ctx);
extern void ww_mutex_lock_slow(struct ww_mutex *lock,
    struct ww_acquire_ctx *ctx);
extern int ww_mutex_lock_slow_interruptible(struct ww_mutex *lock,
    struct ww_acquire_ctx *ctx);
extern int ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
extern void ww_mutex_unlock(struct ww_mutex *lock);
extern bool ww_mutex_is_locked(struct ww_mutex *lock);

#define	ww_mutex_init(l, c)		lkpi_ww_mutex_init(l, c)
#define	ww_mutex_destroy(l)		((void)(l))

#endif
