/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's augmented red-black trees (include/linux/rbtree_augmented.h,
 * GPL-2.0; reimplemented). The callbacks follow Linux's protocol:
 * propagate(node, stop) recomputes from node up to (not including) stop,
 * copy(old, new) copies the augmented value to the node taking old's
 * place, rotate(old, new) after a rotation where new took old's place.
 * Insertion: the caller updates the augmented values on the way down,
 * links the node, then calls rb_insert_augmented().
 */
#ifndef _LINUX_RBTREE_AUGMENTED_H
#define	_LINUX_RBTREE_AUGMENTED_H

#include <linux/rbtree.h>

static inline void
rb_insert_augmented(struct rb_node *node, struct rb_root *root,
    const struct rb_augment_callbacks *augment)
{
	lkpi_rb_insert(node, root, augment);
}

static inline void
rb_insert_augmented_cached(struct rb_node *node, struct rb_root_cached *root,
    bool newleft, const struct rb_augment_callbacks *augment)
{
	if (newleft)
		root->rb_leftmost = node;
	lkpi_rb_insert(node, &root->rb_root, augment);
}

static inline void
rb_erase_augmented(struct rb_node *node, struct rb_root *root,
    const struct rb_augment_callbacks *augment)
{
	lkpi_rb_erase(node, root, augment);
}

static inline void
rb_erase_augmented_cached(struct rb_node *node, struct rb_root_cached *root,
    const struct rb_augment_callbacks *augment)
{
	if (root->rb_leftmost == node)
		root->rb_leftmost = rb_next(node);
	lkpi_rb_erase(node, &root->rb_root, augment);
}

#define	RB_DECLARE_CALLBACKS(RBSTATIC, RBNAME, RBSTRUCT, RBFIELD, RBAUGMENTED, RBCOMPUTE) \
static inline void \
RBNAME ## _propagate(struct rb_node *rb, struct rb_node *stop) \
{ \
	while (rb != stop) { \
		RBSTRUCT *node = rb_entry(rb, RBSTRUCT, RBFIELD); \
		if (RBCOMPUTE(node, true)) \
			break; \
		rb = rb_parent(&node->RBFIELD); \
	} \
} \
static inline void \
RBNAME ## _copy(struct rb_node *rb_old, struct rb_node *rb_new) \
{ \
	RBSTRUCT *old = rb_entry(rb_old, RBSTRUCT, RBFIELD); \
	RBSTRUCT *new_n = rb_entry(rb_new, RBSTRUCT, RBFIELD); \
	new_n->RBAUGMENTED = old->RBAUGMENTED; \
} \
static void \
RBNAME ## _rotate(struct rb_node *rb_old, struct rb_node *rb_new) \
{ \
	RBSTRUCT *old = rb_entry(rb_old, RBSTRUCT, RBFIELD); \
	RBSTRUCT *new_n = rb_entry(rb_new, RBSTRUCT, RBFIELD); \
	new_n->RBAUGMENTED = old->RBAUGMENTED; \
	RBCOMPUTE(old, false); \
} \
RBSTATIC const struct rb_augment_callbacks RBNAME = { \
	.propagate = RBNAME ## _propagate, \
	.copy = RBNAME ## _copy, \
	.rotate = RBNAME ## _rotate \
};

#define	RB_DECLARE_CALLBACKS_MAX(RBSTATIC, RBNAME, RBSTRUCT, RBFIELD, RBTYPE, RBAUGMENTED, RBCOMPUTE) \
static inline bool \
RBNAME ## _compute_max(RBSTRUCT *node, bool exit) \
{ \
	RBSTRUCT *child; \
	RBTYPE max = RBCOMPUTE(node); \
	if (node->RBFIELD.rb_left != NULL) { \
		child = rb_entry(node->RBFIELD.rb_left, RBSTRUCT, RBFIELD); \
		if (child->RBAUGMENTED > max) \
			max = child->RBAUGMENTED; \
	} \
	if (node->RBFIELD.rb_right != NULL) { \
		child = rb_entry(node->RBFIELD.rb_right, RBSTRUCT, RBFIELD); \
		if (child->RBAUGMENTED > max) \
			max = child->RBAUGMENTED; \
	} \
	if (exit && node->RBAUGMENTED == max) \
		return (true); \
	node->RBAUGMENTED = max; \
	return (false); \
} \
RB_DECLARE_CALLBACKS(RBSTATIC, RBNAME, RBSTRUCT, RBFIELD, RBAUGMENTED, RBNAME ## _compute_max)

#endif
