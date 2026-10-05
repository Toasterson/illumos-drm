/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Red-black trees with Linux's node layout and augmentation protocol
 * (see <linux/rbtree.h>). An independent implementation of the textbook
 * algorithms (Cormen et al., ch. 13; the insertion fix-up with its three
 * cases and the deletion fix-up with its four, each with a mirror),
 * written for the parent-and-colour word and NULL leaves. Every rotation
 * reports (old, new) to the augmentation's rotate callback; deletion
 * copies the augmented value to the successor and propagates along the
 * changed path before rebalancing.
 */

#include <linux/rbtree_augmented.h>

static inline bool
is_black(const struct rb_node *n)
{
	return (n == NULL || (n->__rb_parent_color & 1UL) != 0);
}

static inline bool
is_red(const struct rb_node *n)
{
	return (!is_black(n));
}

static inline void
set_parent(struct rb_node *n, struct rb_node *p)
{
	n->__rb_parent_color = (n->__rb_parent_color & 1UL) | (unsigned long)p;
}

static inline void
set_parent_color(struct rb_node *n, struct rb_node *p, int color)
{
	n->__rb_parent_color = (unsigned long)p | (unsigned long)color;
}

static inline void
set_black(struct rb_node *n)
{
	n->__rb_parent_color |= RB_BLACK;
}

/* Makes `new_n` the child of `parent` where `old` was (or the root). */
static inline void
change_child(struct rb_node *old, struct rb_node *new_n,
    struct rb_node *parent, struct rb_root *root)
{
	if (parent == NULL)
		WRITE_ONCE(root->rb_node, new_n);
	else if (parent->rb_left == old)
		WRITE_ONCE(parent->rb_left, new_n);
	else
		WRITE_ONCE(parent->rb_right, new_n);
}

/*
 * After a rotation in which `new_n` took the place of `old`: new_n gets
 * old's parent and colour, old becomes new_n's child with `color`.
 */
static inline void
rotate_set_parents(struct rb_node *old, struct rb_node *new_n,
    struct rb_root *root, int color)
{
	struct rb_node *parent = rb_parent(old);

	new_n->__rb_parent_color = old->__rb_parent_color;
	set_parent_color(old, new_n, color);
	change_child(old, new_n, parent, root);
}

static inline void
aug_rotate(const struct rb_augment_callbacks *aug, struct rb_node *old,
    struct rb_node *new_n)
{
	if (aug != NULL)
		aug->rotate(old, new_n);
}

void
lkpi_rb_insert(struct rb_node *node, struct rb_root *root,
    const struct rb_augment_callbacks *aug)
{
	struct rb_node *parent = rb_parent(node), *gparent, *tmp;

	/* The new node is red (rb_link_node() left the colour bit 0). */
	for (;;) {
		if (parent == NULL) {
			set_parent_color(node, NULL, RB_BLACK);
			break;
		}
		if (is_black(parent))
			break;
		gparent = rb_parent(parent);
		tmp = gparent->rb_right;
		if (parent != tmp) {
			/* parent is the left child of gparent */
			if (tmp != NULL && is_red(tmp)) {
				/* Red uncle: recolour, continue at gparent. */
				set_parent_color(tmp, gparent, RB_BLACK);
				set_parent_color(parent, gparent, RB_BLACK);
				node = gparent;
				parent = rb_parent(node);
				set_parent_color(node, parent, RB_RED);
				continue;
			}
			tmp = parent->rb_right;
			if (node == tmp) {
				/* Inner grandchild: rotate left at parent. */
				tmp = node->rb_left;
				WRITE_ONCE(parent->rb_right, tmp);
				WRITE_ONCE(node->rb_left, parent);
				if (tmp != NULL)
					set_parent_color(tmp, parent, RB_BLACK);
				set_parent_color(parent, node, RB_RED);
				aug_rotate(aug, parent, node);
				parent = node;
				tmp = node->rb_right;
			}
			/* Outer grandchild: rotate right at gparent. */
			WRITE_ONCE(gparent->rb_left, tmp);
			WRITE_ONCE(parent->rb_right, gparent);
			if (tmp != NULL)
				set_parent_color(tmp, gparent, RB_BLACK);
			rotate_set_parents(gparent, parent, root, RB_RED);
			aug_rotate(aug, gparent, parent);
			break;
		} else {
			/* parent is the right child of gparent (mirror) */
			tmp = gparent->rb_left;
			if (tmp != NULL && is_red(tmp)) {
				set_parent_color(tmp, gparent, RB_BLACK);
				set_parent_color(parent, gparent, RB_BLACK);
				node = gparent;
				parent = rb_parent(node);
				set_parent_color(node, parent, RB_RED);
				continue;
			}
			tmp = parent->rb_left;
			if (node == tmp) {
				tmp = node->rb_right;
				WRITE_ONCE(parent->rb_left, tmp);
				WRITE_ONCE(node->rb_right, parent);
				if (tmp != NULL)
					set_parent_color(tmp, parent, RB_BLACK);
				set_parent_color(parent, node, RB_RED);
				aug_rotate(aug, parent, node);
				parent = node;
				tmp = node->rb_left;
			}
			WRITE_ONCE(gparent->rb_right, tmp);
			WRITE_ONCE(parent->rb_left, gparent);
			if (tmp != NULL)
				set_parent_color(tmp, gparent, RB_BLACK);
			rotate_set_parents(gparent, parent, root, RB_RED);
			aug_rotate(aug, gparent, parent);
			break;
		}
	}
}

/*
 * Restores the colour invariants after a black node was removed below
 * `parent` (its child on the removed side is NULL or "doubly black").
 */
static void
erase_color(struct rb_node *parent, struct rb_root *root,
    const struct rb_augment_callbacks *aug)
{
	struct rb_node *node = NULL, *sibling, *tmp1, *tmp2;

	for (;;) {
		sibling = parent->rb_right;
		if (node != sibling) {
			/* node is the left child */
			if (is_red(sibling)) {
				/* Red sibling: rotate left at parent. */
				tmp1 = sibling->rb_left;
				WRITE_ONCE(parent->rb_right, tmp1);
				WRITE_ONCE(sibling->rb_left, parent);
				set_parent_color(tmp1, parent, RB_BLACK);
				rotate_set_parents(parent, sibling, root, RB_RED);
				aug_rotate(aug, parent, sibling);
				sibling = tmp1;
			}
			tmp1 = sibling->rb_right;
			if (is_black(tmp1)) {
				tmp2 = sibling->rb_left;
				if (is_black(tmp2)) {
					/* Black nephews: recolour. */
					set_parent_color(sibling, parent, RB_RED);
					if (is_red(parent)) {
						set_black(parent);
					} else {
						node = parent;
						parent = rb_parent(node);
						if (parent != NULL)
							continue;
					}
					break;
				}
				/* Near nephew red: rotate right at sibling. */
				tmp1 = tmp2->rb_right;
				WRITE_ONCE(sibling->rb_left, tmp1);
				WRITE_ONCE(tmp2->rb_right, sibling);
				WRITE_ONCE(parent->rb_right, tmp2);
				if (tmp1 != NULL)
					set_parent_color(tmp1, sibling, RB_BLACK);
				aug_rotate(aug, sibling, tmp2);
				tmp1 = sibling;
				sibling = tmp2;
			}
			/* Far nephew red: rotate left at parent. */
			tmp2 = sibling->rb_left;
			WRITE_ONCE(parent->rb_right, tmp2);
			WRITE_ONCE(sibling->rb_left, parent);
			set_parent_color(tmp1, sibling, RB_BLACK);
			if (tmp2 != NULL)
				set_parent(tmp2, parent);
			rotate_set_parents(parent, sibling, root, RB_BLACK);
			aug_rotate(aug, parent, sibling);
			break;
		} else {
			/* node is the right child (mirror) */
			sibling = parent->rb_left;
			if (is_red(sibling)) {
				tmp1 = sibling->rb_right;
				WRITE_ONCE(parent->rb_left, tmp1);
				WRITE_ONCE(sibling->rb_right, parent);
				set_parent_color(tmp1, parent, RB_BLACK);
				rotate_set_parents(parent, sibling, root, RB_RED);
				aug_rotate(aug, parent, sibling);
				sibling = tmp1;
			}
			tmp1 = sibling->rb_left;
			if (is_black(tmp1)) {
				tmp2 = sibling->rb_right;
				if (is_black(tmp2)) {
					set_parent_color(sibling, parent, RB_RED);
					if (is_red(parent)) {
						set_black(parent);
					} else {
						node = parent;
						parent = rb_parent(node);
						if (parent != NULL)
							continue;
					}
					break;
				}
				tmp1 = tmp2->rb_left;
				WRITE_ONCE(sibling->rb_right, tmp1);
				WRITE_ONCE(tmp2->rb_left, sibling);
				WRITE_ONCE(parent->rb_left, tmp2);
				if (tmp1 != NULL)
					set_parent_color(tmp1, sibling, RB_BLACK);
				aug_rotate(aug, sibling, tmp2);
				tmp1 = sibling;
				sibling = tmp2;
			}
			tmp2 = sibling->rb_right;
			WRITE_ONCE(parent->rb_left, tmp2);
			WRITE_ONCE(sibling->rb_right, parent);
			set_parent_color(tmp1, sibling, RB_BLACK);
			if (tmp2 != NULL)
				set_parent(tmp2, parent);
			rotate_set_parents(parent, sibling, root, RB_BLACK);
			aug_rotate(aug, parent, sibling);
			break;
		}
	}
}

void
lkpi_rb_erase(struct rb_node *node, struct rb_root *root,
    const struct rb_augment_callbacks *aug)
{
	struct rb_node *child = node->rb_right;
	struct rb_node *tmp = node->rb_left;
	struct rb_node *parent, *rebalance;
	unsigned long pc;

	if (tmp == NULL) {
		/* No left child: replace the node by its right child. */
		pc = node->__rb_parent_color;
		parent = rb_parent(node);
		change_child(node, child, parent, root);
		if (child != NULL) {
			/* A lone child is red; it takes the node's colour. */
			child->__rb_parent_color = pc;
			rebalance = NULL;
		} else {
			rebalance = (pc & RB_BLACK) ? parent : NULL;
		}
		tmp = parent;
	} else if (child == NULL) {
		/* Only a left child, which is red: it takes the place. */
		tmp->__rb_parent_color = pc = node->__rb_parent_color;
		parent = rb_parent(node);
		change_child(node, tmp, parent, root);
		rebalance = NULL;
		tmp = parent;
	} else {
		/* Two children: the in-order successor takes the place. */
		struct rb_node *successor = child, *child2;

		tmp = child->rb_left;
		if (tmp == NULL) {
			/* The right child is the successor. */
			parent = successor;
			child2 = successor->rb_right;
			if (aug != NULL)
				aug->copy(node, successor);
		} else {
			do {
				parent = successor;
				successor = tmp;
				tmp = tmp->rb_left;
			} while (tmp != NULL);
			child2 = successor->rb_right;
			WRITE_ONCE(parent->rb_left, child2);
			WRITE_ONCE(successor->rb_right, child);
			set_parent(child, successor);
			if (aug != NULL) {
				aug->copy(node, successor);
				aug->propagate(parent, successor);
			}
		}
		tmp = node->rb_left;
		WRITE_ONCE(successor->rb_left, tmp);
		set_parent(tmp, successor);
		pc = node->__rb_parent_color;
		tmp = rb_parent(node);
		change_child(node, successor, tmp, root);
		if (child2 != NULL) {
			set_parent_color(child2, parent, RB_BLACK);
			rebalance = NULL;
		} else {
			rebalance = is_black(successor) ? parent : NULL;
		}
		successor->__rb_parent_color = pc;
		tmp = successor;
	}
	if (aug != NULL)
		aug->propagate(tmp, NULL);
	if (rebalance != NULL)
		erase_color(rebalance, root, aug);
}

struct rb_node *
rb_first(const struct rb_root *root)
{
	struct rb_node *n = root->rb_node;

	if (n == NULL)
		return (NULL);
	while (n->rb_left != NULL)
		n = n->rb_left;
	return (n);
}

struct rb_node *
rb_last(const struct rb_root *root)
{
	struct rb_node *n = root->rb_node;

	if (n == NULL)
		return (NULL);
	while (n->rb_right != NULL)
		n = n->rb_right;
	return (n);
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

void
rb_replace_node(struct rb_node *victim, struct rb_node *new_n,
    struct rb_root *root)
{
	struct rb_node *parent = rb_parent(victim);

	*new_n = *victim;
	if (victim->rb_left != NULL)
		set_parent(victim->rb_left, new_n);
	if (victim->rb_right != NULL)
		set_parent(victim->rb_right, new_n);
	change_child(victim, new_n, parent, root);
}

static struct rb_node *
left_deepest(const struct rb_node *node)
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
rb_next_postorder(const struct rb_node *node)
{
	const struct rb_node *parent;

	if (node == NULL)
		return (NULL);
	parent = rb_parent(node);
	if (parent != NULL && node == parent->rb_left && parent->rb_right != NULL)
		return (left_deepest(parent->rb_right));
	return ((struct rb_node *)parent);
}

struct rb_node *
rb_first_postorder(const struct rb_root *root)
{
	if (root->rb_node == NULL)
		return (NULL);
	return (left_deepest(root->rb_node));
}
