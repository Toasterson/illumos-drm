/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Self-tests of the carried drm_mm range allocator (Linux v7.2
 * drivers/gpu/drm/drm_mm.c), run in the kernel by drm_helpers_test against
 * misc/drm_helpers, and on a host by carried/host-test.sh against the same
 * source.
 *
 * Origin: Linux's KUnit suite for drm_mm, drivers/gpu/drm/tests/
 * drm_mm_test.c (GPL-2.0-only), has six cases at v7.2: init, debug,
 * align32, align64, lowest and highest. They are re-expressed here, not
 * copied, as "init", "print" (host only: drm_mm_print() is not among the
 * symbols misc/drm_helpers exports), "align32", "align64", "lowest" and
 * "highest". The other tests cover what i915 relies on: reservations,
 * the four insertion modes and DRM_MM_INSERT_ONCE, ranges, alignments
 * that are not powers of two, colour adjustment with guard gaps, the
 * interval queries, the eviction scan with colour eviction as
 * i915_gem_evict_something() drives it, long random sequences checked
 * against an exhaustive feasibility oracle, and independent allocators
 * used from several threads at once.
 *
 * After every change the allocator's whole state is verified
 * (st_mm_verify()): the node list, every hole, the hole stack, and the
 * three red-black trees (hole sizes, hole addresses with their subtree
 * maximum, the node interval tree with its subtree maximum), including
 * the red-black rules, parent pointers and the leftmost caches. drm_mm
 * keeps no lock; its user serialises calls on one allocator, and these
 * tests do the same.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <drm/drm_mm.h>

#include "selftest.h"

#define	ST_NODES	512	/* node pool of the random tests */
#define	ST_GUARD	16	/* the colour test's guard gap */
#define	ST_WORKERS	4	/* threads of the parallel test */

/* --- verification --------------------------------------------------- */

static bool
st_rb_is_black(const struct rb_node *rb)
{
	return (rb == NULL || (rb->__rb_parent_color & RB_BLACK) != 0);
}

/*
 * The black height of the subtree at `rb`, whose parent must be `parent`,
 * or -1 after setting *bad: a wrong parent pointer, a red node with a red
 * child, or two paths with different numbers of black nodes.
 */
static int
st_rb_black_height(const struct rb_node *rb, const struct rb_node *parent,
    bool *bad)
{
	int lh, rh;

	if (rb == NULL)
		return (1);
	if (rb_parent(rb) != parent) {
		*bad = true;
		return (-1);
	}
	if (!st_rb_is_black(rb) && (!st_rb_is_black(rb->rb_left) ||
	    !st_rb_is_black(rb->rb_right))) {
		*bad = true;
		return (-1);
	}
	lh = st_rb_black_height(rb->rb_left, rb, bad);
	rh = st_rb_black_height(rb->rb_right, rb, bad);
	if (lh < 0 || rh < 0 || lh != rh) {
		*bad = true;
		return (-1);
	}
	return (lh + (st_rb_is_black(rb) ? 1 : 0));
}

static bool
st_rb_valid(const struct rb_root *root)
{
	bool bad = false;

	if (root->rb_node == NULL)
		return (true);
	if (!st_rb_is_black(root->rb_node))
		return (false);
	(void) st_rb_black_height(root->rb_node, NULL, &bad);
	return (!bad);
}

static const struct rb_node *
st_rb_leftmost(const struct rb_root *root)
{
	const struct rb_node *rb = root->rb_node;

	while (rb != NULL && rb->rb_left != NULL)
		rb = rb->rb_left;
	return (rb);
}

/* Walk state of the hole-address tree: in order, by hole start. */
struct st_walk {
	uint64_t count;
	u64 prev;
	bool ordered;
	bool augmented;
};

/* Returns the largest hole size in the subtree; checks subtree_max_hole. */
static u64
st_walk_holes_addr(const struct rb_node *rb, struct st_walk *w)
{
	const struct drm_mm_node *n;
	u64 max, sub;

	if (rb == NULL)
		return (0);
	n = rb_entry(rb, struct drm_mm_node, rb_hole_addr);
	max = st_walk_holes_addr(rb->rb_left, w);
	if (w->count > 0 && __drm_mm_hole_node_start(n) <= w->prev)
		w->ordered = false;
	w->prev = __drm_mm_hole_node_start(n);
	w->count++;
	if (n->hole_size > max)
		max = n->hole_size;
	sub = st_walk_holes_addr(rb->rb_right, w);
	if (sub > max)
		max = sub;
	if (n->subtree_max_hole != max)
		w->augmented = false;
	return (max);
}

/* The hole-size tree: in order, by hole size, largest first. */
static void
st_walk_holes_size(const struct rb_node *rb, struct st_walk *w)
{
	const struct drm_mm_node *n;

	if (rb == NULL)
		return;
	st_walk_holes_size(rb->rb_left, w);
	n = rb_entry(rb, struct drm_mm_node, rb_hole_size);
	if (w->count > 0 && n->hole_size > w->prev)
		w->ordered = false;
	w->prev = n->hole_size;
	w->count++;
	st_walk_holes_size(rb->rb_right, w);
}

/* The interval tree: in order, by start; checks __subtree_last. */
static u64
st_walk_intervals(const struct rb_node *rb, struct st_walk *w)
{
	const struct drm_mm_node *n;
	u64 max, sub;

	if (rb == NULL)
		return (0);
	n = rb_entry(rb, struct drm_mm_node, rb);
	max = st_walk_intervals(rb->rb_left, w);
	if (w->count > 0 && n->start <= w->prev)
		w->ordered = false;
	w->prev = n->start;
	w->count++;
	if (n->start + n->size - 1 > max)
		max = n->start + n->size - 1;
	sub = st_walk_intervals(rb->rb_right, w);
	if (sub > max)
		max = sub;
	if (n->__subtree_last != max)
		w->augmented = false;
	return (max);
}

/* The managed range [*start, *end) of an initialised allocator. */
static void
st_mm_range(const struct drm_mm *mm, u64 *start, u64 *end)
{
	*end = mm->head_node.start;
	*start = *end + mm->head_node.size;	/* head size is -size */
}

/*
 * Verifies the allocator's whole state; `nodes` is the number of nodes
 * the caller believes allocated, or -1 to skip that comparison. Returns
 * true when every check passed.
 */
static bool
st_mm_verify(struct st_ctx *ctx, const struct drm_mm *mm, long nodes,
    const char *where)
{
	const struct drm_mm_node *n, *next;
	const struct drm_mm_node *head = &mm->head_node;
	struct st_walk w;
	u64 start, end, used = 0, free = 0;
	uint64_t count = 0, holes = 0, stacked = 0;
	bool ok = true, list_ok = true, holes_ok = true;

	st_mm_range(mm, &start, &end);

	/* The node list, from the head (whose hole is the first gap). */
	n = head;
	do {
		u64 hs = __drm_mm_hole_node_start(n);
		u64 he;

		next = list_next_entry(n, node_list);
		he = next->start;
		if (n != head) {
			count++;
			used += n->size;
			if (!drm_mm_node_allocated(n) || n->mm != mm ||
			    n->size == 0 || n->start + n->size < n->start ||
			    n->start < start || n->start + n->size > end)
				list_ok = false;
		}
		if (he < hs) {
			list_ok = false;
		} else {
			if (n->hole_size != he - hs ||
			    drm_mm_hole_follows(n) != (he > hs))
				holes_ok = false;
			if (he > hs) {
				holes++;
				free += he - hs;
			}
		}
		n = next;
	} while (n != head && count <= (uint64_t)1 << 40);
	ok &= ST_CHECK(ctx, list_ok, "%s: node list out of order or corrupt",
	    where);
	ok &= ST_CHECK(ctx, holes_ok, "%s: a node's hole_size is wrong", where);
	ok &= ST_CHECK(ctx, used + free == end - start,
	    "%s: used %llu + free %llu != size %llu", where,
	    (unsigned long long)used, (unsigned long long)free,
	    (unsigned long long)(end - start));
	if (nodes >= 0)
		ok &= ST_CHECK(ctx, count == (uint64_t)nodes,
		    "%s: %llu nodes in the list, %ld expected", where,
		    (unsigned long long)count, nodes);

	list_for_each_entry(n, &mm->hole_stack, hole_stack) {
		stacked++;
		if (stacked > holes)
			break;
	}
	ok &= ST_CHECK(ctx, stacked == holes,
	    "%s: hole stack has %llu entries, %llu holes", where,
	    (unsigned long long)stacked, (unsigned long long)holes);

	(void) __builtin_memset(&w, 0, sizeof (w));
	w.ordered = true;
	st_walk_holes_size(mm->holes_size.rb_root.rb_node, &w);
	ok &= ST_CHECK(ctx, st_rb_valid(&mm->holes_size.rb_root) &&
	    w.ordered && w.count == holes &&
	    mm->holes_size.rb_leftmost ==
	    st_rb_leftmost(&mm->holes_size.rb_root),
	    "%s: hole-size tree broken (count %llu of %llu, ordered %d)",
	    where, (unsigned long long)w.count, (unsigned long long)holes,
	    w.ordered);

	(void) __builtin_memset(&w, 0, sizeof (w));
	w.ordered = w.augmented = true;
	(void) st_walk_holes_addr(mm->holes_addr.rb_node, &w);
	ok &= ST_CHECK(ctx, st_rb_valid(&mm->holes_addr) && w.ordered &&
	    w.augmented && w.count == holes,
	    "%s: hole-address tree broken (count %llu of %llu, ordered %d, "
	    "subtree_max_hole %d)", where, (unsigned long long)w.count,
	    (unsigned long long)holes, w.ordered, w.augmented);

	(void) __builtin_memset(&w, 0, sizeof (w));
	w.ordered = w.augmented = true;
	(void) st_walk_intervals(mm->interval_tree.rb_root.rb_node, &w);
	ok &= ST_CHECK(ctx, st_rb_valid(&mm->interval_tree.rb_root) &&
	    w.ordered && w.augmented && w.count == count &&
	    mm->interval_tree.rb_leftmost ==
	    st_rb_leftmost(&mm->interval_tree.rb_root),
	    "%s: interval tree broken (count %llu of %llu, ordered %d, "
	    "__subtree_last %d)", where, (unsigned long long)w.count,
	    (unsigned long long)count, w.ordered, w.augmented);
	return (ok);
}

/* --- oracles -------------------------------------------------------- */

/*
 * Whether some hole can take an allocation of `size` aligned to `align`
 * with colour `color` inside [rs, re), under the allocator's own rules
 * (colour adjustment, then range, then alignment). Every insertion mode
 * without DRM_MM_INSERT_ONCE visits every hole that could fit, so a
 * failed insertion must mean this is false.
 */
static bool
st_mm_feasible(const struct drm_mm *mm, u64 size, u64 align,
    unsigned long color, u64 rs, u64 re)
{
	struct drm_mm_node *hole;
	u64 hs, he;

	drm_mm_for_each_hole(hole, mm, hs, he) {
		u64 cs = hs, ce = he, a, e, rem;

		if (mm->color_adjust != NULL)
			mm->color_adjust(hole, color, &cs, &ce);
		a = max(cs, rs);
		e = min(ce, re);
		if (e <= a || e - a < size)
			continue;
		if (align > 1 && (rem = a % align) != 0) {
			if (e - a < align - rem)
				continue;
			a += align - rem;
			if (e - a < size)
				continue;
		}
		return (true);
	}
	return (false);
}

/* Whether [start, start + size) lies inside one hole, after colouring. */
static bool
st_mm_reservable(const struct drm_mm *mm, u64 start, u64 size,
    unsigned long color)
{
	struct drm_mm_node *hole;
	u64 hs, he;

	if (start + size <= start)
		return (false);
	drm_mm_for_each_hole(hole, mm, hs, he) {
		u64 cs = hs, ce = he;

		if (mm->color_adjust != NULL)
			mm->color_adjust(hole, color, &cs, &ce);
		if (cs <= start && start + size <= ce)
			return (true);
	}
	return (false);
}

/*
 * A colour adjustment like i915's GGTT one: a guard gap of ST_GUARD
 * between neighbours of different colours (the allocator's head node is
 * not allocated and needs none). It never widens a hole.
 */
static void
st_color_adjust(const struct drm_mm_node *node, unsigned long color,
    u64 *start, u64 *end)
{
	const struct drm_mm_node *next = list_next_entry(node, node_list);

	if (drm_mm_node_allocated(node) && node->color != color)
		*start = *end - *start > ST_GUARD ? *start + ST_GUARD : *end;
	if (drm_mm_node_allocated(next) && next->color != color)
		*end = *end - *start > ST_GUARD ? *end - ST_GUARD : *start;
}

/* Checks a node just placed by an insertion. */
static bool
st_node_placed(struct st_ctx *ctx, const struct drm_mm *mm,
    const struct drm_mm_node *node, u64 size, u64 align,
    unsigned long color, u64 rs, u64 re, const char *where)
{
	u64 rem = 0;

	if (align > 1)
		(void) div64_u64_rem(node->start, align, &rem);
	return (ST_CHECK(ctx, drm_mm_node_allocated(node) && node->mm == mm &&
	    node->size == size && node->color == color &&
	    node->start >= rs && node->start + size <= re && rem == 0,
	    "%s: node [0x%llx + 0x%llx] colour %lu outside [0x%llx, 0x%llx), "
	    "size 0x%llx, align 0x%llx", where,
	    (unsigned long long)node->start, (unsigned long long)node->size,
	    node->color, (unsigned long long)rs, (unsigned long long)re,
	    (unsigned long long)size, (unsigned long long)align));
}

static int
st_reserve(struct drm_mm *mm, struct drm_mm_node *node, u64 start, u64 size)
{
	(void) __builtin_memset(node, 0, sizeof (*node));
	node->start = start;
	node->size = size;
	return (drm_mm_reserve_node(mm, node));
}

static int
st_insert(struct drm_mm *mm, struct drm_mm_node *node, u64 size, u64 align,
    unsigned long color, u64 rs, u64 re, enum drm_mm_insert_mode mode)
{
	(void) __builtin_memset(node, 0, sizeof (*node));
	return (drm_mm_insert_node_in_range(mm, node, size, align, color,
	    rs, re, mode));
}

/* --- the ported KUnit cases ----------------------------------------- */

/* An allocator is initialised, clean, one hole; full, empty again. */
static void
st_mm_init(struct st_ctx *ctx)
{
	const u64 size = 4096;
	struct drm_mm mm;
	struct drm_mm_node node, *hole;
	u64 hs, he;
	unsigned int holes = 0;

	(void) __builtin_memset(&mm, 0, sizeof (mm));
	ST_CHECK(ctx, !drm_mm_initialized(&mm),
	    "zeroed allocator reports initialised");
	(void) __builtin_memset(&mm, 0xff, sizeof (mm));
	drm_mm_init(&mm, 0, size);
	ST_CHECK(ctx, drm_mm_initialized(&mm), "not initialised after init");
	ST_CHECK(ctx, drm_mm_clean(&mm), "new allocator not clean");
	drm_mm_for_each_hole(hole, &mm, hs, he) {
		holes++;
		ST_CHECK(ctx, hs == 0 && he == size,
		    "hole [0x%llx, 0x%llx) instead of [0, 0x%llx)",
		    (unsigned long long)hs, (unsigned long long)he,
		    (unsigned long long)size);
	}
	ST_CHECK(ctx, holes == 1, "%u holes in a new allocator", holes);
	st_mm_verify(ctx, &mm, 0, "init");

	ST_CHECK(ctx, st_reserve(&mm, &node, 0, size) == 0,
	    "cannot reserve the whole range");
	holes = 0;
	drm_mm_for_each_hole(hole, &mm, hs, he)
		holes++;
	ST_CHECK(ctx, holes == 0 && !drm_mm_clean(&mm),
	    "full allocator has %u holes", holes);
	st_mm_verify(ctx, &mm, 1, "init full");

	drm_mm_remove_node(&node);
	ST_CHECK(ctx, !drm_mm_node_allocated(&node), "removed node allocated");
	ST_CHECK(ctx, drm_mm_clean(&mm), "not clean after removal");
	st_mm_verify(ctx, &mm, 0, "init emptied");
	drm_mm_takedown(&mm);
}

/*
 * Power-of-two alignments up to 2^(max-1) in an allocator spanning almost
 * all of u64, each with a size just over half the alignment.
 */
static void
st_mm_align_pot(struct st_ctx *ctx, unsigned int max)
{
	struct drm_mm mm;
	struct drm_mm_node *nodes;
	unsigned int bit, n = 0;
	size_t sz = max * sizeof (*nodes);

	nodes = st_zalloc(sz);
	if (!ST_CHECK(ctx, nodes != NULL, "no memory"))
		return;
	(void) __builtin_memset(&mm, 0, sizeof (mm));
	drm_mm_init(&mm, 1, U64_MAX - 2);
	for (bit = max - 1; bit > 0; bit--) {
		u64 align = BIT_ULL(bit);
		u64 size = BIT_ULL(bit - 1) + 1;

		if (!ST_CHECK(ctx, st_insert(&mm, &nodes[n], size, align, bit,
		    0, U64_MAX, DRM_MM_INSERT_BEST) == 0,
		    "align 2^%u, size 0x%llx: no space", bit,
		    (unsigned long long)size))
			break;
		st_node_placed(ctx, &mm, &nodes[n], size, align, bit, 1,
		    U64_MAX - 1, "align_pot");
		n++;
	}
	st_mm_verify(ctx, &mm, n, "align_pot");
	while (n > 0)
		drm_mm_remove_node(&nodes[--n]);
	ST_CHECK(ctx, drm_mm_clean(&mm), "not clean after align_pot");
	drm_mm_takedown(&mm);
	st_free(nodes, sz);
}

static void
st_mm_align32(struct st_ctx *ctx)
{
	st_mm_align_pot(ctx, 32);
}

static void
st_mm_align64(struct st_ctx *ctx)
{
	st_mm_align_pot(ctx, 64);
}

/*
 * Holes [0, 1), [2, 5) and [6, 7): a node of 2 fits only the middle one,
 * at its bottom for LOW and its top for HIGH.
 */
static void
st_mm_once(struct st_ctx *ctx, enum drm_mm_insert_mode mode, u64 expect)
{
	struct drm_mm mm;
	struct drm_mm_node lo, hi, node;

	(void) __builtin_memset(&mm, 0, sizeof (mm));
	drm_mm_init(&mm, 0, 7);
	ST_CHECK(ctx, st_reserve(&mm, &lo, 1, 1) == 0, "reserve [1, 2)");
	ST_CHECK(ctx, st_reserve(&mm, &hi, 5, 1) == 0, "reserve [5, 6)");
	if (ST_CHECK(ctx, st_insert(&mm, &node, 2, 0, 0, 0, 7, mode) == 0,
	    "mode 0x%x: the [2, 5) hole was not used", (unsigned int)mode)) {
		ST_CHECK(ctx, node.start == expect,
		    "mode 0x%x: placed at %llu, expected %llu",
		    (unsigned int)mode, (unsigned long long)node.start,
		    (unsigned long long)expect);
		st_mm_verify(ctx, &mm, 3, "once");
		drm_mm_remove_node(&node);
	}
	drm_mm_remove_node(&hi);
	drm_mm_remove_node(&lo);
	ST_CHECK(ctx, drm_mm_clean(&mm), "not clean after once");
	drm_mm_takedown(&mm);
}

static void
st_mm_lowest(struct st_ctx *ctx)
{
	st_mm_once(ctx, DRM_MM_INSERT_LOW, 2);
}

static void
st_mm_highest(struct st_ctx *ctx)
{
	st_mm_once(ctx, DRM_MM_INSERT_HIGH, 3);
}

/* --- reservations ----------------------------------------------------- */

static void
st_mm_reserve(struct st_ctx *ctx)
{
	struct drm_mm mm;
	struct drm_mm_node a, b, c, x;
	unsigned int i;

	(void) __builtin_memset(&mm, 0, sizeof (mm));
	drm_mm_init(&mm, 0x1000, 0x10000);	/* [0x1000, 0x11000) */
	ST_CHECK(ctx, st_reserve(&mm, &a, 0x1000, 0x1000) == 0,
	    "reserve at the start");
	ST_CHECK(ctx, st_reserve(&mm, &b, 0x10000, 0x1000) == 0,
	    "reserve at the end");
	ST_CHECK(ctx, st_reserve(&mm, &c, 0x8000, 0x800) == 0,
	    "reserve in the middle");
	st_mm_verify(ctx, &mm, 3, "reserve");

	/* Overlaps, and requests outside the range, are refused. */
	static const struct {
		u64 start, size;
	} bad[] = {
		{ 0x1000, 1 }, { 0x1fff, 2 }, { 0x7f00, 0x200 },
		{ 0x8000, 0x800 }, { 0x87ff, 1 }, { 0xffff, 2 },
		{ 0x10fff, 1 }, { 0, 0x800 }, { 0x800, 0x1000 },
		{ 0x11000, 1 }, { 0x2000, 0 }, { U64_MAX - 1, 4 },
	};
	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		ST_CHECK(ctx, st_reserve(&mm, &x, bad[i].start,
		    bad[i].size) == -ENOSPC && !drm_mm_node_allocated(&x),
		    "reserve [0x%llx + 0x%llx] was not refused",
		    (unsigned long long)bad[i].start,
		    (unsigned long long)bad[i].size);
	}
	st_mm_verify(ctx, &mm, 3, "reserve refused");

	/* The gaps around the middle node fill exactly. */
	ST_CHECK(ctx, st_reserve(&mm, &x, 0x2000, 0x6000) == 0,
	    "reserve the gap below the middle");
	drm_mm_remove_node(&x);
	ST_CHECK(ctx, st_reserve(&mm, &x, 0x8800, 0x7800) == 0,
	    "reserve the gap above the middle");
	drm_mm_remove_node(&x);

	drm_mm_remove_node(&c);
	drm_mm_remove_node(&a);
	drm_mm_remove_node(&b);
	ST_CHECK(ctx, drm_mm_clean(&mm), "not clean after reserve");
	st_mm_verify(ctx, &mm, 0, "reserve emptied");
	drm_mm_takedown(&mm);
}

/* --- insertion modes ------------------------------------------------- */

/*
 * Holes [0, 10), [20, 50) and [60, 100) between reservations [10, 20) and
 * [50, 60); [50, 60) is reserved last, so [60, 100) tops the hole stack.
 */
static void
st_mm_layout3(struct st_ctx *ctx, struct drm_mm *mm, struct drm_mm_node *r1,
    struct drm_mm_node *r2)
{
	(void) __builtin_memset(mm, 0, sizeof (*mm));
	drm_mm_init(mm, 0, 100);
	ST_CHECK(ctx, st_reserve(mm, r1, 10, 10) == 0, "reserve [10, 20)");
	ST_CHECK(ctx, st_reserve(mm, r2, 50, 10) == 0, "reserve [50, 60)");
}

static void
st_mm_modes(struct st_ctx *ctx)
{
	static const struct {
		const char *what;
		u64 size, align, rs, re;
		enum drm_mm_insert_mode mode;
		int err;
		u64 start;
	} cases[] = {
		{ "best fit 25", 25, 0, 0, 100, DRM_MM_INSERT_BEST, 0, 20 },
		{ "best fit 5", 5, 0, 0, 100, DRM_MM_INSERT_BEST, 0, 0 },
		{ "best fit 35", 35, 0, 0, 100, DRM_MM_INSERT_BEST, 0, 60 },
		{ "too big", 41, 0, 0, 100, DRM_MM_INSERT_BEST, -ENOSPC, 0 },
		{ "zero size", 0, 0, 0, 100, DRM_MM_INSERT_BEST, -ENOSPC, 0 },
		{ "low", 5, 0, 0, 100, DRM_MM_INSERT_LOW, 0, 0 },
		{ "low from 15", 5, 0, 15, 100, DRM_MM_INSERT_LOW, 0, 20 },
		{ "low from 5", 5, 0, 5, 100, DRM_MM_INSERT_LOW, 0, 5 },
		{ "low from 7", 5, 0, 7, 100, DRM_MM_INSERT_LOW, 0, 20 },
		{ "high", 5, 0, 0, 100, DRM_MM_INSERT_HIGH, 0, 95 },
		{ "high below 55", 5, 0, 0, 55, DRM_MM_INSERT_HIGH, 0, 45 },
		{ "high below 23", 5, 0, 0, 23, DRM_MM_INSERT_HIGH, 0, 5 },
		{ "evict: newest hole", 5, 0, 0, 100, DRM_MM_INSERT_EVICT, 0,
		    60 },
		{ "range too small", 6, 0, 20, 25, DRM_MM_INSERT_BEST,
		    -ENOSPC, 0 },
		{ "range exact", 5, 0, 20, 25, DRM_MM_INSERT_BEST, 0, 20 },
		{ "range over a node", 10, 0, 5, 25, DRM_MM_INSERT_LOW,
		    -ENOSPC, 0 },
		{ "align 16 low", 4, 16, 0, 100, DRM_MM_INSERT_LOW, 0, 0 },
		{ "align 16 from 1", 4, 16, 1, 100, DRM_MM_INSERT_LOW, 0, 32 },
		{ "align 16 high", 4, 16, 0, 100, DRM_MM_INSERT_HIGH, 0, 96 },
		{ "align 7", 4, 7, 1, 100, DRM_MM_INSERT_LOW, 0, 21 },
		{ "align 7 high", 4, 7, 0, 100, DRM_MM_INSERT_HIGH, 0, 91 },
		{ "align 30", 10, 30, 1, 100, DRM_MM_INSERT_LOW, 0, 30 },
		{ "align 45", 10, 45, 1, 100, DRM_MM_INSERT_BEST, 0, 90 },
		{ "align 50 none", 10, 50, 1, 100, DRM_MM_INSERT_BEST,
		    -ENOSPC, 0 },
		{ "low once in range", 5, 0, 60, 100, DRM_MM_INSERT_LOWEST,
		    0, 60 },
		{ "high once in range", 5, 0, 20, 50, DRM_MM_INSERT_HIGHEST,
		    0, 45 },
	};
	struct drm_mm mm;
	struct drm_mm_node r1, r2, node;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		int err;

		st_mm_layout3(ctx, &mm, &r1, &r2);
		err = st_insert(&mm, &node, cases[i].size, cases[i].align, 0,
		    cases[i].rs, cases[i].re, cases[i].mode);
		ST_CHECK(ctx, err == cases[i].err, "%s: returned %d, "
		    "expected %d", cases[i].what, err, cases[i].err);
		if (err == 0) {
			ST_CHECK(ctx, node.start == cases[i].start,
			    "%s: placed at %llu, expected %llu", cases[i].what,
			    (unsigned long long)node.start,
			    (unsigned long long)cases[i].start);
			st_node_placed(ctx, &mm, &node, cases[i].size,
			    cases[i].align, 0, cases[i].rs, cases[i].re,
			    cases[i].what);
			st_mm_verify(ctx, &mm, 3, cases[i].what);
			drm_mm_remove_node(&node);
		} else {
			ST_CHECK(ctx, !st_mm_feasible(&mm, cases[i].size,
			    cases[i].align, 0, cases[i].rs, cases[i].re) ||
			    cases[i].size == 0, "%s: refused but feasible",
			    cases[i].what);
		}
		drm_mm_remove_node(&r2);
		drm_mm_remove_node(&r1);
		ST_CHECK(ctx, drm_mm_clean(&mm), "%s: not clean",
		    cases[i].what);
		st_mm_verify(ctx, &mm, 0, cases[i].what);
		drm_mm_takedown(&mm);
	}
}

/* --- colour adjustment ---------------------------------------------- */

static void
st_mm_color(struct st_ctx *ctx)
{
	struct drm_mm mm;
	struct drm_mm_node a, b, c, d;

	(void) __builtin_memset(&mm, 0, sizeof (mm));
	drm_mm_init(&mm, 0, 1000);
	mm.color_adjust = st_color_adjust;

	/* Colour 1 first, then colour 2 must keep a guard gap after it. */
	ST_CHECK(ctx, st_insert(&mm, &a, 100, 0, 1, 0, 1000,
	    DRM_MM_INSERT_LOW) == 0 && a.start == 0, "first node at 0");
	ST_CHECK(ctx, st_insert(&mm, &b, 100, 0, 2, 0, 1000,
	    DRM_MM_INSERT_LOW) == 0 && b.start == 100 + ST_GUARD,
	    "different colour at %llu, expected %u",
	    (unsigned long long)b.start, 100 + ST_GUARD);
	/* The same colour packs tightly after its own colour. */
	ST_CHECK(ctx, st_insert(&mm, &c, 50, 0, 2, 0, 1000,
	    DRM_MM_INSERT_LOW) == 0 && c.start == 216,
	    "same colour at %llu, expected 216", (unsigned long long)c.start);
	st_mm_verify(ctx, &mm, 3, "color");

	/* Reserving into the guard gap is refused, beyond it allowed. */
	ST_CHECK(ctx, st_reserve(&mm, &d, 100, 10) == -ENOSPC,
	    "reserved inside the guard gap");
	ST_CHECK(ctx, st_reserve(&mm, &d, 100, ST_GUARD) == -ENOSPC,
	    "reserved the whole guard gap with colour 0");
	(void) __builtin_memset(&d, 0, sizeof (d));
	d.start = 100;
	d.size = ST_GUARD;
	d.color = 1;
	ST_CHECK(ctx, drm_mm_reserve_node(&mm, &d) == -ENOSPC,
	    "reserved next to a different colour without a guard");
	drm_mm_remove_node(&b);
	drm_mm_remove_node(&c);
	/* Now a colour-2 node of 900 fits only after a guard. */
	ST_CHECK(ctx, st_insert(&mm, &b, 900 - ST_GUARD, 0, 2, 0, 1000,
	    DRM_MM_INSERT_BEST) == 0 && b.start == 100 + ST_GUARD,
	    "large node of another colour");
	ST_CHECK(ctx, st_insert(&mm, &c, 1, 0, 2, 0, 1000,
	    DRM_MM_INSERT_BEST) == -ENOSPC, "allocator should be full");
	st_mm_verify(ctx, &mm, 2, "color full");
	drm_mm_remove_node(&a);
	drm_mm_remove_node(&b);
	ST_CHECK(ctx, drm_mm_clean(&mm), "not clean after color");
	drm_mm_takedown(&mm);
}

/* --- interval queries ------------------------------------------------- */

static void
st_mm_interval(struct st_ctx *ctx)
{
	const u64 span = 1 << 16;
	struct drm_mm mm;
	struct drm_mm_node *nodes, *n;
	size_t sz = ST_NODES * sizeof (*nodes);
	unsigned int i, q, count = 0;

	nodes = st_zalloc(sz);
	if (!ST_CHECK(ctx, nodes != NULL, "no memory"))
		return;
	(void) __builtin_memset(&mm, 0, sizeof (mm));
	drm_mm_init(&mm, 0, span);
	for (i = 0; i < ST_NODES; i++) {
		u64 size = 1 + st_rand_below(ctx, 200);

		if (st_insert(&mm, &nodes[i], size, 0, 0, 0, span,
		    DRM_MM_INSERT_BEST) == 0)
			count++;
		else
			break;
		/* Leave gaps: free about a third again. */
		if (st_rand_below(ctx, 3) == 0) {
			drm_mm_remove_node(&nodes[i]);
			count--;
		}
	}
	st_mm_verify(ctx, &mm, count, "interval fill");

	for (q = 0; q < 200 * ctx->scale; q++) {
		u64 s = st_rand_below(ctx, span), e;
		unsigned int found = 0, expect = 0;
		u64 prev = 0;
		bool ordered = true, overlap = true;

		/* drm_mm_for_each_node_in_range() needs end <= the range end. */
		e = s + 1 + st_rand_below(ctx, span / 8);
		if (e > span)
			e = span;
		drm_mm_for_each_node_in_range(n, &mm, s, e) {
			if (found > 0 && n->start <= prev)
				ordered = false;
			prev = n->start;
			if (!(n->start < e && n->start + n->size > s))
				overlap = false;
			found++;
		}
		drm_mm_for_each_node(n, &mm) {
			if (n->start < e && n->start + n->size > s)
				expect++;
		}
		ST_CHECK(ctx, found == expect && ordered && overlap,
		    "range [0x%llx, 0x%llx): %u nodes found, %u overlap "
		    "(ordered %d, all overlapping %d)", (unsigned long long)s,
		    (unsigned long long)e, found, expect, ordered, overlap);
	}
	/* A query that hits nothing returns the head node's sentinel. */
	ST_CHECK(ctx, __drm_mm_interval_first(&mm, span, span + 10) ==
	    &mm.head_node, "empty query does not return the head node");

	for (i = 0; i < ST_NODES; i++) {
		if (drm_mm_node_allocated(&nodes[i]))
			drm_mm_remove_node(&nodes[i]);
	}
	ST_CHECK(ctx, drm_mm_clean(&mm), "not clean after interval");
	drm_mm_takedown(&mm);
	st_free(nodes, sz);
}

/* --- random operations against the oracle ----------------------------- */

static const u64 st_aligns[] = {
	0, 1, 2, 4, 8, 64, 4096, 3, 6, 7, 100, 4095, 12288
};

/* One random insertion; returns true if it allocated `node`. */
static bool
st_random_insert(struct st_ctx *ctx, struct drm_mm *mm,
    struct drm_mm_node *node, u64 span, bool colors)
{
	static const enum drm_mm_insert_mode modes[] = {
		DRM_MM_INSERT_BEST, DRM_MM_INSERT_LOW, DRM_MM_INSERT_HIGH,
		DRM_MM_INSERT_EVICT
	};
	enum drm_mm_insert_mode mode = modes[st_rand_below(ctx, 4)];
	u64 size = 1 + st_rand_below(ctx, st_rand_below(ctx, 8) == 0 ?
	    span / 16 : 256);
	u64 align = st_aligns[st_rand_below(ctx, ARRAY_SIZE(st_aligns))];
	unsigned long color = colors ? (unsigned long)st_rand_below(ctx, 3) : 0;
	u64 rs = 0, re = span;
	bool once = st_rand_below(ctx, 10) == 0;
	int err;

	if (st_rand_below(ctx, 2) == 0) {
		rs = st_rand_below(ctx, span);
		re = rs + 1 + st_rand_below(ctx, span - rs);
	}
	if (once)
		mode |= DRM_MM_INSERT_ONCE;
	err = st_insert(mm, node, size, align, color, rs, re, mode);
	if (err == 0) {
		st_node_placed(ctx, mm, node, size, align, color, rs, re,
		    "random insert");
		return (true);
	}
	ST_CHECK(ctx, err == -ENOSPC && !drm_mm_node_allocated(node),
	    "random insert returned %d", err);
	if (!once) {
		ST_CHECK(ctx, !st_mm_feasible(mm, size, align, color, rs, re),
		    "insert size 0x%llx align 0x%llx colour %lu in [0x%llx, "
		    "0x%llx) mode 0x%x refused but feasible",
		    (unsigned long long)size, (unsigned long long)align, color,
		    (unsigned long long)rs, (unsigned long long)re,
		    (unsigned int)mode);
	}
	return (false);
}

/* One random reservation; returns true if it allocated `node`. */
static bool
st_random_reserve(struct st_ctx *ctx, struct drm_mm *mm,
    struct drm_mm_node *node, u64 span, bool colors)
{
	u64 start = st_rand_below(ctx, span);
	u64 size = 1 + st_rand_below(ctx, 64);
	unsigned long color = colors ? (unsigned long)st_rand_below(ctx, 3) : 0;
	bool can = st_mm_reservable(mm, start, size, color);
	int err;

	(void) __builtin_memset(node, 0, sizeof (*node));
	node->start = start;
	node->size = size;
	node->color = color;
	err = drm_mm_reserve_node(mm, node);
	ST_CHECK(ctx, (err == 0) == can && (err == 0 || err == -ENOSPC),
	    "reserve [0x%llx + 0x%llx] colour %lu returned %d, reservable %d",
	    (unsigned long long)start, (unsigned long long)size, color, err,
	    can);
	return (err == 0);
}

/*
 * A random walk of insertions, reservations and removals on one
 * allocator, verified after every step; the shared body of the random
 * and parallel tests.
 */
static void
st_random_walk(struct st_ctx *ctx, bool colors, unsigned int steps)
{
	const u64 span = 1 << 16;
	struct drm_mm mm;
	struct drm_mm_node *nodes;
	size_t sz = ST_NODES * sizeof (*nodes);
	unsigned int i, step;
	long live = 0;

	nodes = st_zalloc(sz);
	if (!ST_CHECK(ctx, nodes != NULL, "no memory"))
		return;
	(void) __builtin_memset(&mm, 0, sizeof (mm));
	drm_mm_init(&mm, 0, span);
	if (colors)
		mm.color_adjust = st_color_adjust;

	for (step = 0; step < steps; step++) {
		unsigned int op = (unsigned int)st_rand_below(ctx, 100);
		struct drm_mm_node *node = &nodes[st_rand_below(ctx, ST_NODES)];

		if (drm_mm_node_allocated(node)) {
			if (op < 60) {
				drm_mm_remove_node(node);
				ST_CHECK(ctx, !drm_mm_node_allocated(node),
				    "removed node still allocated");
				live--;
			}
		} else if (op < 85) {
			if (st_random_insert(ctx, &mm, node, span, colors))
				live++;
		} else {
			if (st_random_reserve(ctx, &mm, node, span, colors))
				live++;
		}
		if (!st_mm_verify(ctx, &mm, live, "random walk"))
			break;
	}

	for (i = 0; i < ST_NODES; i++) {
		if (drm_mm_node_allocated(&nodes[i]))
			drm_mm_remove_node(&nodes[i]);
	}
	ST_CHECK(ctx, drm_mm_clean(&mm), "not clean after the random walk");
	st_mm_verify(ctx, &mm, 0, "random walk emptied");
	drm_mm_takedown(&mm);
	st_free(nodes, sz);
}

static void
st_mm_random(struct st_ctx *ctx)
{
	st_random_walk(ctx, false, 3000 * ctx->scale);
}

static void
st_mm_random_color(struct st_ctx *ctx)
{
	st_random_walk(ctx, true, 3000 * ctx->scale);
}

/* --- the eviction scan ----------------------------------------------- */

/* What the eviction rounds did, to check that both outcomes occurred. */
struct st_evict_stats {
	unsigned int found;
	unsigned int not_found;
	unsigned int evicted;
	unsigned int color_evicted;
};

/*
 * One eviction round as i915_gem_evict_something() drives it: scan the
 * allocated nodes in a random LRU order until a hole for the request
 * appears, unwind the scan in reverse order, evict the nodes the unwind
 * selects and, with colours, the neighbours drm_mm_scan_color_evict()
 * names; then the request must fit with DRM_MM_INSERT_EVICT. In a quarter
 * of the rounds only a few nodes are offered (the others stand for pinned
 * objects, which i915 never adds to a scan), so that scans also fail.
 */
static void
st_evict_round(struct st_ctx *ctx, struct drm_mm *mm,
    struct drm_mm_node *nodes, struct drm_mm_node **lru,
    struct drm_mm_node **sel, long *live, u64 span, bool colors,
    struct st_evict_stats *stats)
{
	static const enum drm_mm_insert_mode modes[] = {
		DRM_MM_INSERT_BEST, DRM_MM_INSERT_LOW, DRM_MM_INSERT_HIGH
	};
	struct drm_mm_scan scan;
	struct drm_mm_node tmp, *n;
	u64 size = 1 + st_rand_below(ctx, 2048);
	u64 align = st_aligns[st_rand_below(ctx, 7)];	/* powers of two */
	unsigned long color = colors ? (unsigned long)st_rand_below(ctx, 3) : 0;
	u64 rs = 0, re = span;
	unsigned int nlru = 0, offered, added = 0, i;
	long before = *live;
	bool found = false, inside = true, color_ok = true;
	int err;

	if (st_rand_below(ctx, 2) == 0) {
		rs = st_rand_below(ctx, span / 2);
		re = rs + span / 4 + st_rand_below(ctx, span / 2);
		if (re > span)
			re = span;
	}
	if (re - rs < size)
		return;

	/* A random LRU order of the allocated nodes. */
	for (i = 0; i < ST_NODES; i++) {
		if (drm_mm_node_allocated(&nodes[i])) {
			unsigned int j = (unsigned int)st_rand_below(ctx,
			    nlru + 1);
			lru[nlru] = lru[j];
			lru[j] = &nodes[i];
			nlru++;
		}
	}

	offered = st_rand_below(ctx, 4) == 0 ?
	    (unsigned int)st_rand_below(ctx, 4) : nlru;
	if (offered > nlru)
		offered = nlru;

	drm_mm_scan_init_with_range(&scan, mm, size, align, color, rs, re,
	    modes[st_rand_below(ctx, 3)]);
	while (added < offered) {
		if (drm_mm_scan_add_block(&scan, lru[added++])) {
			found = true;
			break;
		}
	}
	/* Unwind newest first, collecting the selected nodes in sel[]. */
	{
		unsigned int nsel = 0;

		for (i = added; i > 0; i--) {
			n = lru[i - 1];
			if (drm_mm_scan_remove_block(&scan, n)) {
				if (!(n->start < scan.hit_end &&
				    n->start + n->size > scan.hit_start))
					inside = false;
				sel[nsel++] = n;
			}
		}
		ST_CHECK(ctx, mm->scan_active == 0,
		    "scan still active after the unwind");
		ST_CHECK(ctx, found || nsel == 0,
		    "scan found nothing but selected %u nodes", nsel);
		ST_CHECK(ctx, inside, "a selected node misses the hit range");
		/* The unwind restored every node. */
		st_mm_verify(ctx, mm, *live, "evict unwind");
		if (!found) {
			stats->not_found++;
			return;
		}
		stats->found++;
		stats->evicted += nsel;
		ST_CHECK(ctx, scan.hit_end - scan.hit_start == size &&
		    scan.hit_start >= rs && scan.hit_end <= re,
		    "hit [0x%llx, 0x%llx) not a 0x%llx block inside [0x%llx, "
		    "0x%llx)", (unsigned long long)scan.hit_start,
		    (unsigned long long)scan.hit_end, (unsigned long long)size,
		    (unsigned long long)rs, (unsigned long long)re);
		for (i = 0; i < nsel; i++) {
			drm_mm_remove_node(sel[i]);
			(*live)--;
		}
	}
	while ((n = drm_mm_scan_color_evict(&scan)) != NULL) {
		if (!drm_mm_node_allocated(n) || !colors) {
			color_ok = false;
			break;
		}
		drm_mm_remove_node(n);
		(*live)--;
		stats->color_evicted++;
	}
	ST_CHECK(ctx, color_ok, "colour eviction named a bad node");
	st_mm_verify(ctx, mm, *live, "evicted");

	/* Nothing allocated overlaps the hit range any more. */
	n = __drm_mm_interval_first(mm, scan.hit_start, scan.hit_end - 1);
	ST_CHECK(ctx, n == &mm->head_node, "hit range [0x%llx, 0x%llx) still "
	    "holds [0x%llx + 0x%llx]", (unsigned long long)scan.hit_start,
	    (unsigned long long)scan.hit_end, (unsigned long long)n->start,
	    (unsigned long long)n->size);

	err = st_insert(mm, &tmp, size, align, color, rs, re,
	    DRM_MM_INSERT_EVICT);
	if (ST_CHECK(ctx, err == 0, "no room after eviction (%d), %ld of %ld "
	    "nodes evicted", err, before - *live, before)) {
		st_node_placed(ctx, mm, &tmp, size, align, color, rs, re,
		    "insert after eviction");
		drm_mm_remove_node(&tmp);
	}
}

static void
st_evict(struct st_ctx *ctx, bool colors)
{
	const u64 span = 1 << 16;
	struct drm_mm mm;
	struct drm_mm_node *nodes, **lru;
	/* lru[] and, behind it, the nodes a scan selects. */
	size_t sz = ST_NODES * sizeof (*nodes);
	size_t lsz = 2 * ST_NODES * sizeof (*lru);
	unsigned int i, round;
	long live = 0;
	struct st_evict_stats stats = { 0 };

	nodes = st_zalloc(sz);
	lru = st_zalloc(lsz);
	if (!ST_CHECK(ctx, nodes != NULL && lru != NULL, "no memory"))
		goto out;
	(void) __builtin_memset(&mm, 0, sizeof (mm));
	drm_mm_init(&mm, 0, span);
	if (colors)
		mm.color_adjust = st_color_adjust;

	for (round = 0; round < 60 * ctx->scale; round++) {
		/* Refill to fragmentation: many small nodes, some freed. */
		for (i = 0; i < ST_NODES; i++) {
			struct drm_mm_node *node = &nodes[i];
			unsigned long color;
			u64 size;

			if (drm_mm_node_allocated(node)) {
				if (st_rand_below(ctx, 8) == 0) {
					drm_mm_remove_node(node);
					live--;
				}
				continue;
			}
			/* One draw per statement: argument order is unspecified. */
			size = 16 + st_rand_below(ctx, 240);
			color = colors ? (unsigned long)st_rand_below(ctx, 3) : 0;
			if (st_insert(&mm, node, size, 0, color, 0, span,
			    DRM_MM_INSERT_BEST) == 0)
				live++;
		}
		st_mm_verify(ctx, &mm, live, "evict refill");
		st_evict_round(ctx, &mm, nodes, lru, lru + ST_NODES, &live,
		    span, colors, &stats);
	}
	ST_CHECK(ctx, stats.found > 0 && stats.not_found > 0 &&
	    stats.evicted > 0 && (!colors || stats.color_evicted > 0),
	    "eviction rounds: %u found, %u not found, %u nodes evicted, %u "
	    "by colour", stats.found, stats.not_found, stats.evicted,
	    stats.color_evicted);

	for (i = 0; i < ST_NODES; i++) {
		if (drm_mm_node_allocated(&nodes[i]))
			drm_mm_remove_node(&nodes[i]);
	}
	ST_CHECK(ctx, drm_mm_clean(&mm), "not clean after evict");
	drm_mm_takedown(&mm);
out:
	if (lru != NULL)
		st_free(lru, lsz);
	if (nodes != NULL)
		st_free(nodes, sz);
}

static void
st_mm_evict(struct st_ctx *ctx)
{
	st_evict(ctx, false);
}

static void
st_mm_evict_color(struct st_ctx *ctx)
{
	st_evict(ctx, true);
}

/* --- distinct allocators on several threads ------------------------- */

struct st_worker {
	struct st_ctx ctx;
	bool colors;
};

static void
st_worker_run(void *arg)
{
	struct st_worker *w = arg;

	st_random_walk(&w->ctx, w->colors, 2000 * w->ctx.scale);
}

/*
 * drm_mm keeps no global state and takes no lock: each allocator belongs
 * to its user, who serialises calls on it (i915 holds the address space's
 * mutex). Distinct allocators may therefore be used at once; this runs
 * one random walk per thread, each verified as above.
 */
static void
st_mm_parallel(struct st_ctx *ctx)
{
	struct st_worker *w;
	void *args[ST_WORKERS];
	size_t sz = ST_WORKERS * sizeof (*w);
	unsigned int i;

	w = st_zalloc(sz);
	if (!ST_CHECK(ctx, w != NULL, "no memory"))
		return;
	for (i = 0; i < ST_WORKERS; i++) {
		w[i].ctx.test = ctx->test;
		w[i].ctx.scale = ctx->scale;
		st_seed(&w[i].ctx, st_rand(ctx) | 1);
		w[i].colors = (i & 1) != 0;
		args[i] = &w[i];
	}
	if (ST_CHECK(ctx, st_run_parallel(st_worker_run, args,
	    ST_WORKERS) == 0, "could not start %u threads", ST_WORKERS)) {
		for (i = 0; i < ST_WORKERS; i++) {
			ctx->checks_passed += w[i].ctx.checks_passed;
			ctx->checks_failed += w[i].ctx.checks_failed;
		}
	}
	st_free(w, sz);
}

const struct st_test drm_mm_selftests[] = {
	{ "init", st_mm_init, 0 },
	{ "align32", st_mm_align32, 0 },
	{ "align64", st_mm_align64, 0 },
	{ "lowest", st_mm_lowest, 0 },
	{ "highest", st_mm_highest, 0 },
	{ "reserve", st_mm_reserve, 0 },
	{ "modes", st_mm_modes, 0 },
	{ "color", st_mm_color, 0 },
	{ "interval", st_mm_interval, 0 },
	{ "random", st_mm_random, 0 },
	{ "random_color", st_mm_random_color, 0 },
	{ "evict", st_mm_evict, 0 },
	{ "evict_color", st_mm_evict_color, 0 },
	{ "parallel", st_mm_parallel, 0 },
};
const unsigned int drm_mm_nselftests = ARRAY_SIZE(drm_mm_selftests);
