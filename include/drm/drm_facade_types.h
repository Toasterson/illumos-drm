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
#include <sys/time.h>
#include <sys/debug.h>
#include <sys/atomic.h>

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

/*
 * The pointer types the public structures use for the driver's device,
 * file and node handles (facade 1.1). A driver written for illumos sees
 * the device node (dev_info_t) and opaque file pointers. A compatibility
 * layer may define these three macros before including any facade header
 * (and DRM_FACADE_INODE_T) to see its own types: the illumos LinuxKPI layer defines
 * DRM_FACADE_DEVICE_T as its struct device, DRM_FACADE_DEVICE_DIP(d) as
 * the node of such a device, DRM_FACADE_FILE_T as its struct file and
 * DRM_FACADE_INODE_T as its struct inode (drm_device.anon_inode, which the
 * core leaves NULL and never reads).
 * The core writes drm_device.dev, dma_dev and drm_minor.kdev (the node)
 * and never reads them back, so a layer may replace them with its own
 * device after drm_dev_alloc() and drm_dev_register(); the core never
 * touches drm_gem_object.filp, drm_file.filp and dma_buf.file.
 */
#ifndef	DRM_FACADE_DEVICE_T
#define	DRM_FACADE_DEVICE_T		dev_info_t
#define	DRM_FACADE_DEVICE_DIP(d)	(d)
#endif
#ifndef	DRM_FACADE_FILE_T
#define	DRM_FACADE_FILE_T		void
#endif
#ifndef	DRM_FACADE_INODE_T
#define	DRM_FACADE_INODE_T		void
#endif

/* Forward declarations of the facade's structures. */
/*
 * Allocation flags (Linux's gfp_t) for the facade's allocating functions
 * (drmm_kmalloc() and friends): __GFP_DIRECT_RECLAIM means the call may
 * sleep (KM_SLEEP; GFP_KERNEL, thread context), its absence that it must
 * not (KM_NOSLEEP; GFP_NOWAIT, GFP_ATOMIC, any context); __GFP_ZERO zeroes
 * the memory. The values are the facade's, which a Linux compatibility
 * layer must use too; other bits are ignored.
 */
#ifndef	_DRM_FACADE_GFP
#define	_DRM_FACADE_GFP
typedef unsigned int gfp_t;
#define	__GFP_ZERO		0x100u
#define	__GFP_NOWARN		0x200u
#define	__GFP_DIRECT_RECLAIM	0x400u
#define	GFP_KERNEL		(__GFP_DIRECT_RECLAIM)
#define	GFP_NOWAIT		0u
#define	GFP_ATOMIC		0u
#endif

/*
 * Linux's kref accessors on the facade's struct kref (the counts of
 * drm_gem_object and dma_fence, which are the objects' own): read the
 * count, take a reference from one held, take one unless the count is
 * zero (the object is being freed; it never comes back). A reference
 * taken here is dropped with the object's put function
 * (drm_gem_object_put(), dma_fence_put()); there is no kref_put().
 */
#ifndef	_DRM_FACADE_KREF
#define	_DRM_FACADE_KREF
static inline unsigned int
kref_read(const struct kref *kref)
{
	return ((unsigned int)*(const volatile int *)&kref->refcount.refs.counter);
}

static inline void
kref_get(struct kref *kref)
{
	atomic_inc_32((volatile uint32_t *)&kref->refcount.refs.counter);
}

static inline int
kref_get_unless_zero(struct kref *kref)
{
	volatile uint32_t *c = (volatile uint32_t *)&kref->refcount.refs.counter;
	uint32_t o;

	do {
		if ((o = *c) == 0)
			return (0);
	} while (atomic_cas_32(c, o, o + 1) != o);
	return (1);
}
#endif

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
struct drm_illumos_lkms_funcs;

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_FACADE_TYPES_H */
