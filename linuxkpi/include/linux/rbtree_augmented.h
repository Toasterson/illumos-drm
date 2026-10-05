/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Augmented red-black trees, Linux's interface: every node carries a value
 * computed from its own data and its children's values (a subtree
 * maximum, for instance), and three callbacks keep it right while the tree
 * changes shape:
 *
 *  - propagate(node, stop): recompute the value of `node` and of its
 *    ancestors, up to but not including `stop` (NULL: the root); it may
 *    stop early once a value comes out unchanged;
 *  - copy(old, new): `new` has taken `old`'s place over the same set of
 *    nodes, so it takes `old`'s value;
 *  - rotate(old, new): a rotation moved `new` above `old`: `new` takes
 *    `old`'s value and `old`'s value is recomputed.
 *
 * Insertion: the caller walks down, updating the values of the nodes it
 * passes so that they include the new node, links the node and calls
 * rb_insert_augmented(); the rebalancing calls rotate() only. Erasure:
 * rb_erase_augmented() unlinks the node, calls copy() and propagate() for
 * the nodes whose subtree changed, and rotate() while rebalancing. Values
 * are correct after either call returns.
 *
 * RB_DECLARE_CALLBACKS() and RB_DECLARE_CALLBACKS_MAX() generate the three
 * callbacks and the struct rb_augment_callbacks named RBNAME that
 * holds them.
 *
 * To be reconciled with R5c: written for R5e (carried helpers) before
 * R5c's compatibility layer existed; i915 needs the same trees, so R5c can
 * take this file over or replace it.
 *
 * Origin: the names and the interface follow Linux v7.2's
 * include/linux/rbtree_augmented.h (GPL-2.0-or-later). This file is a
 * reimplementation, not a copy (decision D7).
 */

#ifndef _LINUXKPI_LINUX_RBTREE_AUGMENTED_H
#define	_LINUXKPI_LINUX_RBTREE_AUGMENTED_H

#include <linux/compiler.h>
#include <linux/rbtree.h>

struct rb_augment_callbacks {
	void (*propagate)(struct rb_node *node, struct rb_node *stop);
	void (*copy)(struct rb_node *old, struct rb_node *new);
	void (*rotate)(struct rb_node *old, struct rb_node *new);
};

/*
 * The out-of-line rebalancing of linuxkpi/lib/rbtree.c; `augment` may be
 * NULL for a plain tree. Not Linux names (Linux inlines these steps).
 */
extern void __lkpi_rb_insert(struct rb_node *node, struct rb_root *root,
    const struct rb_augment_callbacks *augment);
extern void __lkpi_rb_erase(struct rb_node *node, struct rb_root *root,
    const struct rb_augment_callbacks *augment);

static inline void
rb_insert_augmented(struct rb_node *node, struct rb_root *root,
    const struct rb_augment_callbacks *augment)
{
	__lkpi_rb_insert(node, root, augment);
}

static inline void
rb_insert_augmented_cached(struct rb_node *node,
    struct rb_root_cached *root, bool newleft,
    const struct rb_augment_callbacks *augment)
{
	if (newleft)
		root->rb_leftmost = node;
	__lkpi_rb_insert(node, &root->rb_root, augment);
}

static inline void
rb_erase_augmented(struct rb_node *node, struct rb_root *root,
    const struct rb_augment_callbacks *augment)
{
	__lkpi_rb_erase(node, root, augment);
}

static inline void
rb_erase_augmented_cached(struct rb_node *node, struct rb_root_cached *root,
    const struct rb_augment_callbacks *augment)
{
	if (root->rb_leftmost == node)
		root->rb_leftmost = rb_next(node);
	__lkpi_rb_erase(node, &root->rb_root, augment);
}

/*
 * RBCOMPUTE(node, exit) recomputes node->RBAUGMENTED from the node and its
 * children and returns true when `exit` is set and the value did not
 * change (propagation may stop there).
 */
#define	RB_DECLARE_CALLBACKS(RBSTATIC, RBNAME, RBSTRUCT, RBFIELD,	\
    RBAUGMENTED, RBCOMPUTE)						\
static inline void							\
RBNAME ## _propagate(struct rb_node *rb, struct rb_node *stop)		\
{									\
	for (; rb != stop; rb = rb_parent(rb)) {			\
		if (RBCOMPUTE(rb_entry(rb, RBSTRUCT, RBFIELD), true))	\
			break;						\
	}								\
}									\
static inline void							\
RBNAME ## _copy(struct rb_node *rb_old, struct rb_node *rb_new)		\
{									\
	rb_entry(rb_new, RBSTRUCT, RBFIELD)->RBAUGMENTED =		\
	    rb_entry(rb_old, RBSTRUCT, RBFIELD)->RBAUGMENTED;		\
}									\
static void								\
RBNAME ## _rotate(struct rb_node *rb_old, struct rb_node *rb_new)	\
{									\
	RBSTRUCT *__lkpi_old = rb_entry(rb_old, RBSTRUCT, RBFIELD);	\
	rb_entry(rb_new, RBSTRUCT, RBFIELD)->RBAUGMENTED =		\
	    __lkpi_old->RBAUGMENTED;					\
	(void) RBCOMPUTE(__lkpi_old, false);				\
}									\
RBSTATIC const struct rb_augment_callbacks RBNAME = {			\
	.propagate = RBNAME ## _propagate,				\
	.copy = RBNAME ## _copy,					\
	.rotate = RBNAME ## _rotate					\
};

/*
 * The common case: node->RBAUGMENTED (of type RBTYPE) is the maximum of
 * RBCOMPUTE(node) (the node's own value, an expression of the node) over
 * the node's subtree.
 */
#define	RB_DECLARE_CALLBACKS_MAX(RBSTATIC, RBNAME, RBSTRUCT, RBFIELD,	\
    RBTYPE, RBAUGMENTED, RBCOMPUTE)					\
static inline bool							\
RBNAME ## _compute_max(RBSTRUCT *node, bool exit)			\
{									\
	RBSTRUCT *__lkpi_c;						\
	RBTYPE __lkpi_max = RBCOMPUTE(node);				\
	if (node->RBFIELD.rb_left != NULL) {				\
		__lkpi_c = rb_entry(node->RBFIELD.rb_left, RBSTRUCT, RBFIELD); \
		if (__lkpi_c->RBAUGMENTED > __lkpi_max)			\
			__lkpi_max = __lkpi_c->RBAUGMENTED;		\
	}								\
	if (node->RBFIELD.rb_right != NULL) {				\
		__lkpi_c = rb_entry(node->RBFIELD.rb_right, RBSTRUCT, RBFIELD); \
		if (__lkpi_c->RBAUGMENTED > __lkpi_max)			\
			__lkpi_max = __lkpi_c->RBAUGMENTED;		\
	}								\
	if (exit && node->RBAUGMENTED == __lkpi_max)			\
		return (true);						\
	node->RBAUGMENTED = __lkpi_max;					\
	return (false);							\
}									\
RB_DECLARE_CALLBACKS(RBSTATIC, RBNAME, RBSTRUCT, RBFIELD,		\
    RBAUGMENTED, RBNAME ## _compute_max)

#endif	/* _LINUXKPI_LINUX_RBTREE_AUGMENTED_H */
