/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Tests of src/lkpi_rbtree.c: random insertions and erasures checked
 * against the red-black invariants and a reference set, the iterators,
 * rb_replace_node(), the cached-leftmost forms, and an interval tree
 * made with INTERVAL_TREE_DEFINE() checked against brute force.
 */

#include <linux/kernel.h>
#include <linux/rbtree.h>
#include <linux/rbtree_augmented.h>
#include <linux/interval_tree_generic.h>
#include "test.h"

#define	NKEYS	2048

struct knode {
	struct rb_node rb;
	unsigned long key;
	bool in;
};

static struct knode knodes[NKEYS];

static bool
rb_red(const struct rb_node *n)
{
	return (n != NULL && (n->__rb_parent_color & 1UL) == 0);
}

/*
 * Checks the subtree: order, parent links, no red-red, equal black
 * heights; returns the black height or -1.
 */
static int
rb_check(const struct rb_node *n, const struct rb_node *parent,
    const unsigned long *lo, const unsigned long *hi, size_t *count)
{
	const struct knode *k;
	int lh, rh;

	if (n == NULL)
		return (1);
	k = rb_entry(n, struct knode, rb);
	if (rb_parent(n) != parent)
		return (-1);
	if ((lo != NULL && k->key <= *lo) || (hi != NULL && k->key >= *hi))
		return (-1);
	if (rb_red(n) && (rb_red(n->rb_left) || rb_red(n->rb_right)))
		return (-1);
	(*count)++;
	lh = rb_check(n->rb_left, n, lo, &k->key, count);
	rh = rb_check(n->rb_right, n, &k->key, hi, count);
	if (lh < 0 || rh < 0 || lh != rh)
		return (-1);
	return (lh + (rb_red(n) ? 0 : 1));
}

static void
k_insert(struct rb_root *root, struct knode *nn)
{
	struct rb_node **link = &root->rb_node, *parent = NULL;

	while (*link != NULL) {
		parent = *link;
		if (nn->key < rb_entry(parent, struct knode, rb)->key)
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}
	rb_link_node(&nn->rb, parent, link);
	rb_insert_color(&nn->rb, root);
	nn->in = true;
}

static bool
tree_ok(struct rb_root *root, size_t expect)
{
	size_t count = 0;

	if (rb_red(root->rb_node))
		return (false);
	if (rb_check(root->rb_node, NULL, NULL, NULL, &count) < 0)
		return (false);
	return (count == expect);
}

TEST(rbtree_random)
{
	struct rb_root root = RB_ROOT;
	unsigned long long seed = 0x1234567;
	size_t live = 0, i, round;
	bool ok = true;

	for (i = 0; i < NKEYS; i++) {
		knodes[i].key = i * 2;
		knodes[i].in = false;
	}
	for (round = 0; round < 20000; round++) {
		struct knode *k = &knodes[t_rand(&seed) % NKEYS];

		if (k->in) {
			rb_erase(&k->rb, &root);
			k->in = false;
			live--;
		} else {
			k_insert(&root, k);
			live++;
		}
		if ((round % 97) == 0 && !tree_ok(&root, live))
			ok = false;
	}
	CHECK(ok);
	CHECK(tree_ok(&root, live));

	/* In-order iteration equals the reference set, both directions. */
	{
		struct rb_node *n;
		unsigned long prev = 0;
		size_t seen = 0;
		bool order = true;

		for (n = rb_first(&root); n != NULL; n = rb_next(n)) {
			struct knode *k = rb_entry(n, struct knode, rb);

			if (seen != 0 && k->key <= prev)
				order = false;
			if (!k->in)
				order = false;
			prev = k->key;
			seen++;
		}
		CHECK(order);
		CHECK_EQ(seen, live);
		seen = 0;
		for (n = rb_last(&root); n != NULL; n = rb_prev(n)) {
			struct knode *k = rb_entry(n, struct knode, rb);

			if (seen != 0 && k->key >= prev)
				order = false;
			prev = k->key;
			seen++;
		}
		CHECK(order);
		CHECK_EQ(seen, live);
	}

	/* Post-order: every node after its children, all visited. */
	{
		struct rb_node *n;
		size_t seen = 0;
		bool post = true;
		static bool done[NKEYS];

		memset(done, 0, sizeof (done));
		for (n = rb_first_postorder(&root); n != NULL;
		    n = rb_next_postorder(n)) {
			struct knode *k = rb_entry(n, struct knode, rb);

			if (n->rb_left != NULL && !done[rb_entry(n->rb_left,
			    struct knode, rb) - knodes])
				post = false;
			if (n->rb_right != NULL && !done[rb_entry(n->rb_right,
			    struct knode, rb) - knodes])
				post = false;
			done[k - knodes] = true;
			seen++;
		}
		CHECK(post);
		CHECK_EQ(seen, live);
	}

	/* Erase everything in random order. */
	for (i = 0; i < NKEYS; i++) {
		struct knode *k = &knodes[(i * 7919) % NKEYS];

		if (k->in) {
			rb_erase(&k->rb, &root);
			k->in = false;
			live--;
			if ((i % 61) == 0 && !tree_ok(&root, live))
				ok = false;
		}
	}
	CHECK(ok);
	CHECK_EQ(live, 0);
	CHECK(RB_EMPTY_ROOT(&root));
	CHECK(rb_first(&root) == NULL);
	CHECK(rb_first_postorder(&root) == NULL);
}

TEST(rbtree_replace_cached)
{
	struct rb_root_cached root = RB_ROOT_CACHED;
	struct knode n[8], repl;
	struct rb_node *rb;
	size_t count = 0;
	int i;

	for (i = 0; i < 8; i++) {
		struct rb_node **link = &root.rb_root.rb_node, *parent = NULL;
		bool leftmost = true;

		n[i].key = (unsigned long)(10 * (8 - i));
		while (*link != NULL) {
			parent = *link;
			if (n[i].key < rb_entry(parent, struct knode, rb)->key) {
				link = &parent->rb_left;
			} else {
				link = &parent->rb_right;
				leftmost = false;
			}
		}
		rb_link_node(&n[i].rb, parent, link);
		rb_insert_color_cached(&n[i].rb, &root, leftmost);
	}
	CHECK(rb_first_cached(&root) == &n[7].rb);
	CHECK(rb_first_cached(&root) == rb_first(&root.rb_root));

	/* Replace the node with key 40 by one with the same key. */
	repl.key = 40;
	rb_replace_node(&n[4].rb, &repl.rb, &root.rb_root);
	CHECK(rb_check(root.rb_root.rb_node, NULL, NULL, NULL, &count) > 0);
	CHECK_EQ(count, 8);
	for (rb = rb_first(&root.rb_root); rb != NULL; rb = rb_next(rb)) {
		if (rb_entry(rb, struct knode, rb)->key == 40)
			CHECK(rb == &repl.rb);
	}

	/* Erasing the leftmost moves the cache to the next. */
	CHECK(rb_erase_cached(&n[7].rb, &root) == &n[6].rb);
	CHECK(rb_first_cached(&root) == &n[6].rb);
	(void) rb_erase_cached(&n[0].rb, &root);
	CHECK(rb_first_cached(&root) == &n[6].rb);
	count = 0;
	CHECK(rb_check(root.rb_root.rb_node, NULL, NULL, NULL, &count) > 0);
	CHECK_EQ(count, 6);
}

/* --- interval tree -------------------------------------------------------- */

struct itnode {
	struct rb_node rb;
	unsigned long start, last, subtree_last;
	bool in;
};

#define	IT_START(n)	((n)->start)
#define	IT_LAST(n)	((n)->last)

INTERVAL_TREE_DEFINE(struct itnode, rb, unsigned long, subtree_last,
    IT_START, IT_LAST, static, itt)

#define	NIT	600

static struct itnode its[NIT];

/* The augmented value of every node is the max `last` of its subtree. */
static unsigned long
it_check(const struct rb_node *n, bool *ok)
{
	const struct itnode *k;
	unsigned long m, l;

	if (n == NULL)
		return (0);
	k = rb_entry(n, struct itnode, rb);
	m = k->last;
	l = it_check(n->rb_left, ok);
	if (l > m)
		m = l;
	l = it_check(n->rb_right, ok);
	if (l > m)
		m = l;
	if (k->subtree_last != m)
		*ok = false;
	return (m);
}

TEST(interval_tree)
{
	struct rb_root_cached root = RB_ROOT_CACHED;
	unsigned long long seed = 0xdeadbeef;
	size_t round, i;
	bool aug_ok = true, query_ok = true;

	for (i = 0; i < NIT; i++) {
		its[i].start = t_rand(&seed) % 10000;
		its[i].last = its[i].start + t_rand(&seed) % 300;
		its[i].in = false;
	}
	for (round = 0; round < 6000; round++) {
		struct itnode *k = &its[t_rand(&seed) % NIT];

		if (k->in)
			itt_remove(k, &root);
		else
			itt_insert(k, &root);
		k->in = !k->in;

		if ((round % 37) == 0)
			(void) it_check(root.rb_root.rb_node, &aug_ok);
		if ((round % 11) == 0) {
			unsigned long qs = t_rand(&seed) % 10400;
			unsigned long ql = qs + t_rand(&seed) % 500;
			size_t want = 0, got = 0;
			struct itnode *f;

			for (i = 0; i < NIT; i++) {
				if (its[i].in && its[i].start <= ql &&
				    its[i].last >= qs)
					want++;
			}
			for (f = itt_iter_first(&root, qs, ql); f != NULL;
			    f = itt_iter_next(f, qs, ql)) {
				if (!f->in || f->start > ql || f->last < qs)
					query_ok = false;
				got++;
			}
			if (got != want)
				query_ok = false;
		}
	}
	CHECK(aug_ok);
	CHECK(query_ok);
	{
		size_t live = 0;

		for (i = 0; i < NIT; i++)
			live += its[i].in;
		for (i = 0; i < NIT; i++) {
			if (its[i].in)
				itt_remove(&its[i], &root);
		}
		CHECK(RB_EMPTY_ROOT(&root.rb_root));
		CHECK(root.rb_leftmost == NULL);
		CHECK(live > 0);
	}
}
