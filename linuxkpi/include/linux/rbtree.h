/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's red-black trees: the caller walks the tree to the insertion
 * point itself, links the new node there with rb_link_node() and then
 * calls rb_insert_color() to rebalance; rb_erase() removes a node. The
 * tree never allocates and takes no lock: every operation on one tree
 * must be serialised by its user. The out-of-line functions are in
 * linuxkpi/lib/rbtree.c.
 *
 * Cached roots (struct rb_root_cached) also keep the leftmost node:
 * rb_insert_color_cached() takes whether the new node went left all the
 * way down, rb_erase_cached() moves the cache to the successor.
 *
 * To be reconciled with R5c: written for R5e (carried helpers) before
 * R5c's compatibility layer existed; i915 needs the same trees, so R5c can
 * take this file over or replace it.
 *
 * Origin: the names and the interface follow Linux v7.2's
 * include/linux/rbtree.h (GPL-2.0-or-later); the algorithms are the
 * textbook red-black tree (Cormen et al., Introduction to Algorithms,
 * chapter 13). This file is a reimplementation, not a copy (decision D7).
 */

#ifndef _LINUXKPI_LINUX_RBTREE_H
#define	_LINUXKPI_LINUX_RBTREE_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/rbtree_types.h>

#define	RB_RED		0UL
#define	RB_BLACK	1UL

#define	rb_parent(r) \
	((struct rb_node *)((r)->__rb_parent_color & ~3UL))
#define	rb_entry(ptr, type, member)	container_of(ptr, type, member)
#define	rb_entry_safe(ptr, type, member) __extension__ ({		\
	__typeof__(ptr) __lkpi_rbe = (ptr);				\
	__lkpi_rbe != NULL ? rb_entry(__lkpi_rbe, type, member) : NULL;	\
})

#define	RB_EMPTY_ROOT(root)	(READ_ONCE((root)->rb_node) == NULL)
/* A node that is in no tree points to itself (after RB_CLEAR_NODE()). */
#define	RB_EMPTY_NODE(node) \
	((node)->__rb_parent_color == (unsigned long)(node))
#define	RB_CLEAR_NODE(node) \
	((node)->__rb_parent_color = (unsigned long)(node))

extern void rb_insert_color(struct rb_node *, struct rb_root *);
extern void rb_erase(struct rb_node *, struct rb_root *);

extern struct rb_node *rb_next(const struct rb_node *);
extern struct rb_node *rb_prev(const struct rb_node *);
extern struct rb_node *rb_first(const struct rb_root *);
extern struct rb_node *rb_last(const struct rb_root *);

/* Post-order: children before their parent (for freeing a whole tree). */
extern struct rb_node *rb_first_postorder(const struct rb_root *);
extern struct rb_node *rb_next_postorder(const struct rb_node *);

/* `new` takes `victim`'s place, colour and children; no rebalancing. */
extern void rb_replace_node(struct rb_node *victim, struct rb_node *new,
    struct rb_root *root);

/* Links a red leaf `node` below `parent` at *rb_link (a child slot). */
static inline void
rb_link_node(struct rb_node *node, struct rb_node *parent,
    struct rb_node **rb_link)
{
	node->__rb_parent_color = (unsigned long)parent;
	node->rb_left = node->rb_right = NULL;
	*rb_link = node;
}

#define	rb_first_cached(root)	((root)->rb_leftmost)

static inline void
rb_insert_color_cached(struct rb_node *node, struct rb_root_cached *root,
    bool leftmost)
{
	if (leftmost)
		root->rb_leftmost = node;
	rb_insert_color(node, &root->rb_root);
}

/* Returns the new leftmost node when `node` was the leftmost, else NULL. */
static inline struct rb_node *
rb_erase_cached(struct rb_node *node, struct rb_root_cached *root)
{
	struct rb_node *leftmost = NULL;

	if (root->rb_leftmost == node)
		leftmost = root->rb_leftmost = rb_next(node);
	rb_erase(node, &root->rb_root);
	return (leftmost);
}

static inline void
rb_replace_node_cached(struct rb_node *victim, struct rb_node *new,
    struct rb_root_cached *root)
{
	if (root->rb_leftmost == victim)
		root->rb_leftmost = new;
	rb_replace_node(victim, new, &root->rb_root);
}

/*
 * Visits every entry in post-order; the body may free `pos` (the next
 * entry is fetched first).
 */
#define	rbtree_postorder_for_each_entry_safe(pos, n, root, field)	\
	for ((pos) = rb_entry_safe(rb_first_postorder(root),		\
	    __typeof__(*(pos)), field);					\
	    (pos) != NULL && ((n) = rb_entry_safe(			\
	    rb_next_postorder(&(pos)->field), __typeof__(*(pos)), field), 1); \
	    (pos) = (n))

#endif	/* _LINUXKPI_LINUX_RBTREE_H */
