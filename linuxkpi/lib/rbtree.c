/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Red-black trees with Linux's interface (linux/rbtree.h,
 * linux/rbtree_augmented.h): rebalancing after insertion and erasure,
 * with the augment callbacks of an augmented tree, and the in-order and
 * post-order walks.
 *
 * Invariants (Cormen et al., Introduction to Algorithms, chapter 13):
 * every node is red or black, the root is black, a red node has no red
 * child, and every path from a node down to a missing child passes the
 * same number of black nodes. NULL children count as black.
 *
 * The tree takes no lock and allocates nothing; its user serialises every
 * operation on one tree. Nothing here keeps global state, so distinct
 * trees may be used from any number of threads at once.
 *
 * To be reconciled with R5c: written for R5e (carried helpers) before
 * R5c's compatibility layer existed. It is compiled into misc/drm_helpers
 * with local scope; when R5c provides the trees in its own module,
 * misc/drm_helpers links against that instead.
 *
 * Origin: the interface follows Linux v7.2's lib/rbtree.c and
 * include/linux/rbtree*.h (GPL-2.0-or-later); the algorithms are the
 * textbook ones cited above, written for this file. This is a
 * reimplementation, not a copy (decision D7).
 */

#include <linux/rbtree_augmented.h>

static inline bool
lkpi_rb_is_black(const struct rb_node *node)
{
	/* A missing child is a black leaf. */
	return (node == NULL || (node->__rb_parent_color & RB_BLACK) != 0);
}

static inline bool
lkpi_rb_is_red(const struct rb_node *node)
{
	return (!lkpi_rb_is_black(node));
}

static inline unsigned long
lkpi_rb_color(const struct rb_node *node)
{
	return (node->__rb_parent_color & RB_BLACK);
}

static inline void
lkpi_rb_set_color(struct rb_node *node, unsigned long color)
{
	node->__rb_parent_color = (node->__rb_parent_color & ~RB_BLACK) | color;
}

static inline void
lkpi_rb_set_parent(struct rb_node *node, struct rb_node *parent)
{
	node->__rb_parent_color = (unsigned long)parent |
	    (node->__rb_parent_color & 3UL);
}

/* Makes `new` the child of `parent` (or the root) where `old` was. */
static inline void
lkpi_rb_change_child(struct rb_node *old, struct rb_node *new,
    struct rb_node *parent, struct rb_root *root)
{
	if (parent == NULL)
		root->rb_node = new;
	else if (parent->rb_left == old)
		parent->rb_left = new;
	else
		parent->rb_right = new;
}

/*
 * Rotations. rotate_left(x) lifts x's right child y above x; colours stay
 * with the nodes. y then roots the subtree x rooted, so an augmented tree
 * gives y x's value and recomputes x (the rotate callback).
 */
static void
lkpi_rb_rotate_left(struct rb_node *x, struct rb_root *root,
    const struct rb_augment_callbacks *augment)
{
	struct rb_node *y = x->rb_right;
	struct rb_node *parent = rb_parent(x);

	x->rb_right = y->rb_left;
	if (y->rb_left != NULL)
		lkpi_rb_set_parent(y->rb_left, x);
	y->rb_left = x;
	lkpi_rb_set_parent(y, parent);
	lkpi_rb_change_child(x, y, parent, root);
	lkpi_rb_set_parent(x, y);
	if (augment != NULL)
		augment->rotate(x, y);
}

static void
lkpi_rb_rotate_right(struct rb_node *x, struct rb_root *root,
    const struct rb_augment_callbacks *augment)
{
	struct rb_node *y = x->rb_left;
	struct rb_node *parent = rb_parent(x);

	x->rb_left = y->rb_right;
	if (y->rb_right != NULL)
		lkpi_rb_set_parent(y->rb_right, x);
	y->rb_right = x;
	lkpi_rb_set_parent(y, parent);
	lkpi_rb_change_child(x, y, parent, root);
	lkpi_rb_set_parent(x, y);
	if (augment != NULL)
		augment->rotate(x, y);
}

/*
 * Rebalances after `node` was linked as a red leaf (rb_link_node()). The
 * only possible violation is a red node with a red parent; it moves up by
 * recolouring while the uncle is red and is resolved by at most two
 * rotations otherwise.
 */
void
__lkpi_rb_insert(struct rb_node *node, struct rb_root *root,
    const struct rb_augment_callbacks *augment)
{
	struct rb_node *parent, *grand, *uncle;

	while ((parent = rb_parent(node)) != NULL && lkpi_rb_is_red(parent)) {
		/* A red parent is not the root, so it has a parent. */
		grand = rb_parent(parent);
		if (parent == grand->rb_left) {
			uncle = grand->rb_right;
			if (lkpi_rb_is_red(uncle)) {
				lkpi_rb_set_color(parent, RB_BLACK);
				lkpi_rb_set_color(uncle, RB_BLACK);
				lkpi_rb_set_color(grand, RB_RED);
				node = grand;
				continue;
			}
			if (node == parent->rb_right) {
				/* Bring the red pair onto one line. */
				lkpi_rb_rotate_left(parent, root, augment);
				node = parent;
				parent = rb_parent(node);
			}
			lkpi_rb_set_color(parent, RB_BLACK);
			lkpi_rb_set_color(grand, RB_RED);
			lkpi_rb_rotate_right(grand, root, augment);
		} else {
			uncle = grand->rb_left;
			if (lkpi_rb_is_red(uncle)) {
				lkpi_rb_set_color(parent, RB_BLACK);
				lkpi_rb_set_color(uncle, RB_BLACK);
				lkpi_rb_set_color(grand, RB_RED);
				node = grand;
				continue;
			}
			if (node == parent->rb_left) {
				lkpi_rb_rotate_right(parent, root, augment);
				node = parent;
				parent = rb_parent(node);
			}
			lkpi_rb_set_color(parent, RB_BLACK);
			lkpi_rb_set_color(grand, RB_RED);
			lkpi_rb_rotate_left(grand, root, augment);
		}
	}
	lkpi_rb_set_color(root->rb_node, RB_BLACK);
}

void
rb_insert_color(struct rb_node *node, struct rb_root *root)
{
	__lkpi_rb_insert(node, root, NULL);
}

/*
 * Exchanges the tree positions and colours of `node` and `succ`, its
 * in-order successor; `node` has two children, `succ` has no left child.
 * Afterwards `succ` sits where `node` was, and `node` where `succ` was,
 * with at most a right child.
 */
static void
lkpi_rb_swap_with_successor(struct rb_node *node, struct rb_node *succ,
    struct rb_root *root)
{
	struct rb_node *parent = rb_parent(node);
	struct rb_node *left = node->rb_left;
	struct rb_node *succ_right = succ->rb_right;
	unsigned long node_color = lkpi_rb_color(node);
	unsigned long succ_color = lkpi_rb_color(succ);

	if (succ == node->rb_right) {
		/* succ is node's right child: node becomes succ's right. */
		succ->rb_right = node;
		node->__rb_parent_color = (unsigned long)succ | succ_color;
	} else {
		struct rb_node *succ_parent = rb_parent(succ);
		struct rb_node *right = node->rb_right;

		/* succ is the left child of succ_parent, deeper down. */
		succ->rb_right = right;
		lkpi_rb_set_parent(right, succ);
		succ_parent->rb_left = node;
		node->__rb_parent_color = (unsigned long)succ_parent | succ_color;
	}
	succ->rb_left = left;
	lkpi_rb_set_parent(left, succ);
	succ->__rb_parent_color = (unsigned long)parent | node_color;
	lkpi_rb_change_child(node, succ, parent, root);

	node->rb_left = NULL;
	node->rb_right = succ_right;
	if (succ_right != NULL)
		lkpi_rb_set_parent(succ_right, node);
}

/*
 * Restores the black heights after a black node was removed from below
 * `parent`, on the side where `node` (NULL, or a black node carrying the
 * extra black) now is.
 */
static void
lkpi_rb_erase_fixup(struct rb_node *node, struct rb_node *parent,
    struct rb_root *root, const struct rb_augment_callbacks *augment)
{
	struct rb_node *sibling;

	while (node != root->rb_node && lkpi_rb_is_black(node)) {
		if (node == parent->rb_left) {
			/* The short side's sibling has a black height >= 1. */
			sibling = parent->rb_right;
			if (lkpi_rb_is_red(sibling)) {
				lkpi_rb_set_color(sibling, RB_BLACK);
				lkpi_rb_set_color(parent, RB_RED);
				lkpi_rb_rotate_left(parent, root, augment);
				sibling = parent->rb_right;
			}
			if (lkpi_rb_is_black(sibling->rb_left) &&
			    lkpi_rb_is_black(sibling->rb_right)) {
				/* Move the deficit up one level. */
				lkpi_rb_set_color(sibling, RB_RED);
				node = parent;
				parent = rb_parent(node);
				continue;
			}
			if (lkpi_rb_is_black(sibling->rb_right)) {
				lkpi_rb_set_color(sibling->rb_left, RB_BLACK);
				lkpi_rb_set_color(sibling, RB_RED);
				lkpi_rb_rotate_right(sibling, root, augment);
				sibling = parent->rb_right;
			}
			lkpi_rb_set_color(sibling, lkpi_rb_color(parent));
			lkpi_rb_set_color(parent, RB_BLACK);
			lkpi_rb_set_color(sibling->rb_right, RB_BLACK);
			lkpi_rb_rotate_left(parent, root, augment);
			node = root->rb_node;
			break;
		} else {
			sibling = parent->rb_left;
			if (lkpi_rb_is_red(sibling)) {
				lkpi_rb_set_color(sibling, RB_BLACK);
				lkpi_rb_set_color(parent, RB_RED);
				lkpi_rb_rotate_right(parent, root, augment);
				sibling = parent->rb_left;
			}
			if (lkpi_rb_is_black(sibling->rb_left) &&
			    lkpi_rb_is_black(sibling->rb_right)) {
				lkpi_rb_set_color(sibling, RB_RED);
				node = parent;
				parent = rb_parent(node);
				continue;
			}
			if (lkpi_rb_is_black(sibling->rb_left)) {
				lkpi_rb_set_color(sibling->rb_right, RB_BLACK);
				lkpi_rb_set_color(sibling, RB_RED);
				lkpi_rb_rotate_left(sibling, root, augment);
				sibling = parent->rb_left;
			}
			lkpi_rb_set_color(sibling, lkpi_rb_color(parent));
			lkpi_rb_set_color(parent, RB_BLACK);
			lkpi_rb_set_color(sibling->rb_left, RB_BLACK);
			lkpi_rb_rotate_right(parent, root, augment);
			node = root->rb_node;
			break;
		}
	}
	if (node != NULL)
		lkpi_rb_set_color(node, RB_BLACK);
}

/*
 * Removes `node`. A node with two children first trades places with its
 * successor, which then roots node's former subtree (copy callback), so
 * the node actually unlinked has at most one child. The augmented values
 * are brought up to date from the unlink point upwards before any
 * rebalancing rotation (propagate callback): first up to the successor,
 * then from the successor to the root, since the successor's copied value
 * still counts the removed node.
 */
void
__lkpi_rb_erase(struct rb_node *node, struct rb_root *root,
    const struct rb_augment_callbacks *augment)
{
	struct rb_node *succ = NULL;
	struct rb_node *child, *parent;
	unsigned long color;

	if (node->rb_left != NULL && node->rb_right != NULL) {
		succ = node->rb_right;
		while (succ->rb_left != NULL)
			succ = succ->rb_left;
		lkpi_rb_swap_with_successor(node, succ, root);
		if (augment != NULL)
			augment->copy(node, succ);
	}

	child = node->rb_left != NULL ? node->rb_left : node->rb_right;
	parent = rb_parent(node);
	color = lkpi_rb_color(node);
	lkpi_rb_change_child(node, child, parent, root);
	if (child != NULL)
		lkpi_rb_set_parent(child, parent);

	if (augment != NULL && parent != NULL) {
		if (succ != NULL) {
			augment->propagate(parent, succ);
			augment->propagate(succ, NULL);
		} else {
			augment->propagate(parent, NULL);
		}
	}

	if (color == RB_BLACK) {
		if (lkpi_rb_is_red(child))
			lkpi_rb_set_color(child, RB_BLACK);
		else
			lkpi_rb_erase_fixup(child, parent, root, augment);
	}
}

void
rb_erase(struct rb_node *node, struct rb_root *root)
{
	__lkpi_rb_erase(node, root, NULL);
}

struct rb_node *
rb_first(const struct rb_root *root)
{
	struct rb_node *node = root->rb_node;

	if (node == NULL)
		return (NULL);
	while (node->rb_left != NULL)
		node = node->rb_left;
	return (node);
}

struct rb_node *
rb_last(const struct rb_root *root)
{
	struct rb_node *node = root->rb_node;

	if (node == NULL)
		return (NULL);
	while (node->rb_right != NULL)
		node = node->rb_right;
	return (node);
}

struct rb_node *
rb_next(const struct rb_node *node)
{
	struct rb_node *parent;

	if (RB_EMPTY_NODE(node))
		return (NULL);
	if (node->rb_right != NULL) {
		node = node->rb_right;
		while (node->rb_left != NULL)
			node = node->rb_left;
		return ((struct rb_node *)node);
	}
	/* Up until we arrive from a left child. */
	while ((parent = rb_parent(node)) != NULL && node == parent->rb_right)
		node = parent;
	return (parent);
}

struct rb_node *
rb_prev(const struct rb_node *node)
{
	struct rb_node *parent;

	if (RB_EMPTY_NODE(node))
		return (NULL);
	if (node->rb_left != NULL) {
		node = node->rb_left;
		while (node->rb_right != NULL)
			node = node->rb_right;
		return ((struct rb_node *)node);
	}
	while ((parent = rb_parent(node)) != NULL && node == parent->rb_left)
		node = parent;
	return (parent);
}

/* The first node of `node`'s subtree in post-order. */
static struct rb_node *
lkpi_rb_deepest_first(const struct rb_node *node)
{
	for (;;) {
		if (node->rb_left != NULL)
			node = node->rb_left;
		else if (node->rb_right != NULL)
			node = node->rb_right;
		else
			return ((struct rb_node *)node);
	}
}

struct rb_node *
rb_first_postorder(const struct rb_root *root)
{
	if (root->rb_node == NULL)
		return (NULL);
	return (lkpi_rb_deepest_first(root->rb_node));
}

struct rb_node *
rb_next_postorder(const struct rb_node *node)
{
	struct rb_node *parent;

	if (node == NULL)
		return (NULL);
	parent = rb_parent(node);
	/* From a left child, the parent's right subtree comes next. */
	if (parent != NULL && node == parent->rb_left &&
	    parent->rb_right != NULL)
		return (lkpi_rb_deepest_first(parent->rb_right));
	return (parent);
}

void
rb_replace_node(struct rb_node *victim, struct rb_node *new,
    struct rb_root *root)
{
	struct rb_node *parent = rb_parent(victim);

	*new = *victim;
	if (victim->rb_left != NULL)
		lkpi_rb_set_parent(victim->rb_left, new);
	if (victim->rb_right != NULL)
		lkpi_rb_set_parent(victim->rb_right, new);
	lkpi_rb_change_child(victim, new, parent, root);
}
