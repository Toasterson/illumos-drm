/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's red-black trees (include/linux/rbtree.h, GPL-2.0). The
 * interface and node layout are Linux's; the algorithms are an
 * independent implementation (src/lkpi_rbtree.c, the textbook
 * insertion and deletion fix-ups) with Linux's augmentation callback
 * protocol, so <linux/rbtree_augmented.h> and interval trees work.
 */
#ifndef _LINUX_RBTREE_H
#define	_LINUX_RBTREE_H

#include <linux/types.h>
#include <linux/container_of.h>
#include <linux/compiler.h>
#include <linux/rbtree_types.h>

#define	RB_RED		0
#define	RB_BLACK	1

#define	rb_parent(r)	((struct rb_node *)((r)->__rb_parent_color & ~3UL))
#define	rb_entry(ptr, type, member)	container_of(ptr, type, member)
#define	rb_entry_safe(ptr, type, member) ({ \
	typeof (ptr) ____ptr = (ptr); \
	____ptr ? rb_entry(____ptr, type, member) : NULL; })
#define	RB_EMPTY_ROOT(root)	(READ_ONCE((root)->rb_node) == NULL)
#define	RB_EMPTY_NODE(node) \
	((node)->__rb_parent_color == (unsigned long)(node))
#define	RB_CLEAR_NODE(node) \
	((node)->__rb_parent_color = (unsigned long)(node))

struct rb_augment_callbacks {
	void (*propagate)(struct rb_node *node, struct rb_node *stop);
	void (*copy)(struct rb_node *old, struct rb_node *new_n);
	void (*rotate)(struct rb_node *old, struct rb_node *new_n);
};

extern void lkpi_rb_insert(struct rb_node *node, struct rb_root *root,
    const struct rb_augment_callbacks *aug);
extern void lkpi_rb_erase(struct rb_node *node, struct rb_root *root,
    const struct rb_augment_callbacks *aug);
extern struct rb_node *rb_next(const struct rb_node *node);
extern struct rb_node *rb_prev(const struct rb_node *node);
extern struct rb_node *rb_first(const struct rb_root *root);
extern struct rb_node *rb_last(const struct rb_root *root);
extern struct rb_node *rb_first_postorder(const struct rb_root *root);
extern struct rb_node *rb_next_postorder(const struct rb_node *node);
extern void rb_replace_node(struct rb_node *victim, struct rb_node *new_n,
    struct rb_root *root);

static inline void
rb_insert_color(struct rb_node *node, struct rb_root *root)
{
	lkpi_rb_insert(node, root, NULL);
}

static inline void
rb_erase(struct rb_node *node, struct rb_root *root)
{
	lkpi_rb_erase(node, root, NULL);
}

#define	rb_replace_node_rcu(victim, new_n, root) \
	rb_replace_node(victim, new_n, root)

static inline void
rb_link_node(struct rb_node *node, struct rb_node *parent,
    struct rb_node **rb_link)
{
	node->__rb_parent_color = (unsigned long)parent;
	node->rb_left = node->rb_right = NULL;
	*rb_link = node;
}

#define	rb_link_node_rcu(node, parent, link)	rb_link_node(node, parent, link)

#define	rbtree_postorder_for_each_entry_safe(pos, n, root, field) \
	for ((pos) = rb_entry_safe(rb_first_postorder(root), typeof (*(pos)), field); \
	    (pos) != NULL && ({ (n) = rb_entry_safe(rb_next_postorder(&(pos)->field), \
	    typeof (*(pos)), field); 1; }); \
	    (pos) = (n))

#define	rb_first_cached(root)	(root)->rb_leftmost

static inline void
rb_insert_color_cached(struct rb_node *node, struct rb_root_cached *root,
    bool leftmost)
{
	if (leftmost)
		root->rb_leftmost = node;
	rb_insert_color(node, &root->rb_root);
}

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
rb_replace_node_cached(struct rb_node *victim, struct rb_node *new_n,
    struct rb_root_cached *root)
{
	if (root->rb_leftmost == victim)
		root->rb_leftmost = new_n;
	rb_replace_node(victim, new_n, &root->rb_root);
}

static inline struct rb_node *
rb_add_cached(struct rb_node *node, struct rb_root_cached *tree,
    bool (*less)(struct rb_node *, const struct rb_node *))
{
	struct rb_node **link = &tree->rb_root.rb_node;
	struct rb_node *parent = NULL;
	bool leftmost = true;

	while (*link != NULL) {
		parent = *link;
		if (less(node, parent)) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}
	rb_link_node(node, parent, link);
	rb_insert_color_cached(node, tree, leftmost);
	return (leftmost ? node : NULL);
}

static inline void
rb_add(struct rb_node *node, struct rb_root *tree,
    bool (*less)(struct rb_node *, const struct rb_node *))
{
	struct rb_node **link = &tree->rb_node;
	struct rb_node *parent = NULL;

	while (*link != NULL) {
		parent = *link;
		if (less(node, parent))
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}
	rb_link_node(node, parent, link);
	rb_insert_color(node, tree);
}

static inline struct rb_node *
rb_find_add(struct rb_node *node, struct rb_root *tree,
    int (*cmp)(struct rb_node *, const struct rb_node *))
{
	struct rb_node **link = &tree->rb_node;
	struct rb_node *parent = NULL;
	int c;

	while (*link != NULL) {
		parent = *link;
		c = cmp(node, parent);
		if (c < 0)
			link = &parent->rb_left;
		else if (c > 0)
			link = &parent->rb_right;
		else
			return (parent);
	}
	rb_link_node(node, parent, link);
	rb_insert_color(node, tree);
	return (NULL);
}

static inline struct rb_node *
rb_find(const void *key, const struct rb_root *tree,
    int (*cmp)(const void *key, const struct rb_node *))
{
	struct rb_node *node = tree->rb_node;

	while (node != NULL) {
		int c = cmp(key, node);

		if (c < 0)
			node = node->rb_left;
		else if (c > 0)
			node = node->rb_right;
		else
			return (node);
	}
	return (NULL);
}

static inline struct rb_node *
rb_find_first(const void *key, const struct rb_root *tree,
    int (*cmp)(const void *key, const struct rb_node *))
{
	struct rb_node *node = tree->rb_node, *match = NULL;

	while (node != NULL) {
		int c = cmp(key, node);

		if (c <= 0) {
			if (c == 0)
				match = node;
			node = node->rb_left;
		} else {
			node = node->rb_right;
		}
	}
	return (match);
}

static inline struct rb_node *
rb_next_match(const void *key, struct rb_node *node,
    int (*cmp)(const void *key, const struct rb_node *))
{
	node = rb_next(node);
	if (node != NULL && cmp(key, node))
		node = NULL;
	return (node);
}

#define	rb_for_each(node, key, tree, cmp) \
	for ((node) = rb_find_first((key), (tree), (cmp)); \
	    (node) != NULL; (node) = rb_next_match((key), (node), (cmp)))

#endif
