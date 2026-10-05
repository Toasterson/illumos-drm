/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's XArray (<linux/xarray.h>), and the IDR and IDA on top of it
 * (<linux/idr.h>), as an index-ordered red-black tree (independent
 * design; the layer's rbtree, src/lkpi_rbtree.c). Every present index is
 * a node {index, entry}. A node whose entry is NULL is a reserved index
 * (Linux's XA_ZERO_ENTRY): xa_load() and the searches see NULL / skip it,
 * but it is occupied for xa_insert() and the allocators. As on Linux,
 * storing NULL erases, except in an allocating array (XA_FLAGS_ALLOC*),
 * where it reserves; xa_erase() always removes.
 *
 * Locking: xa_*() take xa->xa_lock, __xa_*() expect the caller to hold
 * it. Nodes are allocated with the caller's gfp (lkpi_kmflag()); the
 * xa_*() forms allocate before taking the lock. The lock is an adaptive
 * kmutex_t, so a GFP_KERNEL allocation under it may sleep as on Linux
 * (where __xa_store() drops the lock to allocate).
 *
 * Errors: xa_store()/xa_cmpxchg() return xa_mk_err(-ENOMEM/-EINVAL);
 * xa_insert() -EBUSY/-ENOMEM/-EINVAL; xa_alloc() -EBUSY when no index is
 * free, as on Linux. The IDR returns -ENOSPC when full.
 */

#include <linux/kernel.h>
#include <linux/xarray.h>
#include <linux/idr.h>
#include <linux/radix-tree.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/bug.h>
#include <linux/limits.h>

struct lkpi_xa_node {
	struct rb_node xn_rb;
	unsigned long xn_index;
	void *xn_entry;			/* NULL: reserved */
};

/* Internal stand-in for a reserved entry in comparisons (never stored). */
#define	LKPI_XA_ZERO	((void *)0x406UL)

#define	xn_of(rb)	rb_entry((rb), struct lkpi_xa_node, xn_rb)

static inline bool
lkpi_xa_track_free(const struct xarray *xa)
{
	return ((xa->xa_flags & XA_FLAGS_TRACK_FREE) != 0);
}

static inline void *
lkpi_xa_raw(const struct lkpi_xa_node *n)
{
	if (n == NULL)
		return (NULL);
	return (n->xn_entry != NULL ? n->xn_entry : LKPI_XA_ZERO);
}

static inline void *
lkpi_xa_zero_to_null(void *e)
{
	return (e == LKPI_XA_ZERO ? NULL : e);
}

static struct lkpi_xa_node *
lkpi_xa_node_alloc(gfp_t gfp)
{
	return (kmem_alloc(sizeof (struct lkpi_xa_node), lkpi_kmflag(gfp)));
}

static void
lkpi_xa_node_free(struct lkpi_xa_node *n)
{
	if (n != NULL)
		kmem_free(n, sizeof (*n));
}

static struct lkpi_xa_node *
lkpi_xa_lookup(const struct xarray *xa, unsigned long index)
{
	struct rb_node *rb = xa->xa_root.rb_node;

	while (rb != NULL) {
		struct lkpi_xa_node *n = xn_of(rb);

		if (index < n->xn_index)
			rb = rb->rb_left;
		else if (index > n->xn_index)
			rb = rb->rb_right;
		else
			return (n);
	}
	return (NULL);
}

/* The node with the smallest index >= index, or NULL. */
static struct lkpi_xa_node *
lkpi_xa_lower_bound(const struct xarray *xa, unsigned long index)
{
	struct rb_node *rb = xa->xa_root.rb_node;
	struct lkpi_xa_node *best = NULL;

	while (rb != NULL) {
		struct lkpi_xa_node *n = xn_of(rb);

		if (n->xn_index >= index) {
			best = n;
			if (n->xn_index == index)
				break;
			rb = rb->rb_left;
		} else {
			rb = rb->rb_right;
		}
	}
	return (best);
}

/* Links a node whose index is not in the tree. */
static void
lkpi_xa_link(struct xarray *xa, struct lkpi_xa_node *nn)
{
	struct rb_node **link = &xa->xa_root.rb_node, *parent = NULL;

	while (*link != NULL) {
		parent = *link;
		if (nn->xn_index < xn_of(parent)->xn_index)
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}
	rb_link_node(&nn->xn_rb, parent, link);
	rb_insert_color(&nn->xn_rb, &xa->xa_root);
	xa->xa_count++;
}

static void
lkpi_xa_unlink(struct xarray *xa, struct lkpi_xa_node *n)
{
	rb_erase(&n->xn_rb, &xa->xa_root);
	xa->xa_count--;
}

/*
 * Stores entry (NULL: reserved) at index, creating the node from *pre or
 * with gfp. Returns the previous raw entry, or an error entry.
 */
static void *
lkpi_xa_set(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp,
    struct lkpi_xa_node **pre)
{
	struct lkpi_xa_node *n = lkpi_xa_lookup(xa, index);
	void *old;

	if (n != NULL) {
		old = lkpi_xa_raw(n);
		n->xn_entry = entry;
		return (old);
	}
	if (pre != NULL && *pre != NULL) {
		n = *pre;
		*pre = NULL;
	} else {
		n = lkpi_xa_node_alloc(gfp);
		if (n == NULL)
			return (xa_mk_err(-ENOMEM));
	}
	n->xn_index = index;
	n->xn_entry = entry;
	lkpi_xa_link(xa, n);
	return (NULL);
}

/* Removes index; returns its raw entry. The node is freed. */
static void *
lkpi_xa_remove(struct xarray *xa, unsigned long index)
{
	struct lkpi_xa_node *n = lkpi_xa_lookup(xa, index);
	void *old;

	if (n == NULL)
		return (NULL);
	old = lkpi_xa_raw(n);
	lkpi_xa_unlink(xa, n);
	lkpi_xa_node_free(n);
	return (old);
}

static void *
lkpi_xa_store_locked(struct xarray *xa, unsigned long index, void *entry,
    gfp_t gfp, struct lkpi_xa_node **pre)
{
	if (WARN_ON_ONCE(xa_is_internal(entry)))
		return (xa_mk_err(-EINVAL));
	if (entry == NULL && !lkpi_xa_track_free(xa))
		return (lkpi_xa_zero_to_null(lkpi_xa_remove(xa, index)));
	return (lkpi_xa_zero_to_null(lkpi_xa_set(xa, index, entry, gfp, pre)));
}

/*
 * Compares the raw entry at index (a reserved index is not NULL) with
 * old and, if equal, stores entry (NULL erases). Returns the raw entry
 * found or an error entry.
 */
static void *
lkpi_xa_cmpxchg_locked(struct xarray *xa, unsigned long index, void *old,
    void *entry, gfp_t gfp, struct lkpi_xa_node **pre)
{
	struct lkpi_xa_node *n;
	void *cur, *r;

	if (WARN_ON_ONCE(xa_is_internal(entry) && entry != LKPI_XA_ZERO))
		return (xa_mk_err(-EINVAL));
	n = lkpi_xa_lookup(xa, index);
	cur = lkpi_xa_raw(n);
	if (cur != old)
		return (cur);
	if (entry == NULL) {
		(void) lkpi_xa_remove(xa, index);
		return (cur);
	}
	r = lkpi_xa_set(xa, index,
	    entry == LKPI_XA_ZERO ? NULL : entry, gfp, pre);
	return (xa_is_err(r) ? r : cur);
}

/*
 * The lowest index in [min, max] that has no node, by an in-order walk
 * from the first node >= min. 0 or -EBUSY.
 */
static int
lkpi_xa_find_free(const struct xarray *xa, unsigned long min,
    unsigned long max, unsigned long *idp)
{
	struct lkpi_xa_node *n;
	unsigned long cand = min;

	if (min > max)
		return (-EBUSY);
	for (n = lkpi_xa_lower_bound(xa, min); n != NULL;
	    n = rb_entry_safe(rb_next(&n->xn_rb), struct lkpi_xa_node,
	    xn_rb)) {
		if (n->xn_index > cand)
			break;
		/* n->xn_index == cand: occupied, try the next one. */
		if (cand == max)
			return (-EBUSY);
		cand++;
	}
	*idp = cand;
	return (0);
}

static int
lkpi_xa_alloc_locked(struct xarray *xa, unsigned long *idp, void *entry,
    unsigned long min, unsigned long max, gfp_t gfp,
    struct lkpi_xa_node **pre)
{
	unsigned long id;
	void *r;
	int err;

	if (WARN_ON_ONCE(!lkpi_xa_track_free(xa)))
		return (-EINVAL);
	if (WARN_ON_ONCE(xa_is_internal(entry)))
		return (-EINVAL);
	if ((xa->xa_flags & XA_FLAGS_ZERO_BUSY) && min == 0)
		min = 1;
	err = lkpi_xa_find_free(xa, min, max, &id);
	if (err != 0)
		return (err);
	r = lkpi_xa_set(xa, id, entry, gfp, pre);
	if (xa_is_err(r))
		return (xa_err(r));
	*idp = id;
	return (0);
}

/* The first entry (not reserved) at an index in [*indexp, max]. */
static void *
lkpi_xa_find_locked(struct xarray *xa, unsigned long *indexp,
    unsigned long max)
{
	struct lkpi_xa_node *n;

	for (n = lkpi_xa_lower_bound(xa, *indexp);
	    n != NULL && n->xn_index <= max;
	    n = rb_entry_safe(rb_next(&n->xn_rb), struct lkpi_xa_node,
	    xn_rb)) {
		if (n->xn_entry != NULL) {
			*indexp = n->xn_index;
			return (n->xn_entry);
		}
	}
	return (NULL);
}

/* --- the XArray interface ----------------------------------------------- */

void
xa_init_flags(struct xarray *xa, gfp_t flags)
{
	spin_lock_init(&xa->xa_lock);
	xa->xa_flags = flags;
	xa->xa_root = RB_ROOT;
	xa->xa_count = 0;
}

/* Frees every node (not the entries); the array stays usable. */
void
xa_destroy(struct xarray *xa)
{
	struct rb_root root;
	struct rb_node *rb, *next;

	xa_lock(xa);
	root = xa->xa_root;
	xa->xa_root = RB_ROOT;
	xa->xa_count = 0;
	xa->xa_flags &= ~XA_FLAGS_ALLOC_WRAPPED;
	xa_unlock(xa);

	for (rb = rb_first_postorder(&root); rb != NULL; rb = next) {
		next = rb_next_postorder(rb);
		lkpi_xa_node_free(xn_of(rb));
	}
}

void *
xa_load(struct xarray *xa, unsigned long index)
{
	struct lkpi_xa_node *n;
	void *e;

	xa_lock(xa);
	n = lkpi_xa_lookup(xa, index);
	e = n != NULL ? n->xn_entry : NULL;
	xa_unlock(xa);
	return (e);
}

void *
__xa_store(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
	return (lkpi_xa_store_locked(xa, index, entry, gfp, NULL));
}

void *
xa_store(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
	struct lkpi_xa_node *pre = NULL;
	void *old;

	if (entry != NULL || lkpi_xa_track_free(xa)) {
		pre = lkpi_xa_node_alloc(gfp);
		if (pre == NULL && (gfp & __GFP_DIRECT_RECLAIM))
			return (xa_mk_err(-ENOMEM));
	}
	xa_lock(xa);
	old = lkpi_xa_store_locked(xa, index, entry, gfp, &pre);
	xa_unlock(xa);
	lkpi_xa_node_free(pre);
	return (old);
}

void *
__xa_erase(struct xarray *xa, unsigned long index)
{
	return (lkpi_xa_zero_to_null(lkpi_xa_remove(xa, index)));
}

void *
xa_erase(struct xarray *xa, unsigned long index)
{
	void *old;

	xa_lock(xa);
	old = __xa_erase(xa, index);
	xa_unlock(xa);
	return (old);
}

void *
__xa_cmpxchg(struct xarray *xa, unsigned long index, void *old, void *entry,
    gfp_t gfp)
{
	return (lkpi_xa_zero_to_null(lkpi_xa_cmpxchg_locked(xa, index, old,
	    entry, gfp, NULL)));
}

void *
xa_cmpxchg(struct xarray *xa, unsigned long index, void *old, void *entry,
    gfp_t gfp)
{
	struct lkpi_xa_node *pre = NULL;
	void *cur;

	if (entry != NULL) {
		pre = lkpi_xa_node_alloc(gfp);
		if (pre == NULL && (gfp & __GFP_DIRECT_RECLAIM))
			return (xa_mk_err(-ENOMEM));
	}
	xa_lock(xa);
	cur = lkpi_xa_cmpxchg_locked(xa, index, old, entry, gfp, &pre);
	xa_unlock(xa);
	lkpi_xa_node_free(pre);
	return (lkpi_xa_zero_to_null(cur));
}

static int
lkpi_xa_insert_locked(struct xarray *xa, unsigned long index, void *entry,
    gfp_t gfp, struct lkpi_xa_node **pre)
{
	void *cur;

	if (WARN_ON_ONCE(xa_is_internal(entry)))
		return (-EINVAL);
	cur = lkpi_xa_cmpxchg_locked(xa, index, NULL,
	    entry != NULL ? entry : LKPI_XA_ZERO, gfp, pre);
	if (cur == NULL)
		return (0);
	if (xa_is_err(cur))
		return (xa_err(cur));
	return (-EBUSY);
}

int
__xa_insert(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
	return (lkpi_xa_insert_locked(xa, index, entry, gfp, NULL));
}

int
xa_insert(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
	struct lkpi_xa_node *pre = lkpi_xa_node_alloc(gfp);
	int err;

	if (pre == NULL && (gfp & __GFP_DIRECT_RECLAIM))
		return (-ENOMEM);
	xa_lock(xa);
	err = lkpi_xa_insert_locked(xa, index, entry, gfp, &pre);
	xa_unlock(xa);
	lkpi_xa_node_free(pre);
	return (err);
}

int
__xa_alloc(struct xarray *xa, u32 *id, void *entry, struct xa_limit limit,
    gfp_t gfp)
{
	unsigned long idx;
	int err;

	err = lkpi_xa_alloc_locked(xa, &idx, entry, limit.min, limit.max, gfp,
	    NULL);
	if (err == 0)
		*id = (u32)idx;
	return (err);
}

int
xa_alloc(struct xarray *xa, u32 *id, void *entry, struct xa_limit limit,
    gfp_t gfp)
{
	struct lkpi_xa_node *pre = lkpi_xa_node_alloc(gfp);
	unsigned long idx;
	int err;

	if (pre == NULL && (gfp & __GFP_DIRECT_RECLAIM))
		return (-ENOMEM);
	xa_lock(xa);
	err = lkpi_xa_alloc_locked(xa, &idx, entry, limit.min, limit.max, gfp,
	    &pre);
	xa_unlock(xa);
	lkpi_xa_node_free(pre);
	if (err == 0)
		*id = (u32)idx;
	return (err);
}

/*
 * Linux's cyclic allocation: the lowest free index at or after *next
 * (within the limit), else from limit.min; returns 1 instead of 0 when
 * the search wrapped, and sets *next past the new index.
 */
static int
lkpi_xa_alloc_cyclic_locked(struct xarray *xa, u32 *id, void *entry,
    struct xa_limit limit, u32 *next, gfp_t gfp, struct lkpi_xa_node **pre)
{
	unsigned long idx;
	u32 min = limit.min;
	int err;

	if (*next > min)
		min = *next;
	err = lkpi_xa_alloc_locked(xa, &idx, entry, min, limit.max, gfp, pre);
	if (err == 0 && (xa->xa_flags & XA_FLAGS_ALLOC_WRAPPED)) {
		xa->xa_flags &= ~XA_FLAGS_ALLOC_WRAPPED;
		err = 1;
	}
	if (err < 0 && err != -ENOMEM && min > limit.min) {
		err = lkpi_xa_alloc_locked(xa, &idx, entry, limit.min,
		    limit.max, gfp, pre);
		if (err == 0)
			err = 1;
	}
	if (err >= 0) {
		*id = (u32)idx;
		*next = (u32)idx + 1;
		if (*next == 0)
			xa->xa_flags |= XA_FLAGS_ALLOC_WRAPPED;
	}
	return (err);
}

int
__xa_alloc_cyclic(struct xarray *xa, u32 *id, void *entry,
    struct xa_limit limit, u32 *next, gfp_t gfp)
{
	return (lkpi_xa_alloc_cyclic_locked(xa, id, entry, limit, next, gfp,
	    NULL));
}

int
xa_alloc_cyclic(struct xarray *xa, u32 *id, void *entry,
    struct xa_limit limit, u32 *next, gfp_t gfp)
{
	struct lkpi_xa_node *pre = lkpi_xa_node_alloc(gfp);
	int err;

	if (pre == NULL && (gfp & __GFP_DIRECT_RECLAIM))
		return (-ENOMEM);
	xa_lock(xa);
	err = lkpi_xa_alloc_cyclic_locked(xa, id, entry, limit, next, gfp,
	    &pre);
	xa_unlock(xa);
	lkpi_xa_node_free(pre);
	return (err);
}

/*
 * The first entry at an index in [*index, max]; *index is set to it.
 * Marks are not supported: a mark filter finds nothing.
 */
void *
xa_find(struct xarray *xa, unsigned long *index, unsigned long max,
    xa_mark_t filter)
{
	void *e;

	if (filter != XA_PRESENT)
		return (NULL);
	xa_lock(xa);
	e = lkpi_xa_find_locked(xa, index, max);
	xa_unlock(xa);
	return (e);
}

/* As xa_find(), from the index after *index. */
void *
xa_find_after(struct xarray *xa, unsigned long *index, unsigned long max,
    xa_mark_t filter)
{
	unsigned long i;
	void *e;

	if (filter != XA_PRESENT || *index == ULONG_MAX || *index >= max)
		return (NULL);
	i = *index + 1;
	xa_lock(xa);
	e = lkpi_xa_find_locked(xa, &i, max);
	xa_unlock(xa);
	if (e != NULL)
		*index = i;
	return (e);
}

/* The address of index's entry in its node (valid until it is erased). */
void *
lkpi_xa_slot(struct xarray *xa, unsigned long index)
{
	struct lkpi_xa_node *n;

	xa_lock(xa);
	n = lkpi_xa_lookup(xa, index);
	xa_unlock(xa);
	return (n != NULL ? (void *)&n->xn_entry : NULL);
}

/* Removes a reservation (an index holding no entry); see xa_release(). */
void
lkpi_xa_release(struct xarray *xa, unsigned long index)
{
	xa_lock(xa);
	(void) lkpi_xa_cmpxchg_locked(xa, index, LKPI_XA_ZERO, NULL, 0, NULL);
	xa_unlock(xa);
}

/* --- the IDR ------------------------------------------------------------- */

/*
 * Ids are offset by idr_base: id = index + idr_base. All functions take
 * the array's lock per call; as on Linux, the caller serialises an
 * allocation with its later use of the id.
 */

void
idr_init_base(struct idr *idr, int base)
{
	xa_init_flags(&idr->idr_rt, XA_FLAGS_ALLOC);
	idr->idr_base = (unsigned int)base;
	idr->idr_next = 0;
}

int
idr_alloc_u32(struct idr *idr, void *ptr, u32 *nextid, unsigned long max,
    gfp_t gfp)
{
	unsigned long base = idr->idr_base, idx;
	unsigned long start = *nextid < base ? 0 : *nextid - base;
	struct lkpi_xa_node *pre;
	int err;

	if (max > UINT_MAX)
		max = UINT_MAX;
	if (max < base || start > max - base)
		return (-ENOSPC);
	pre = lkpi_xa_node_alloc(gfp);
	if (pre == NULL && (gfp & __GFP_DIRECT_RECLAIM))
		return (-ENOMEM);
	xa_lock(&idr->idr_rt);
	err = lkpi_xa_alloc_locked(&idr->idr_rt, &idx, ptr, start, max - base,
	    gfp, &pre);
	xa_unlock(&idr->idr_rt);
	lkpi_xa_node_free(pre);
	if (err == -EBUSY)
		return (-ENOSPC);
	if (err != 0)
		return (err);
	*nextid = (u32)(idx + base);
	return (0);
}

/* The lowest free id in [start, end) (end <= 0: INT_MAX inclusive). */
int
idr_alloc(struct idr *idr, void *ptr, int start, int end, gfp_t gfp)
{
	u32 id = (u32)start;
	int err;

	if (WARN_ON_ONCE(start < 0))
		return (-EINVAL);
	err = idr_alloc_u32(idr, ptr, &id, end > 0 ? (unsigned long)end - 1 :
	    INT_MAX, gfp);
	return (err != 0 ? err : (int)id);
}

int
idr_alloc_cyclic(struct idr *idr, void *ptr, int start, int end, gfp_t gfp)
{
	unsigned long max = end > 0 ? (unsigned long)end - 1 : INT_MAX;
	u32 id = idr->idr_next;
	int err;

	if (WARN_ON_ONCE(start < 0))
		return (-EINVAL);
	if ((int)id < start)
		id = (u32)start;
	err = idr_alloc_u32(idr, ptr, &id, max, gfp);
	if (err == -ENOSPC && id > (u32)start) {
		id = (u32)start;
		err = idr_alloc_u32(idr, ptr, &id, max, gfp);
	}
	if (err != 0)
		return (err);
	idr->idr_next = id + 1;
	return ((int)id);
}

void *
idr_find(const struct idr *idr, unsigned long id)
{
	if (id < idr->idr_base)
		return (NULL);
	return (xa_load((struct xarray *)&idr->idr_rt, id - idr->idr_base));
}

void *
idr_remove(struct idr *idr, unsigned long id)
{
	if (id < idr->idr_base)
		return (NULL);
	return (xa_erase(&idr->idr_rt, id - idr->idr_base));
}

/* The previous pointer of an allocated id, or ERR_PTR(-ENOENT). */
void *
idr_replace(struct idr *idr, void *ptr, unsigned long id)
{
	struct lkpi_xa_node *n;
	void *old;

	if (id < idr->idr_base)
		return (ERR_PTR(-ENOENT));
	if (WARN_ON_ONCE(xa_is_internal(ptr)))
		return (ERR_PTR(-EINVAL));
	xa_lock(&idr->idr_rt);
	n = lkpi_xa_lookup(&idr->idr_rt, id - idr->idr_base);
	if (n == NULL) {
		old = ERR_PTR(-ENOENT);
	} else {
		old = n->xn_entry;
		n->xn_entry = ptr;
	}
	xa_unlock(&idr->idr_rt);
	return (old);
}

void *
idr_get_next_ul(struct idr *idr, unsigned long *nextid)
{
	unsigned long base = idr->idr_base;
	unsigned long idx = *nextid < base ? 0 : *nextid - base;
	void *e;

	e = xa_find(&idr->idr_rt, &idx, ULONG_MAX, XA_PRESENT);
	if (e != NULL)
		*nextid = idx + base;
	return (e);
}

void *
idr_get_next(struct idr *idr, int *nextid)
{
	unsigned long id = *nextid < 0 ? 0 : (unsigned long)*nextid;
	void *e = idr_get_next_ul(idr, &id);

	if (e == NULL || WARN_ON_ONCE(id > INT_MAX))
		return (NULL);
	*nextid = (int)id;
	return (e);
}

/*
 * Calls fn for every id with a non-NULL pointer, in id order, until fn
 * returns non-zero (which is returned). The lock is not held across fn,
 * which may therefore remove ids.
 */
int
idr_for_each(const struct idr *idr, int (*fn)(int id, void *p, void *data),
    void *data)
{
	struct xarray *xa = (struct xarray *)&idr->idr_rt;
	unsigned long idx = 0;
	void *e;
	int ret;

	for (e = xa_find(xa, &idx, ULONG_MAX, XA_PRESENT); e != NULL;
	    e = xa_find_after(xa, &idx, ULONG_MAX, XA_PRESENT)) {
		unsigned long id = idx + idr->idr_base;

		if (WARN_ON_ONCE(id > INT_MAX))
			break;
		ret = fn((int)id, e, data);
		if (ret != 0)
			return (ret);
	}
	return (0);
}

void
idr_destroy(struct idr *idr)
{
	xa_destroy(&idr->idr_rt);
	idr->idr_next = 0;
}

/* --- the IDA ------------------------------------------------------------- */

/* Allocated ids hold this value entry, so that a free can be checked. */
#define	LKPI_IDA_USED	xa_mk_value(1)

int
ida_alloc_range(struct ida *ida, unsigned int min, unsigned int max,
    gfp_t gfp)
{
	struct lkpi_xa_node *pre;
	unsigned long idx;
	int err;

	if ((int)min < 0)
		return (-ENOSPC);
	if ((int)max < 0)
		max = INT_MAX;
	if (min > max)
		return (-ENOSPC);
	pre = lkpi_xa_node_alloc(gfp);
	if (pre == NULL && (gfp & __GFP_DIRECT_RECLAIM))
		return (-ENOMEM);
	xa_lock(&ida->xa);
	err = lkpi_xa_alloc_locked(&ida->xa, &idx, LKPI_IDA_USED, min, max,
	    gfp, &pre);
	xa_unlock(&ida->xa);
	lkpi_xa_node_free(pre);
	if (err == -EBUSY)
		return (-ENOSPC);
	return (err != 0 ? err : (int)idx);
}

void
ida_free(struct ida *ida, unsigned int id)
{
	if (WARN((int)id < 0, "ida_free: invalid id %u", id))
		return;
	if (xa_erase(&ida->xa, id) == NULL)
		WARN(1, "ida_free: id %u was not allocated", id);
}

void
ida_destroy(struct ida *ida)
{
	xa_destroy(&ida->xa);
}
