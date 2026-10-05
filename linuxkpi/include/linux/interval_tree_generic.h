/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's generic interval tree: INTERVAL_TREE_DEFINE() turns a structure
 * with an rb_node into an augmented red-black tree of closed intervals
 * [ITSTART(node), ITLAST(node)], ordered by start, where each node's
 * ITSUBTREE field holds the largest ITLAST() of its subtree. It defines
 *
 *	ITSTATIC void ITPREFIX_insert(ITSTRUCT *node, struct rb_root_cached *);
 *	ITSTATIC void ITPREFIX_remove(ITSTRUCT *node, struct rb_root_cached *);
 *	ITSTATIC ITSTRUCT *ITPREFIX_iter_first(struct rb_root_cached *,
 *	    ITTYPE start, ITTYPE last);
 *	ITSTATIC ITSTRUCT *ITPREFIX_iter_next(ITSTRUCT *node,
 *	    ITTYPE start, ITTYPE last);
 *
 * and the augment callbacks ITPREFIX_augment. iter_first() returns the
 * overlapping interval with the lowest start (ties: the leftmost in the
 * tree), iter_next() the next overlapping one after `node` in tree order,
 * NULL at the end; an interval overlaps [start, last] when
 * ITSTART <= last and ITLAST >= start.
 *
 * To be reconciled with R5c: written for R5e (carried helpers) before
 * R5c's compatibility layer existed; i915 uses the same macro, so R5c can
 * take this file over or replace it.
 *
 * Origin: the macro's name, parameters and the functions it defines follow
 * Linux v7.2's include/linux/interval_tree_generic.h (GPL-2.0-or-later);
 * the search is the textbook augmented interval tree (Cormen et al.,
 * Introduction to Algorithms, section 14.3). This file is a
 * reimplementation, not a copy (decision D7).
 */

#ifndef _LINUXKPI_LINUX_INTERVAL_TREE_GENERIC_H
#define	_LINUXKPI_LINUX_INTERVAL_TREE_GENERIC_H

#include <linux/rbtree_augmented.h>

#define	INTERVAL_TREE_DEFINE(ITSTRUCT, ITRB, ITTYPE, ITSUBTREE,		\
    ITSTART, ITLAST, ITSTATIC, ITPREFIX)				\
									\
RB_DECLARE_CALLBACKS_MAX(static, ITPREFIX ## _augment, ITSTRUCT, ITRB,	\
    ITTYPE, ITSUBTREE, ITLAST)						\
									\
ITSTATIC void								\
ITPREFIX ## _insert(ITSTRUCT *node, struct rb_root_cached *root)	\
{									\
	struct rb_node **__lkpi_link = &root->rb_root.rb_node;		\
	struct rb_node *__lkpi_parent = NULL;				\
	ITTYPE __lkpi_start = ITSTART(node), __lkpi_last = ITLAST(node); \
	bool __lkpi_leftmost = true;					\
									\
	while (*__lkpi_link != NULL) {					\
		ITSTRUCT *__lkpi_p;					\
		__lkpi_parent = *__lkpi_link;				\
		__lkpi_p = rb_entry(__lkpi_parent, ITSTRUCT, ITRB);	\
		if (__lkpi_p->ITSUBTREE < __lkpi_last)			\
			__lkpi_p->ITSUBTREE = __lkpi_last;		\
		if (__lkpi_start < ITSTART(__lkpi_p)) {			\
			__lkpi_link = &__lkpi_parent->rb_left;		\
		} else {						\
			__lkpi_link = &__lkpi_parent->rb_right;		\
			__lkpi_leftmost = false;			\
		}							\
	}								\
	node->ITSUBTREE = __lkpi_last;					\
	rb_link_node(&node->ITRB, __lkpi_parent, __lkpi_link);		\
	rb_insert_augmented_cached(&node->ITRB, root, __lkpi_leftmost,	\
	    &ITPREFIX ## _augment);					\
}									\
									\
ITSTATIC void								\
ITPREFIX ## _remove(ITSTRUCT *node, struct rb_root_cached *root)	\
{									\
	rb_erase_augmented_cached(&node->ITRB, root, &ITPREFIX ## _augment); \
}									\
									\
/*									\
 * The overlapping interval with the lowest start in the subtree of	\
 * `node`, which holds at least one interval ending at or after `start`	\
 * (node->ITSUBTREE >= start).						\
 */									\
static inline __maybe_unused ITSTRUCT *					\
ITPREFIX ## _subtree_search(ITSTRUCT *node, ITTYPE start, ITTYPE last)	\
{									\
	for (;;) {							\
		struct rb_node *__lkpi_l = node->ITRB.rb_left;		\
		struct rb_node *__lkpi_r = node->ITRB.rb_right;		\
		/* Any candidate on the left starts no later: go there. */ \
		if (__lkpi_l != NULL && start <=			\
		    rb_entry(__lkpi_l, ITSTRUCT, ITRB)->ITSUBTREE) {	\
			node = rb_entry(__lkpi_l, ITSTRUCT, ITRB);	\
			continue;					\
		}							\
		/* The rest starts here or later. */			\
		if (ITSTART(node) > last)				\
			return (NULL);					\
		if (ITLAST(node) >= start)				\
			return (node);					\
		if (__lkpi_r == NULL || start >				\
		    rb_entry(__lkpi_r, ITSTRUCT, ITRB)->ITSUBTREE)	\
			return (NULL);					\
		node = rb_entry(__lkpi_r, ITSTRUCT, ITRB);		\
	}								\
}									\
									\
ITSTATIC ITSTRUCT *							\
ITPREFIX ## _iter_first(struct rb_root_cached *root,			\
    ITTYPE start, ITTYPE last)						\
{									\
	ITSTRUCT *__lkpi_node;						\
									\
	if (root->rb_root.rb_node == NULL)				\
		return (NULL);						\
	__lkpi_node = rb_entry(root->rb_root.rb_node, ITSTRUCT, ITRB);	\
	/* Every interval ends before `start`, or starts after `last`. */ \
	if (__lkpi_node->ITSUBTREE < start)				\
		return (NULL);						\
	if (ITSTART(rb_entry(root->rb_leftmost, ITSTRUCT, ITRB)) > last) \
		return (NULL);						\
	return (ITPREFIX ## _subtree_search(__lkpi_node, start, last));	\
}									\
									\
ITSTATIC ITSTRUCT *							\
ITPREFIX ## _iter_next(ITSTRUCT *node, ITTYPE start, ITTYPE last)	\
{									\
	struct rb_node *__lkpi_cur = &node->ITRB;			\
									\
	for (;;) {							\
		struct rb_node *__lkpi_r = __lkpi_cur->rb_right;	\
		struct rb_node *__lkpi_up;				\
		ITSTRUCT *__lkpi_n;					\
		/*							\
		 * The right subtree follows `cur` in order. When it holds \
		 * an interval ending at or after `start` but none overlaps, \
		 * that interval starts after `last`, and so does all that \
		 * comes later.						\
		 */							\
		if (__lkpi_r != NULL && start <=			\
		    rb_entry(__lkpi_r, ITSTRUCT, ITRB)->ITSUBTREE)	\
			return (ITPREFIX ## _subtree_search(		\
			    rb_entry(__lkpi_r, ITSTRUCT, ITRB), start, last)); \
		/* Climb to the first ancestor `cur` lies left of. */	\
		for (;;) {						\
			__lkpi_up = rb_parent(__lkpi_cur);		\
			if (__lkpi_up == NULL)				\
				return (NULL);				\
			if (__lkpi_up->rb_left == __lkpi_cur)		\
				break;					\
			__lkpi_cur = __lkpi_up;				\
		}							\
		__lkpi_cur = __lkpi_up;					\
		__lkpi_n = rb_entry(__lkpi_cur, ITSTRUCT, ITRB);	\
		if (ITSTART(__lkpi_n) > last)				\
			return (NULL);					\
		if (ITLAST(__lkpi_n) >= start)				\
			return (__lkpi_n);				\
	}								\
}

#endif	/* _LINUXKPI_LINUX_INTERVAL_TREE_GENERIC_H */
