/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's radix tree interface (include/linux/radix-tree.h, GPL-2.0),
 * which in Linux is a front end of the XArray; here too
 * (<linux/xarray.h>). The iterator walks present indices in order; a slot
 * is the address of an index's entry in its tree node, valid until that
 * index is deleted.
 */
#ifndef _LINUX_RADIX_TREE_H
#define	_LINUX_RADIX_TREE_H

#include <linux/xarray.h>

#define	radix_tree_root		xarray
#define	RADIX_TREE_INIT(name, mask)	XARRAY_INIT(name, mask)
#define	RADIX_TREE(name, mask)	struct radix_tree_root name = RADIX_TREE_INIT(name, mask)
#define	INIT_RADIX_TREE(root, mask)	xa_init_flags(root, mask)
#define	RADIX_TREE_MAX_TAGS	3

struct radix_tree_iter {
	unsigned long index;
	unsigned long next_index;
};

static inline int
radix_tree_insert(struct radix_tree_root *root, unsigned long index,
    void *item)
{
	return (xa_insert(root, index, item, root->xa_flags | GFP_KERNEL));
}

#define	radix_tree_lookup(root, index)	xa_load((root), (index))
#define	radix_tree_delete(root, index)	xa_erase((root), (index))
#define	radix_tree_empty(root)		xa_empty(root)
#define	radix_tree_lookup_slot(root, index) \
	((void __rcu **)lkpi_xa_slot((root), (index)))
#define	radix_tree_deref_slot(slot)	(*(slot))
#define	radix_tree_deref_slot_protected(slot, l)	(*(slot))
#define	radix_tree_exception(arg)	0
#define	radix_tree_deref_retry(arg)	0
#define	radix_tree_preload(gfp)		0
#define	radix_tree_maybe_preload(gfp)	0
#define	radix_tree_preload_end()	((void)0)
#define	radix_tree_iter_resume(slot, iter)	(slot)

static inline void __rcu **
lkpi_radix_tree_next(struct radix_tree_root *root,
    struct radix_tree_iter *iter, unsigned long start)
{
	unsigned long idx = start;
	void *e = xa_find(root, &idx, ULONG_MAX, XA_PRESENT);

	if (e == NULL)
		return (NULL);
	iter->index = idx;
	iter->next_index = idx + 1;
	return ((void __rcu **)lkpi_xa_slot(root, idx));
}

#define	radix_tree_for_each_slot(slot, root, iter, start) \
	for ((slot) = lkpi_radix_tree_next((root), (iter), (start)); \
	    (slot) != NULL; \
	    (slot) = lkpi_radix_tree_next((root), (iter), (iter)->next_index))

static inline void
radix_tree_iter_delete(struct radix_tree_root *root,
    struct radix_tree_iter *iter, void __rcu **slot)
{
	(void) xa_erase(root, iter->index);
}

#endif
