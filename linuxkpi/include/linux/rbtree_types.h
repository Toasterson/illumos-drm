/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The types of Linux's red-black trees: struct rb_node, struct rb_root and
 * struct rb_root_cached (a root that also keeps the leftmost node).
 *
 * A node's parent pointer and colour share one word, __rb_parent_color:
 * nodes are at least word aligned, so bit 0 is free and holds the colour
 * (0 red, 1 black). Code written for Linux reads rb_left and rb_right
 * directly and tests RB_EMPTY_NODE(), so the field names and that encoding
 * are part of the interface.
 *
 * To be reconciled with R5c: written for R5e (carried helpers) before
 * R5c's compatibility layer existed; i915 needs the same trees, so R5c can
 * take this file over or replace it.
 *
 * Origin: the names and the interface follow Linux v7.2's
 * include/linux/rbtree_types.h (GPL-2.0-or-later). This file is a
 * reimplementation, not a copy (decision D7).
 */

#ifndef _LINUXKPI_LINUX_RBTREE_TYPES_H
#define	_LINUXKPI_LINUX_RBTREE_TYPES_H

struct rb_node {
	unsigned long __rb_parent_color;
	struct rb_node *rb_right;
	struct rb_node *rb_left;
} __attribute__((__aligned__(sizeof (long))));

struct rb_root {
	struct rb_node *rb_node;
};

struct rb_root_cached {
	struct rb_root rb_root;
	struct rb_node *rb_leftmost;
};

#define	RB_ROOT		((struct rb_root) { NULL })
#define	RB_ROOT_CACHED	((struct rb_root_cached) { { NULL }, NULL })

#endif	/* _LINUXKPI_LINUX_RBTREE_TYPES_H */
