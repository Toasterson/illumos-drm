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
 * of jiffies; timestamps (ktime_t) are gethrtime(9F) nanoseconds.
 */

#ifndef _DMA_FENCE_H
#define	_DMA_FENCE_H

#include <drm/drm_facade_types.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* The bits of dma_fence.flags; Linux v7.2's numbering. */
enum dma_fence_flag_bits {
	DMA_FENCE_FLAG_INITIALIZED_BIT = 0,
	DMA_FENCE_FLAG_INLINE_LOCK_BIT = 1,
	DMA_FENCE_FLAG_SEQNO64_BIT = 2,
	DMA_FENCE_FLAG_SIGNALED_BIT = 3,
	DMA_FENCE_FLAG_TIMESTAMP_BIT = 4,
	DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT = 5,
	DMA_FENCE_FLAG_USER_BITS = 6
};

/*
 * A fence: the completion of one piece of work. A driver embeds it in its
 * own structure, initialises it with dma_fence_init() and finds its
 * structure with container_of(); fences the core makes (the stub fence, a
 * sync object's fence, a fence of drm_illumos_fence_create()) are
 * allocated by the core with the same layout.
 *
 * Every public field is the fence's state, as on Linux, and a driver may
 * use it the way Linux drivers do:
 *	extern_lock/inline_lock
 *		the fence's lock (dma_fence_spinlock()): the lock passed to
 *		dma_fence_init(), or, for NULL, inline_lock, which the core
 *		initialises (DMA_FENCE_FLAG_INLINE_LOCK_BIT). It protects
 *		cb_list and the signalling; callbacks run with it held.
 *	ops	the driver's callbacks (below); kept after signalling.
 *	cb_list	the callbacks of an unsignalled fence, a list of
 *		struct dma_fence_cb linked through node, changed with the
 *		lock held. A driver may add to it and signal it directly, as
 *		i915 does; the core's own interest in a fence (a sync object,
 *		reservation, sync file or wait holding it) is one more
 *		callback on this list.
 *	timestamp
 *		replaces cb_list once DMA_FENCE_FLAG_TIMESTAMP_BIT is set.
 *	rcu	the driver's after its release callback.
 *	context, seqno, error
 *		as on Linux; error is set before the fence signals.
 *	flags	the dma_fence_flag_bits, changed atomically; bits from
 *		DMA_FENCE_FLAG_USER_BITS on belong to the driver.
 *	refcount
 *		the reference count (dma_fence_get(), dma_fence_put()).
 * __core is the core's (its link to the core's fence object and the
 * deferred-release link) and must not be touched.
 *
 * A fence's lock must be an adaptive or low-level-interrupt lock, never a
 * high-level spin mutex: the core takes its own adaptive locks while the
 * fence's lock is held (a high-level interrupt hands signalling to a soft
 * interrupt, as illumos requires anyway).
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
 * A fence's callbacks, as on Linux. All are optional, but a fence without
 * get_driver_name and get_timeline_name is named "unknown" (with a
 * warning at dma_fence_init()).
 *
 * get_driver_name, get_timeline_name
 *		names for dma_fence_driver_name() and dma_fence_timeline_name();
 *		any context.
 * enable_signaling
 *		called once, with the fence's lock held, when somebody first
 *		needs the fence to signal (a callback, a wait, the core taking
 *		the fence into a sync object, reservation or sync file, or
 *		dma_fence_enable_sw_signaling()). Returning false says the fence
 *		has signalled already; the core then signals it. Any context up
 *		to low-level interrupts.
 * signaled	polled by dma_fence_is_signaled() and dma_fence_get_status()
 *		while the signalled bit is clear; true makes the core signal
 *		the fence in the caller's context. May be called with or
 *		without the lock.
 * wait		replaces dma_fence_default_wait() for dma_fence_wait_timeout()
 *		(deprecated on Linux, supported here); thread context.
 * release	called once the last reference is gone, to free the driver's
 *		structure. NULL means the core frees nothing (the driver owns
 *		the memory some other way). The core calls it in thread
 *		context: a last reference dropped in interrupt context, or by
 *		a thread holding the fence's lock, is released later by the
 *		core's task queue (see dma_fence_put()).
 * set_deadline	called by dma_fence_set_deadline(), without the lock, any
 *		context.
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
 * A callback, embedded in the caller's structure. While it is added,
 * `node` links it into the fence's cb_list; once it is off the list (run,
 * removed or refused) `node` is empty (list_empty() is true), as on
 * Linux. Its function runs with the fence's lock held, in the
 * signaller's context, and may free the structure around the callback.
 */
struct dma_fence_cb {
	struct list_head node;
	dma_fence_func_t func;
};

/* The fence's lock (Linux's dma_fence_spinlock()). */
static inline spinlock_t *
dma_fence_spinlock(struct dma_fence *fence)
{
	if ((fence->flags & (1UL << DMA_FENCE_FLAG_INLINE_LOCK_BIT)) != 0)
		return (&fence->inline_lock);
	return (fence->extern_lock);
}

/*
 * Takes and drops the fence's lock. spinlock_t is a kmutex_t on illumos;
 * `flags` is kept for source compatibility and is not used.
 */
#define	dma_fence_lock_irqsave(fence, flags) do { \
	(flags) = 0; \
	mutex_enter(&dma_fence_spinlock(fence)->rlock); \
	} while (0)
#define	dma_fence_unlock_irqrestore(fence, flags) do { \
	(void) (flags); \
	mutex_exit(&dma_fence_spinlock(fence)->rlock); \
	} while (0)
#define	dma_fence_assert_held(fence) \
	ASSERT(MUTEX_HELD(&dma_fence_spinlock(fence)->rlock))

static inline bool
dma_fence_test_signaled_flag(struct dma_fence *fence)
{
	return ((*(volatile unsigned long *)&fence->flags &
	    (1UL << DMA_FENCE_FLAG_SIGNALED_BIT)) != 0);
}

static inline bool
dma_fence_was_initialized(struct dma_fence *fence)
{
	return (fence != NULL && (*(volatile unsigned long *)&fence->flags &
	    (1UL << DMA_FENCE_FLAG_INITIALIZED_BIT)) != 0);
}

/* Lockdep annotations on Linux; nothing on illumos. */
static inline bool
dma_fence_begin_signalling(void)
{
	return (true);
}

static inline void
dma_fence_end_signalling(bool cookie)
{
	(void) cookie;
}

static inline void
__dma_fence_might_wait(void)
{
}

/*
 * Initialises a fence on `context` with number `seqno`, unsignalled, with
 * one reference, as Linux does. `lock` may be NULL: the core then
 * initialises inline_lock (and destroys it before ops->release). Any
 * context. A fence the core cannot link to its own fence object (no
 * memory in interrupt context) is initialised as signalled with -ENOMEM
 * as its error and a warning, so that nothing waits for it forever.
 * dma_fence_init64() marks the sequence numbers as 64-bit for the later
 * comparisons.
 */
extern void dma_fence_init(struct dma_fence *fence,
    const struct dma_fence_ops *ops, spinlock_t *lock, uint64_t context,
    uint64_t seqno);
extern void dma_fence_init64(struct dma_fence *fence,
    const struct dma_fence_ops *ops, spinlock_t *lock, uint64_t context,
    uint64_t seqno);

/*
 * Allocates `num` consecutive fence contexts, unique in the system.
 */
extern uint64_t dma_fence_context_alloc(unsigned int num);

/*
 * Signals the fence: sets DMA_FENCE_FLAG_SIGNALED_BIT, replaces cb_list
 * by the timestamp and runs every callback with the lock held, in the
 * caller's context, as Linux does. Signalling twice does nothing. The
 * caller holds a reference. Any context up to low-level interrupts.
 * The _locked variants are called with the fence's lock held; the
 * _timestamp variants record `timestamp` instead of the current time.
 * dma_fence_check_and_signal() returns whether the fence had signalled
 * already.
 */
extern void dma_fence_signal(struct dma_fence *fence);
extern void dma_fence_signal_locked(struct dma_fence *fence);
extern void dma_fence_signal_timestamp(struct dma_fence *fence,
    ktime_t timestamp);
extern void dma_fence_signal_timestamp_locked(struct dma_fence *fence,
    ktime_t timestamp);
extern bool dma_fence_check_and_signal(struct dma_fence *fence);
extern bool dma_fence_check_and_signal_locked(struct dma_fence *fence);

/*
 * Records an error (negative errno) for a fence that has not signalled
 * yet; ignored, with a warning, once it has. Any context.
 */
extern void dma_fence_set_error(struct dma_fence *fence, int error);

extern struct dma_fence *dma_fence_get(struct dma_fence *fence);

/*
 * Drops a reference; NULL is allowed. Any context up to low-level
 * interrupts. The last reference releases the fence: callbacks still on
 * the list of an unsignalled fence run with -EDEADLK as the error (with a
 * warning, as on Linux), the core's state goes, an inline lock is
 * destroyed and ops->release is called. That happens at once in thread
 * context; from an interrupt handler, from a thread that holds the
 * fence's lock (a callback of this fence that drops its last reference),
 * and for a structure the core made for one of its fences whose
 * signalling was enabled (its release waits for the core's callback on
 * that fence, which may be queued behind the caller on the caller's own
 * thread: a callback of another structure of the same core fence dropping
 * this one's last reference), the release is queued for the core's task
 * queue. The core cannot tell a
 * timeout(9F) handler, quiesce(9E) or a thread holding a spin lock from a
 * thread that may block: there, use drm_illumos_fence_put_deferred()
 * (drm_illumos.h), which always queues.
 */
extern void dma_fence_put(struct dma_fence *fence);

/*
 * Whether the fence has signalled: the signalled bit, or else
 * ops->signaled(), which, when true, signals the fence in the caller's
 * context first. Any context up to low-level interrupts.
 */
extern bool dma_fence_is_signaled(struct dma_fence *fence);

/*
 * 0 while unsignalled, 1 when signalled without error, the (negative)
 * error otherwise. Polls ops->signaled() as dma_fence_is_signaled()
 * does. Any context.
 */
extern int dma_fence_get_status(struct dma_fence *fence);

/* Calls ops->enable_signaling() once (see struct dma_fence_ops). */
extern void dma_fence_enable_sw_signaling(struct dma_fence *fence);

/*
 * Waits until the fence signals or `timeout` clock ticks have passed
 * (MAX_SCHEDULE_TIMEOUT for no limit): ops->wait if the fence has one,
 * dma_fence_default_wait() otherwise, after enabling signalling. Returns
 * the remaining ticks (at least 1) when it signalled, 0 on timeout,
 * -ERESTARTSYS (-EINTR) when a signal interrupted an interruptible wait,
 * -EINVAL for a negative timeout. Thread context only.
 */
#ifndef	MAX_SCHEDULE_TIMEOUT
#define	MAX_SCHEDULE_TIMEOUT	((long)(~0UL >> 1))
#endif
extern long dma_fence_wait_timeout(struct dma_fence *fence, bool intr,
    long timeout);
extern long dma_fence_default_wait(struct dma_fence *fence, bool intr,
    long timeout);

static inline long
dma_fence_wait(struct dma_fence *fence, bool intr)
{
	long r = dma_fence_wait_timeout(fence, intr, MAX_SCHEDULE_TIMEOUT);

	return (r < 0 ? r : 0);
}

/*
 * Runs `func` when the fence signals, in the signaller's context with
 * the fence's lock held. Returns 0, or -ENOENT if the fence has signalled
 * already (func is not called, node is left empty), -EINVAL for NULL.
 * Enables signalling first. Any context up to low-level interrupts.
 */
extern int dma_fence_add_callback(struct dma_fence *fence,
    struct dma_fence_cb *cb, dma_fence_func_t func);

/*
 * Removes a callback that has not run: true if it was removed, false if
 * it had run (or was never added). It takes the fence's lock, so a
 * callback that is running has finished when this returns. Any context,
 * but not from a callback of the same fence (the lock is held there).
 */
extern bool dma_fence_remove_callback(struct dma_fence *fence,
    struct dma_fence_cb *cb);

/* Passes a deadline to ops->set_deadline; any context. */
extern void dma_fence_set_deadline(struct dma_fence *fence,
    ktime_t deadline);

/*
 * The names of the fence's driver and timeline: ops->get_driver_name()
 * and ops->get_timeline_name(), "unknown" without them. Any context.
 */
extern const char *dma_fence_driver_name(struct dma_fence *fence);
extern const char *dma_fence_timeline_name(struct dma_fence *fence);

/* A signalled fence shared by everyone, with a new reference. */
extern struct dma_fence *dma_fence_get_stub(void);

/*
 * The sequence-number comparisons of Linux: whether f1 is later than f2
 * on the fence's context, wraparound-safe (32-bit unless the fence was
 * made with dma_fence_init64()).
 */
static inline bool
__dma_fence_is_later(struct dma_fence *fence, uint64_t f1, uint64_t f2)
{
	if ((fence->flags & (1UL << DMA_FENCE_FLAG_SEQNO64_BIT)) != 0)
		return (f1 > f2);
	return ((int32_t)((uint32_t)f1 - (uint32_t)f2) > 0);
}

static inline bool
dma_fence_is_later(struct dma_fence *f1, struct dma_fence *f2)
{
	if (f1->context != f2->context)
		return (false);
	return (__dma_fence_is_later(f1, f1->seqno, f2->seqno));
}

static inline bool
dma_fence_is_later_or_same(struct dma_fence *f1, struct dma_fence *f2)
{
	return (f1 == f2 || dma_fence_is_later(f1, f2));
}

static inline struct dma_fence *
dma_fence_later(struct dma_fence *f1, struct dma_fence *f2)
{
	if (f1->context != f2->context)
		return (NULL);
	return (dma_fence_is_later(f1, f2) ? f1 : f2);
}

/*
 * Whether the fence has signalled, polling ops->signaled() and signalling
 * it on success; with the fence's lock held.
 */
static inline bool
dma_fence_is_signaled_locked(struct dma_fence *fence)
{
	if (dma_fence_test_signaled_flag(fence))
		return (true);
	if (fence->ops != NULL && fence->ops->signaled != NULL &&
	    fence->ops->signaled(fence)) {
		dma_fence_signal_locked(fence);
		return (true);
	}
	return (false);
}

static inline int
dma_fence_get_status_locked(struct dma_fence *fence)
{
	if (dma_fence_is_signaled_locked(fence))
		return (fence->error < 0 ? fence->error : 1);
	return (0);
}

/*
 * When the fence signalled (its timestamp, or the current time while the
 * timestamp is being set).
 */
static inline ktime_t
dma_fence_timestamp(struct dma_fence *fence)
{
	if ((*(volatile unsigned long *)&fence->flags &
	    (1UL << DMA_FENCE_FLAG_TIMESTAMP_BIT)) == 0)
		return ((ktime_t)gethrtime());
	return (fence->timestamp);
}

/*
 * A preallocated point of a timeline, for drm_syncobj_add_point(). The
 * core allocates the chain link itself; this only reserves the call.
 * dma_fence_chain_free() frees an unused one.
 */
extern struct dma_fence_chain *dma_fence_chain_alloc(void);
extern void dma_fence_chain_free(struct dma_fence_chain *chain);

#ifdef	__cplusplus
}
#endif

#endif	/* _DMA_FENCE_H */
