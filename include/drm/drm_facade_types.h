/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The DRM facade of illumos-drm: the C interface of the kernel module
 * misc/drm_core. This header holds the base types that the facade structures
 * are built from.
 *
 * Origin: the types mirror the names and the role of Linux's struct
 * list_head (include/linux/types.h), struct kref and refcount_t
 * (include/linux/kref.h, refcount.h), struct rcu_head, ktime_t, the
 * ERR_PTR() family (include/linux/err.h) and container_of()
 * (include/linux/container_of.h), as of Linux v7.2. Those headers are
 * GPL-2.0; this file is a reimplementation for illumos, not a copy, and is
 * MIT-licensed (decision D7 of illumos-drm, 2026-10-01). The locks are
 * illumos kmutex_t underneath.
 */

#ifndef _DRM_FACADE_TYPES_H
#define	_DRM_FACADE_TYPES_H

#include <sys/types.h>
#include <sys/param.h>
#include <sys/stdbool.h>
#include <sys/ksynch.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/stddef.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Every facade function that has a Linux counterpart follows the Linux
 * error convention: 0 or a negative errno for int results, a pointer or
 * ERR_PTR(-errno) (or NULL, where Linux returns NULL) for pointer results.
 */
#ifndef	MAX_ERRNO
#define	MAX_ERRNO	4095
#endif
#ifndef	IS_ERR_VALUE
#define	IS_ERR_VALUE(x)	\
	((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
static inline void *
ERR_PTR(long error)
{
	return ((void *)error);
}
static inline long
PTR_ERR(const void *ptr)
{
	return ((long)ptr);
}
static inline bool
IS_ERR(const void *ptr)
{
	return (IS_ERR_VALUE((unsigned long)ptr));
}
static inline bool
IS_ERR_OR_NULL(const void *ptr)
{
	return (ptr == NULL || IS_ERR_VALUE((unsigned long)ptr));
}
#endif

#ifndef	container_of
#define	container_of(ptr, type, member) \
	((type *)(void *)((char *)(ptr) - offsetof(type, member)))
#endif

/* A doubly linked list node, Linux's struct list_head. */
struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

/* Linux's atomic_t, refcount_t and struct kref: a 32-bit count. */
typedef struct {
	volatile int counter;
} atomic_t;

typedef struct refcount_struct {
	atomic_t refs;
} refcount_t;

struct kref {
	refcount_t refcount;
};

/* An RCU callback head, Linux's struct rcu_head (unused by the core). */
struct rcu_head {
	struct rcu_head *next;
	void (*func)(struct rcu_head *);
};

/* Nanoseconds; Linux's ktime_t. The core uses gethrtime(9F) time. */
typedef int64_t ktime_t;

/*
 * The facade's lock types. Linux's spinlock_t and struct mutex become an
 * illumos kmutex_t. The illumos kernel already has a struct mutex (the
 * tag of kmutex_t), so Linux's is struct lx_mutex here. In facade
 * version 0 the core does not take the locks embedded in public
 * structures; they exist for the layout and for drivers' own use
 * (initialise them with mutex_init(9F)).
 */
typedef struct spinlock {
	kmutex_t rlock;
} spinlock_t;

struct lx_mutex {
	kmutex_t m;
};

/*
 * Linux's struct ww_mutex. In facade version 0 the wound/wait lock of a
 * reservation object lives in the core; this is its public placeholder.
 */
struct ww_mutex {
	struct lx_mutex base;
	void *ctx;
	void *ww_class;
};

/* Forward declarations of the facade's structures. */
struct drm_device;
struct drm_driver;
struct drm_file;
struct drm_minor;
struct drm_gem_object;
struct drm_gem_object_funcs;
struct drm_ioctl_desc;
struct drm_syncobj;
struct drm_mode_create_dumb;
struct dma_fence;
struct dma_fence_ops;
struct dma_fence_cb;
struct dma_fence_chain;
struct dma_resv;
struct dma_buf;
struct dma_buf_attachment;
struct ww_acquire_ctx;
struct drm_rkms_funcs;

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_FACADE_TYPES_H */
