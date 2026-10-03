/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's red-black tree types (include/linux/rbtree_types.h, GPL-2.0;
 * the same layout: the parent pointer and the colour share a word).
 */
#ifndef _LINUX_RBTREE_TYPES_H
#define	_LINUX_RBTREE_TYPES_H

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

#define	RB_ROOT			((struct rb_root) { NULL, })
#define	RB_ROOT_CACHED		((struct rb_root_cached) { { NULL, }, NULL })

#endif
