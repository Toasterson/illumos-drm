/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's XArray interface (include/linux/xarray.h, GPL-2.0) over an
 * index-ordered red-black tree (src/lkpi_xarray.c; independent design).
 * Each present index is a node holding its entry; lookups are O(log n),
 * allocation of a free index walks the tree in order. The array's lock is
 * a layer spinlock; xa_*() take it, __xa_*() expect the caller to hold it.
 * Readers under RCU are not lock-free here: xa_load() takes the lock.
 * Entries must be pointers aligned to 4 bytes or xa_mk_value() values, as
 * on Linux; storing NULL erases, except in an allocating array
 * (XA_FLAGS_ALLOC*), where it reserves the index as on Linux (xa_load()
 * returns NULL, the allocators skip it, xa_erase()/xa_release() free it).
 * Marks are not supported (XA_MARK_* searches find nothing, xa_set_mark()
 * does nothing).
 */
#ifndef _LINUX_XARRAY_H
#define	_LINUX_XARRAY_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/rbtree.h>
#include <linux/gfp.h>
#include <linux/err.h>
#include <linux/limits.h>

struct xarray {
	spinlock_t xa_lock;
	gfp_t xa_flags;
	struct rb_root xa_root;
	unsigned long xa_count;
};

struct xa_limit {
	u32 max;
	u32 min;
};

typedef unsigned int xa_mark_t;
#define	XA_MARK_0		0U
#define	XA_MARK_1		1U
#define	XA_MARK_2		2U
#define	XA_PRESENT		8U
#define	XA_FREE_MARK		XA_MARK_0

#define	XA_FLAGS_LOCK_IRQ	0x1u
#define	XA_FLAGS_LOCK_BH	0x2u
#define	XA_FLAGS_TRACK_FREE	0x4u
#define	XA_FLAGS_ZERO_BUSY	0x8u
#define	XA_FLAGS_ALLOC_WRAPPED	0x10u
#define	XA_FLAGS_ALLOC		(XA_FLAGS_TRACK_FREE)
#define	XA_FLAGS_ALLOC1		(XA_FLAGS_TRACK_FREE | XA_FLAGS_ZERO_BUSY)

#define	XA_LIMIT(_min, _max)	((struct xa_limit) { .min = (_min), .max = (_max) })
#define	xa_limit_32b		XA_LIMIT(0, UINT_MAX)
#define	xa_limit_31b		XA_LIMIT(0, INT_MAX)
#define	xa_limit_16b		XA_LIMIT(0, USHRT_MAX)

#define	XARRAY_INIT(name, flags) { .xa_flags = (flags), .xa_root = RB_ROOT }
#define	DEFINE_XARRAY_FLAGS(name, flags)	struct xarray name = XARRAY_INIT(name, flags)
#define	DEFINE_XARRAY(name)		DEFINE_XARRAY_FLAGS(name, 0)
#define	DEFINE_XARRAY_ALLOC(name)	DEFINE_XARRAY_FLAGS(name, XA_FLAGS_ALLOC)
#define	DEFINE_XARRAY_ALLOC1(name)	DEFINE_XARRAY_FLAGS(name, XA_FLAGS_ALLOC1)

/* Value entries and error entries, Linux's encoding. */
static inline void *
xa_mk_value(unsigned long v)
{
	return ((void *)((v << 1) | 1));
}

static inline unsigned long
xa_to_value(const void *entry)
{
	return ((unsigned long)entry >> 1);
}

static inline bool
xa_is_value(const void *entry)
{
	return (((unsigned long)entry & 1) != 0);
}

static inline bool
xa_is_err(const void *entry)
{
	return (((unsigned long)entry & 3) == 2 &&
	    (unsigned long)entry >= (((unsigned long)-4095L << 2) | 2UL));
}

static inline int
xa_err(void *entry)
{
	if (xa_is_err(entry))
		return ((int)((long)entry >> 2));
	return (0);
}

static inline void *
xa_mk_err(long err)
{
	return ((void *)(((unsigned long)err << 2) | 2UL));
}

#define	xa_is_zero(e)		false
#define	xa_is_internal(e)	(((unsigned long)(e) & 3) == 2)

extern void xa_init_flags(struct xarray *xa, gfp_t flags);
extern void xa_destroy(struct xarray *xa);
extern void *xa_load(struct xarray *xa, unsigned long index);
extern void *xa_store(struct xarray *xa, unsigned long index, void *entry,
    gfp_t gfp);
extern void *__xa_store(struct xarray *xa, unsigned long index, void *entry,
    gfp_t gfp);
extern void *xa_erase(struct xarray *xa, unsigned long index);
extern void *__xa_erase(struct xarray *xa, unsigned long index);
extern void *xa_cmpxchg(struct xarray *xa, unsigned long index, void *old,
    void *entry, gfp_t gfp);
extern void *__xa_cmpxchg(struct xarray *xa, unsigned long index, void *old,
    void *entry, gfp_t gfp);
extern int xa_insert(struct xarray *xa, unsigned long index, void *entry,
    gfp_t gfp);
extern int __xa_insert(struct xarray *xa, unsigned long index, void *entry,
    gfp_t gfp);
extern int xa_alloc(struct xarray *xa, u32 *id, void *entry,
    struct xa_limit limit, gfp_t gfp);
extern int __xa_alloc(struct xarray *xa, u32 *id, void *entry,
    struct xa_limit limit, gfp_t gfp);
extern int xa_alloc_cyclic(struct xarray *xa, u32 *id, void *entry,
    struct xa_limit limit, u32 *next, gfp_t gfp);
extern int __xa_alloc_cyclic(struct xarray *xa, u32 *id, void *entry,
    struct xa_limit limit, u32 *next, gfp_t gfp);
extern void *xa_find(struct xarray *xa, unsigned long *index,
    unsigned long max, xa_mark_t filter);
extern void *xa_find_after(struct xarray *xa, unsigned long *index,
    unsigned long max, xa_mark_t filter);
extern void *lkpi_xa_slot(struct xarray *xa, unsigned long index);
extern void lkpi_xa_release(struct xarray *xa, unsigned long index);

static inline void
xa_init(struct xarray *xa)
{
	xa_init_flags(xa, 0);
}

static inline bool
xa_empty(const struct xarray *xa)
{
	return (xa->xa_root.rb_node == NULL);
}

#define	xa_lock(xa)		spin_lock(&(xa)->xa_lock)
#define	xa_unlock(xa)		spin_unlock(&(xa)->xa_lock)
#define	xa_lock_irq(xa)		spin_lock_irq(&(xa)->xa_lock)
#define	xa_unlock_irq(xa)	spin_unlock_irq(&(xa)->xa_lock)
#define	xa_lock_bh(xa)		spin_lock_bh(&(xa)->xa_lock)
#define	xa_unlock_bh(xa)	spin_unlock_bh(&(xa)->xa_lock)
#define	xa_lock_irqsave(xa, f)	spin_lock_irqsave(&(xa)->xa_lock, f)
#define	xa_unlock_irqrestore(xa, f)	spin_unlock_irqrestore(&(xa)->xa_lock, f)
#define	xa_store_irq(xa, i, e, g)	xa_store(xa, i, e, g)
#define	xa_store_bh(xa, i, e, g)	xa_store(xa, i, e, g)
#define	xa_erase_irq(xa, i)		xa_erase(xa, i)
#define	xa_erase_bh(xa, i)		xa_erase(xa, i)
#define	xa_insert_irq(xa, i, e, g)	xa_insert(xa, i, e, g)
#define	xa_alloc_irq(xa, id, e, l, g)	xa_alloc(xa, id, e, l, g)
#define	xa_alloc_cyclic_irq(xa, id, e, l, n, g)	xa_alloc_cyclic(xa, id, e, l, n, g)
#define	xa_alloc_bh(xa, id, e, l, g)	xa_alloc(xa, id, e, l, g)
#define	xa_release(xa, i)		lkpi_xa_release((xa), (i))
#define	xa_set_mark(xa, i, m)		((void)(xa), (void)(i))
#define	xa_clear_mark(xa, i, m)		((void)(xa), (void)(i))
#define	__xa_set_mark(xa, i, m)		((void)(xa), (void)(i))
#define	__xa_clear_mark(xa, i, m)	((void)(xa), (void)(i))
#define	xa_get_mark(xa, i, m)		false
#define	xa_marked(xa, m)		false

#define	xa_for_each_range(xa, index, entry, start, last) \
	for ((index) = (start), \
	    (entry) = xa_find((xa), &(index), (last), XA_PRESENT); \
	    (entry) != NULL; \
	    (entry) = xa_find_after((xa), &(index), (last), XA_PRESENT))
#define	xa_for_each_start(xa, index, entry, start) \
	xa_for_each_range(xa, index, entry, start, ULONG_MAX)
#define	xa_for_each(xa, index, entry) \
	xa_for_each_start(xa, index, entry, 0)
#define	xa_for_each_marked(xa, index, entry, filter) \
	for ((index) = 0, (entry) = NULL; (entry) != NULL; )

#endif
