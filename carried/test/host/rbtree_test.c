/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Host tests of linuxkpi's red-black trees (linuxkpi/lib/rbtree.c,
 * linux/rbtree*.h) and generic interval tree (linux/interval_tree_generic.h),
 * the MIT reimplementations the carried drm_mm runs on. Random insertions
 * and erasures, plain, cached and augmented, each followed by a full
 * verification: the red-black rules, parent pointers, key order, the
 * leftmost cache, rb_first/rb_last/rb_next/rb_prev agreeing with an
 * in-order walk, the post-order walk visiting children before parents,
 * and every augmented value. The interval tree's queries are compared
 * with a brute-force scan.
 */

#include <stdio.h>
#include <string.h>

#include <linux/kernel.h>
#include <linux/rbtree_augmented.h>
#include <linux/interval_tree_generic.h>

#include "selftest.h"

#define	RT_POOL		256

struct rt_node {
	struct rb_node rb;
	u64 key;
	u64 val;		/* own value */
	u64 max;		/* augmented: max of val over the subtree */
	unsigned int seen;	/* post-order visit epoch */
	bool in;
};

#define	RT_VAL(n)	((n)->val)
RB_DECLARE_CALLBACKS_MAX(static, rt_aug, struct rt_node, rb, u64, max, RT_VAL)

struct rt_tree {
	struct rb_root_cached root;
	struct rt_node *pool;
	unsigned int count;
	unsigned int epoch;
	bool augmented;
};

static void
rt_insert(struct rt_tree *t, struct rt_node *n)
{
	struct rb_node **link = &t->root.rb_root.rb_node, *parent = NULL;
	bool leftmost = true;

	n->max = n->val;
	while (*link != NULL) {
		struct rt_node *p = rb_entry(*link, struct rt_node, rb);

		parent = *link;
		if (t->augmented && p->max < n->val)
			p->max = n->val;
		if (n->key < p->key) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}
	rb_link_node(&n->rb, parent, link);
	if (t->augmented)
		rb_insert_augmented_cached(&n->rb, &t->root, leftmost, &rt_aug);
	else
		rb_insert_color_cached(&n->rb, &t->root, leftmost);
	n->in = true;
	t->count++;
}

static void
rt_erase(struct rt_tree *t, struct rt_node *n)
{
	if (t->augmented)
		rb_erase_augmented_cached(&n->rb, &t->root, &rt_aug);
	else
		(void) rb_erase_cached(&n->rb, &t->root);
	RB_CLEAR_NODE(&n->rb);
	n->in = false;
	t->count--;
}

/* Black height, or -1 on a broken rule (see drm_mm_selftest.c). */
static int
rt_black_height(const struct rb_node *rb, const struct rb_node *parent)
{
	int lh, rh;
	bool black;

	if (rb == NULL)
		return (1);
	black = (rb->__rb_parent_color & RB_BLACK) != 0;
	if (rb_parent(rb) != parent)
		return (-1);
	if (!black && ((rb->rb_left != NULL &&
	    !(rb->rb_left->__rb_parent_color & RB_BLACK)) ||
	    (rb->rb_right != NULL &&
	    !(rb->rb_right->__rb_parent_color & RB_BLACK))))
		return (-1);
	lh = rt_black_height(rb->rb_left, rb);
	rh = rt_black_height(rb->rb_right, rb);
	if (lh < 0 || rh < 0 || lh != rh)
		return (-1);
	return (lh + (black ? 1 : 0));
}

/* In-order walk: count, key order, augmented values. */
static u64
rt_walk(const struct rb_node *rb, unsigned int *count, u64 *prev,
    bool *ordered, bool *augmented)
{
	const struct rt_node *n;
	u64 max, sub;

	if (rb == NULL)
		return (0);
	n = rb_entry(rb, struct rt_node, rb);
	max = rt_walk(rb->rb_left, count, prev, ordered, augmented);
	if (*count > 0 && n->key < *prev)
		*ordered = false;
	*prev = n->key;
	(*count)++;
	if (n->val > max)
		max = n->val;
	sub = rt_walk(rb->rb_right, count, prev, ordered, augmented);
	if (sub > max)
		max = sub;
	if (n->max != max)
		*augmented = false;
	return (max);
}

static bool
rt_verify(struct st_ctx *ctx, struct rt_tree *t)
{
	const struct rb_root *root = &t->root.rb_root;
	const struct rb_node *rb, *last = NULL;
	unsigned int count = 0, fwd = 0, back = 0, post = 0;
	u64 prev = 0;
	bool ordered = true, augmented = true, post_ok = true, ok = true;

	ok &= ST_CHECK(ctx, root->rb_node == NULL ||
	    (root->rb_node->__rb_parent_color & RB_BLACK) != 0,
	    "root is red");
	ok &= ST_CHECK(ctx, rt_black_height(root->rb_node, NULL) > 0,
	    "red-black rules or parent pointers broken");
	(void) rt_walk(root->rb_node, &count, &prev, &ordered, &augmented);
	ok &= ST_CHECK(ctx, count == t->count && ordered,
	    "in-order walk: %u nodes of %u, ordered %d", count, t->count,
	    ordered);
	if (t->augmented)
		ok &= ST_CHECK(ctx, augmented, "augmented values wrong");
	ok &= ST_CHECK(ctx, t->root.rb_leftmost == rb_first(root),
	    "leftmost cache is not rb_first()");

	for (rb = rb_first(root); rb != NULL; rb = rb_next(rb)) {
		last = rb;
		fwd++;
	}
	ok &= ST_CHECK(ctx, fwd == count && last == rb_last(root),
	    "rb_next() walk: %u nodes of %u", fwd, count);
	for (rb = rb_last(root); rb != NULL; rb = rb_prev(rb))
		back++;
	ok &= ST_CHECK(ctx, back == count, "rb_prev() walk: %u nodes of %u",
	    back, count);

	t->epoch++;
	for (rb = rb_first_postorder(root); rb != NULL;
	    rb = rb_next_postorder(rb)) {
		struct rt_node *n = rb_entry(rb, struct rt_node, rb);

		if (n->seen == t->epoch)
			post_ok = false;	/* visited twice */
		if ((rb->rb_left != NULL && rb_entry(rb->rb_left,
		    struct rt_node, rb)->seen != t->epoch) ||
		    (rb->rb_right != NULL && rb_entry(rb->rb_right,
		    struct rt_node, rb)->seen != t->epoch))
			post_ok = false;	/* parent before a child */
		n->seen = t->epoch;
		post++;
	}
	ok &= ST_CHECK(ctx, post_ok && post == count,
	    "post-order walk: %u nodes of %u, order ok %d", post, count,
	    post_ok);
	return (ok);
}

static void
rt_random(struct st_ctx *ctx, bool augmented, unsigned int keys)
{
	struct rt_tree t;
	unsigned int step, i;
	size_t sz = RT_POOL * sizeof (struct rt_node);

	memset(&t, 0, sizeof (t));
	t.root = RB_ROOT_CACHED;
	t.augmented = augmented;
	t.pool = st_zalloc(sz);
	if (!ST_CHECK(ctx, t.pool != NULL, "no memory"))
		return;
	for (i = 0; i < RT_POOL; i++)
		RB_CLEAR_NODE(&t.pool[i].rb);

	for (step = 0; step < 20000 * ctx->scale; step++) {
		struct rt_node *n = &t.pool[st_rand_below(ctx, RT_POOL)];

		if (n->in) {
			rt_erase(&t, n);
			ST_CHECK(ctx, RB_EMPTY_NODE(&n->rb) &&
			    rb_next(&n->rb) == NULL, "cleared node not empty");
		} else {
			n->key = st_rand_below(ctx, keys);
			n->val = st_rand_below(ctx, 1000000);
			rt_insert(&t, n);
		}
		if (!rt_verify(ctx, &t))
			break;
	}
	for (i = 0; i < RT_POOL; i++) {
		if (t.pool[i].in)
			rt_erase(&t, &t.pool[i]);
	}
	ST_CHECK(ctx, RB_EMPTY_ROOT(&t.root.rb_root) && t.count == 0 &&
	    t.root.rb_leftmost == NULL, "tree not empty at the end");
	st_free(t.pool, sz);
}

static void
rt_plain(struct st_ctx *ctx)
{
	rt_random(ctx, false, 100000);
}

static void
rt_plain_dups(struct st_ctx *ctx)
{
	rt_random(ctx, false, 16);	/* many equal keys */
}

static void
rt_augmented(struct st_ctx *ctx)
{
	rt_random(ctx, true, 100000);
}

static void
rt_augmented_dups(struct st_ctx *ctx)
{
	rt_random(ctx, true, 16);
}

/* Sorted insertions, then erasure in three orders: the skewed shapes. */
static void
rt_sequential(struct st_ctx *ctx)
{
	struct rt_tree t;
	size_t sz = RT_POOL * sizeof (struct rt_node);
	unsigned int order, i;

	t.pool = st_zalloc(sz);
	if (!ST_CHECK(ctx, t.pool != NULL, "no memory"))
		return;
	for (order = 0; order < 3; order++) {
		t.root = RB_ROOT_CACHED;
		t.count = 0;
		t.epoch = 0;
		t.augmented = order != 1;
		memset(t.pool, 0, sz);
		for (i = 0; i < RT_POOL; i++) {
			t.pool[i].key = order == 2 ? RT_POOL - i : i;
			t.pool[i].val = i * 7 % 13;
			rt_insert(&t, &t.pool[i]);
			rt_verify(ctx, &t);
		}
		for (i = 0; i < RT_POOL; i++) {
			unsigned int k = order == 0 ? i : order == 1 ?
			    RT_POOL - 1 - i : (i * 37) % RT_POOL;

			rt_erase(&t, &t.pool[k]);
			rt_verify(ctx, &t);
		}
		ST_CHECK(ctx, t.count == 0 && RB_EMPTY_ROOT(&t.root.rb_root),
		    "order %u: tree not empty", order);
	}
	st_free(t.pool, sz);
}

/* rb_replace_node_cached(): the new node takes the old one's place. */
static void
rt_replace(struct st_ctx *ctx)
{
	struct rt_tree t;
	struct rt_node *spare;
	size_t sz = RT_POOL * sizeof (struct rt_node);
	unsigned int i;

	memset(&t, 0, sizeof (t));
	t.root = RB_ROOT_CACHED;
	t.augmented = true;
	t.pool = st_zalloc(sz);
	spare = st_zalloc(sizeof (*spare));
	if (!ST_CHECK(ctx, t.pool != NULL && spare != NULL, "no memory"))
		goto out;
	for (i = 0; i < RT_POOL; i++) {
		t.pool[i].key = st_rand_below(ctx, 1000);
		t.pool[i].val = st_rand_below(ctx, 1000);
		rt_insert(&t, &t.pool[i]);
	}
	rt_verify(ctx, &t);
	for (i = 0; i < 1000 * ctx->scale; i++) {
		struct rt_node *old = &t.pool[st_rand_below(ctx, RT_POOL)];

		/* Same key and value: the order and the maxima still hold. */
		spare->key = old->key;
		spare->val = old->val;
		spare->max = old->max;
		rb_replace_node_cached(&old->rb, &spare->rb, &t.root);
		spare->in = true;
		ST_CHECK(ctx, rt_verify(ctx, &t), "after replacing a node");
		/* And back: the pool's node takes the spare's place. */
		rb_replace_node_cached(&spare->rb, &old->rb, &t.root);
		ST_CHECK(ctx, rt_verify(ctx, &t), "after replacing it back");
	}
out:
	st_free(spare, sizeof (*spare));
	if (t.pool != NULL)
		st_free(t.pool, sz);
}

/* Post-order iteration may free each entry it visits. */
static void
rt_postorder_free(struct st_ctx *ctx)
{
	struct rt_tree t;
	struct rt_node *pos, *n;
	size_t sz = RT_POOL * sizeof (struct rt_node);
	unsigned int i, visited = 0;

	memset(&t, 0, sizeof (t));
	t.root = RB_ROOT_CACHED;
	t.pool = st_zalloc(sz);
	if (!ST_CHECK(ctx, t.pool != NULL, "no memory"))
		return;
	for (i = 0; i < RT_POOL; i++) {
		t.pool[i].key = st_rand_below(ctx, 1000);
		rt_insert(&t, &t.pool[i]);
	}
	rbtree_postorder_for_each_entry_safe(pos, n, &t.root.rb_root, rb) {
		/* "Free": poison the node; the iterator must not touch it. */
		memset(pos, 0xa5, sizeof (*pos));
		visited++;
	}
	ST_CHECK(ctx, visited == RT_POOL, "visited %u of %u", visited,
	    RT_POOL);
	st_free(t.pool, sz);
}

/* --- the generic interval tree -------------------------------------- */

struct it_node {
	struct rb_node rb;
	u64 start, last;
	u64 subtree_last;
	bool in;
};

#define	IT_START(n)	((n)->start)
#define	IT_LAST(n)	((n)->last)
INTERVAL_TREE_DEFINE(struct it_node, rb, u64, subtree_last, IT_START,
    IT_LAST, static inline __maybe_unused, it_tree)

static u64
it_walk(const struct rb_node *rb, unsigned int *count, bool *augmented)
{
	const struct it_node *n;
	u64 max, sub;

	if (rb == NULL)
		return (0);
	n = rb_entry(rb, struct it_node, rb);
	max = it_walk(rb->rb_left, count, augmented);
	(*count)++;
	if (n->last > max)
		max = n->last;
	sub = it_walk(rb->rb_right, count, augmented);
	if (sub > max)
		max = sub;
	if (n->subtree_last != max)
		*augmented = false;
	return (max);
}

static void
rt_interval(struct st_ctx *ctx)
{
	const u64 span = 10000;
	struct rb_root_cached root = RB_ROOT_CACHED;
	struct it_node *pool;
	size_t sz = RT_POOL * sizeof (*pool);
	unsigned int step, i, live = 0;

	pool = st_zalloc(sz);
	if (!ST_CHECK(ctx, pool != NULL, "no memory"))
		return;
	for (step = 0; step < 20000 * ctx->scale; step++) {
		struct it_node *n = &pool[st_rand_below(ctx, RT_POOL)];
		unsigned int count = 0, found = 0, expect = 0;
		bool augmented = true, order = true, overlap = true;
		u64 qs, ql, prev = 0;
		struct it_node *x;

		if (n->in) {
			it_tree_remove(n, &root);
			n->in = false;
			live--;
		} else {
			n->start = st_rand_below(ctx, span);
			n->last = n->start + st_rand_below(ctx, 500);
			it_tree_insert(n, &root);
			n->in = true;
			live++;
		}
		(void) it_walk(root.rb_root.rb_node, &count, &augmented);
		ST_CHECK(ctx, count == live && augmented &&
		    rt_black_height(root.rb_root.rb_node, NULL) > 0 &&
		    root.rb_leftmost == rb_first(&root.rb_root),
		    "interval tree: %u nodes of %u, subtree_last %d", count,
		    live, augmented);

		qs = st_rand_below(ctx, span + 600);
		ql = qs + st_rand_below(ctx, 300);
		for (x = it_tree_iter_first(&root, qs, ql); x != NULL;
		    x = it_tree_iter_next(x, qs, ql)) {
			if (found > 0 && x->start < prev)
				order = false;
			prev = x->start;
			if (!(x->start <= ql && x->last >= qs))
				overlap = false;
			found++;
			if (found > RT_POOL)
				break;
		}
		for (i = 0; i < RT_POOL; i++) {
			if (pool[i].in && pool[i].start <= ql &&
			    pool[i].last >= qs)
				expect++;
		}
		ST_CHECK(ctx, found == expect && order && overlap,
		    "query [%llu, %llu]: %u found, %u overlap (order %d, "
		    "overlap %d)", (unsigned long long)qs,
		    (unsigned long long)ql, found, expect, order, overlap);
	}
	st_free(pool, sz);
}

const struct st_test rbtree_tests[] = {
	{ "plain", rt_plain, ST_HOST_ONLY },
	{ "plain_dups", rt_plain_dups, ST_HOST_ONLY },
	{ "augmented", rt_augmented, ST_HOST_ONLY },
	{ "augmented_dups", rt_augmented_dups, ST_HOST_ONLY },
	{ "sequential", rt_sequential, ST_HOST_ONLY },
	{ "replace", rt_replace, ST_HOST_ONLY },
	{ "postorder_free", rt_postorder_free, ST_HOST_ONLY },
	{ "interval", rt_interval, ST_HOST_ONLY },
};
const unsigned int rbtree_ntests = ARRAY_SIZE(rbtree_tests);
