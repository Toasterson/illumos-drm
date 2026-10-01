/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Fences in the illumos-drm facade.
 *
 * Origin: struct dma_fence, struct dma_fence_ops, struct dma_fence_cb and
 * the functions below mirror the names, field order and semantics of
 * Linux v7.2's include/linux/dma-fence.h. That header is GPL-2.0; this
 * file is a reimplementation for illumos from the documented interface,
 * not a copy, and is MIT-licensed (decision D7 of illumos-drm,
 * 2026-10-01). Timeouts are in clock ticks (hz), the illumos counterpart
 * of jiffies.
 */

#ifndef _DMA_FENCE_H
#define	_DMA_FENCE_H

#include <drm/drm_facade_types.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* The bits of dma_fence.flags; Linux's numbering. */
enum dma_fence_flag_bits {
	DMA_FENCE_FLAG_INITIALIZED_BIT = 0,
	DMA_FENCE_FLAG_INLINE_LOCK_BIT = 1,
	DMA_FENCE_FLAG_SIGNALED_BIT = 2,
	DMA_FENCE_FLAG_TIMESTAMP_BIT = 3,
	DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT = 4,
	DMA_FENCE_FLAG_USER_BITS = 5
};

/*
 * A fence: the completion of one piece of work. A driver embeds it in its
 * own structure and initialises it with dma_fence_init(); fences the core
 * makes (the stub fence, a sync object's fence, a fence of
 * drm_illumos_fence_create()) are allocated by the core. Either way the
 * signalling, waiting and callbacks happen in the core, which keeps the
 * fence's state behind __core.
 *
 * Which fields are maintained in facade version 0:
 *	ops, context, seqno, refcount (live: dma_fence_get() and
 *	dma_fence_put()), and flags, timestamp and error as of the last
 *	call of a facade function on this structure (they are copied from
 *	the core; read them with dma_fence_is_signaled() and
 *	dma_fence_get_status() rather than directly). The lock is not used
 *	by the core, and cb_list is not a list (the core keeps the
 *	callbacks).
 */
struct dma_fence {
	union {
		spinlock_t *extern_lock;
		spinlock_t inline_lock;
	};
	const struct dma_fence_ops *ops;
	union {
		struct list_head cb_list;
		ktime_t timestamp;
		struct rcu_head rcu;
	};
	uint64_t context;
	uint64_t seqno;
	unsigned long flags;
	struct kref refcount;
	int error;
	/* private: the core's state */
	uint64_t __core[4];
};

/*
 * A fence's callbacks. Facade version 0 calls only release: fences are
 * signalled explicitly with dma_fence_signal(), and sync files report the
 * device's driver name; get_driver_name, get_timeline_name,
 * enable_signaling, signaled, wait and set_deadline are not called yet.
 *
 * release	called when the last reference is dropped, to free the
 *		driver's structure, in the context of that dma_fence_put()
 *		(any context up to low-level interrupts, as on Linux). NULL
 *		means the core frees nothing (the driver owns the memory some
 *		other way).
 */
struct dma_fence_ops {
	const char *(*get_driver_name)(struct dma_fence *fence);
	const char *(*get_timeline_name)(struct dma_fence *fence);
	bool (*enable_signaling)(struct dma_fence *fence);
	bool (*signaled)(struct dma_fence *fence);
	long (*wait)(struct dma_fence *fence, bool intr, long timeout);
	void (*release)(struct dma_fence *fence);
	void (*set_deadline)(struct dma_fence *fence, ktime_t deadline);
};

typedef void (*dma_fence_func_t)(struct dma_fence *fence,
    struct dma_fence_cb *cb);

/*
 * A callback, embedded in the caller's structure. `node` belongs to the
 * core while the callback is added.
 */
struct dma_fence_cb {
	struct list_head node;
	dma_fence_func_t func;
};

/*
 * Initialises a fence on `context` with number `seqno`, unsignalled, with
 * one reference. `lock` may be NULL; the core does not take it.
 * May be called from any context. A fence the core cannot set up (no
 * memory in interrupt context) is initialised as signalled with
 * -ENOMEM as its error and a warning, so that nothing waits for it
 * forever.
 */
extern void dma_fence_init(struct dma_fence *fence,
    const struct dma_fence_ops *ops, spinlock_t *lock, uint64_t context,
    uint64_t seqno);

/*
 * Allocates `num` consecutive fence contexts, unique in the system.
 * Fences of these contexts may be signalled in thread context, from soft
 * interrupts and from low-level interrupt handlers.
 */
extern uint64_t dma_fence_context_alloc(unsigned int num);

/*
 * Signals the fence: records the time, wakes every waiter and runs every
 * callback, in the caller's context. Signalling twice does nothing.
 * May be called from any context up to low-level interrupts.
 */
extern void dma_fence_signal(struct dma_fence *fence);

/* Records an error for a fence that has not signalled yet. */
extern void dma_fence_set_error(struct dma_fence *fence, int error);

extern struct dma_fence *dma_fence_get(struct dma_fence *fence);

/*
 * Drops a reference; NULL is allowed. May be called from any context up
 * to low-level interrupts: the last reference releases the core's state
 * (the core fence itself is destroyed by a task queue when this was its
 * last reference too) and calls ops->release in the caller's context.
 */
extern void dma_fence_put(struct dma_fence *fence);

extern bool dma_fence_is_signaled(struct dma_fence *fence);

/*
 * 0 while unsignalled, 1 when signalled without error, the (negative)
 * error otherwise.
 */
extern int dma_fence_get_status(struct dma_fence *fence);

/*
 * Waits until the fence signals or `timeout` clock ticks have passed
 * (MAX_SCHEDULE_TIMEOUT for no limit). Returns the remaining ticks (at
 * least 1) when it signalled, 0 on timeout, -ERESTARTSYS (-EINTR) when a
 * signal interrupted an interruptible wait. Thread context only.
 */
#ifndef	MAX_SCHEDULE_TIMEOUT
#define	MAX_SCHEDULE_TIMEOUT	((long)(~0UL >> 1))
#endif
extern long dma_fence_wait_timeout(struct dma_fence *fence, bool intr,
    long timeout);

/*
 * Runs `func` when the fence signals, in the signaller's context. Returns
 * 0, or -ENOENT if the fence has signalled already (func is not called),
 * or -ENOMEM (an illumos addition: the core could not allocate its
 * callback record without sleeping). May be called from any context up
 * to low-level interrupts. `func` may drop the last reference to this
 * structure (dma_fence_put()), as on Linux; callbacks still added when
 * the last reference goes are removed and never run, and the release
 * waits for a callback function running on another processor.
 */
extern int dma_fence_add_callback(struct dma_fence *fence,
    struct dma_fence_cb *cb, dma_fence_func_t func);

/*
 * Removes a callback that has not run. Returns true if it was removed;
 * false if it has run or is running, and in that case waits until it has
 * finished. Thread context only, and not from a callback of the same
 * fence (as on Linux, where that would deadlock on the fence's lock).
 */
extern bool dma_fence_remove_callback(struct dma_fence *fence,
    struct dma_fence_cb *cb);

/* A signalled fence shared by everyone, with a new reference. */
extern struct dma_fence *dma_fence_get_stub(void);

/*
 * A preallocated point of a timeline, for drm_syncobj_add_point(). In
 * facade version 0 the core allocates the chain link itself; this only
 * reserves the call. dma_fence_chain_free() frees an unused one.
 */
extern struct dma_fence_chain *dma_fence_chain_alloc(void);
extern void dma_fence_chain_free(struct dma_fence_chain *chain);

#ifdef	__cplusplus
}
#endif

#endif	/* _DMA_FENCE_H */
