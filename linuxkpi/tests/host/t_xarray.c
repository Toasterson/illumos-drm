/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Tests of src/lkpi_xarray.c: the XArray (store, load, erase, insert,
 * cmpxchg, reservations, alloc, alloc_cyclic, find, a random workload
 * against a reference array, node accounting), the radix tree front end,
 * the IDR and the IDA.
 */

#include <linux/kernel.h>
#include <linux/xarray.h>
#include <linux/radix-tree.h>
#include <linux/idr.h>
#include <linux/gfp.h>
#include <linux/errno.h>
#include "test.h"

static int obj[64];
#define	P(i)	((void *)&obj[(i)])

TEST(xarray_basic)
{
	struct xarray xa;
	unsigned long base = lkpi_host_kmem_live, idx;
	void *e;
	int n;

	xa_init(&xa);
	CHECK(xa_empty(&xa));
	CHECK(xa_load(&xa, 0) == NULL);
	CHECK(xa_store(&xa, 5, P(5), GFP_KERNEL) == NULL);
	CHECK(xa_store(&xa, ULONG_MAX, P(1), GFP_KERNEL) == NULL);
	CHECK(xa_store(&xa, 0, xa_mk_value(42), GFP_KERNEL) == NULL);
	CHECK(xa_load(&xa, 5) == P(5));
	CHECK(xa_load(&xa, ULONG_MAX) == P(1));
	CHECK(xa_is_value(xa_load(&xa, 0)));
	CHECK_EQ(xa_to_value(xa_load(&xa, 0)), 42);
	CHECK(xa_load(&xa, 6) == NULL);
	/* Replacing returns the old entry. */
	CHECK(xa_store(&xa, 5, P(6), GFP_KERNEL) == P(5));
	CHECK(xa_load(&xa, 5) == P(6));
	/* Storing NULL in a plain array erases. */
	CHECK(xa_store(&xa, 5, NULL, GFP_KERNEL) == P(6));
	CHECK(xa_load(&xa, 5) == NULL);
	CHECK(lkpi_xa_slot(&xa, 5) == NULL);
	CHECK_EQ(xa.xa_count, 2);
	/* An internal entry is refused. */
	lkpi_host_cmn_quiet = 1;
	e = xa_store(&xa, 9, (void *)0x6UL, GFP_KERNEL);
	lkpi_host_cmn_quiet = 0;
	CHECK(xa_is_err(e));
	CHECK_EQ(xa_err(e), -EINVAL);
	/* Insert. */
	CHECK_EQ(xa_insert(&xa, 7, P(7), GFP_KERNEL), 0);
	CHECK_EQ(xa_insert(&xa, 7, P(8), GFP_KERNEL), -EBUSY);
	CHECK(xa_load(&xa, 7) == P(7));
	/* cmpxchg. */
	CHECK(xa_cmpxchg(&xa, 7, P(1), P(9), GFP_KERNEL) == P(7));
	CHECK(xa_load(&xa, 7) == P(7));
	CHECK(xa_cmpxchg(&xa, 7, P(7), P(9), GFP_KERNEL) == P(7));
	CHECK(xa_load(&xa, 7) == P(9));
	CHECK(xa_cmpxchg(&xa, 8, NULL, P(8), GFP_KERNEL) == NULL);
	CHECK(xa_load(&xa, 8) == P(8));
	CHECK(xa_cmpxchg(&xa, 8, P(8), NULL, GFP_KERNEL) == P(8));
	CHECK(xa_load(&xa, 8) == NULL);
	CHECK(lkpi_xa_slot(&xa, 8) == NULL);
	/* Slots. */
	CHECK(lkpi_xa_slot(&xa, 7) != NULL &&
	    *(void **)lkpi_xa_slot(&xa, 7) == P(9));
	/* Error entries round-trip. */
	CHECK(xa_is_err(xa_mk_err(-ENOMEM)));
	CHECK_EQ(xa_err(xa_mk_err(-ENOMEM)), -ENOMEM);
	CHECK(!xa_is_err(P(1)));
	CHECK(!xa_is_err(xa_mk_value(3)));

	/* Iteration in index order. */
	{
		unsigned long want[] = { 0, 7, ULONG_MAX };

		n = 0;
		xa_for_each(&xa, idx, e) {
			if (n < 3)
				CHECK_EQ(idx, want[n]);
			n++;
		}
		CHECK_EQ(n, 3);
		n = 0;
		xa_for_each_range(&xa, idx, e, 1, 100)
			n++;
		CHECK_EQ(n, 1);
		idx = 8;
		CHECK(xa_find(&xa, &idx, ULONG_MAX, XA_PRESENT) == P(1));
		CHECK_EQ(idx, ULONG_MAX);
		CHECK(xa_find_after(&xa, &idx, ULONG_MAX, XA_PRESENT) == NULL);
		idx = 0;
		CHECK(xa_find(&xa, &idx, 6, XA_PRESENT) != NULL && idx == 0);
		CHECK(xa_find_after(&xa, &idx, 6, XA_PRESENT) == NULL);
		idx = 0;
		CHECK(xa_find(&xa, &idx, ULONG_MAX, XA_MARK_0) == NULL);
	}

	CHECK(xa_erase(&xa, 7) == P(9));
	CHECK(xa_erase(&xa, 7) == NULL);
	xa_destroy(&xa);
	CHECK(xa_empty(&xa));
	CHECK_EQ(lkpi_host_kmem_live, base);
	/* Usable after xa_destroy(). */
	CHECK(xa_store(&xa, 3, P(3), GFP_KERNEL) == NULL);
	xa_destroy(&xa);
	CHECK_EQ(lkpi_host_kmem_live, base);
}

TEST(xarray_alloc)
{
	DEFINE_XARRAY_ALLOC(xa);
	struct xarray xa1;
	unsigned long base = lkpi_host_kmem_live, idx;
	u32 id, next = 0;
	void *e;
	int i, n;

	/* Lowest free index first. */
	for (i = 0; i < 5; i++) {
		CHECK_EQ(xa_alloc(&xa, &id, P(i), xa_limit_32b, GFP_KERNEL), 0);
		CHECK_EQ(id, i);
	}
	CHECK(xa_erase(&xa, 2) == P(2));
	CHECK_EQ(xa_alloc(&xa, &id, P(9), xa_limit_32b, GFP_KERNEL), 0);
	CHECK_EQ(id, 2);
	CHECK_EQ(xa_alloc(&xa, &id, P(9), XA_LIMIT(3, 6), GFP_KERNEL), 0);
	CHECK_EQ(id, 5);
	CHECK_EQ(xa_alloc(&xa, &id, P(9), XA_LIMIT(6, 6), GFP_KERNEL), 0);
	CHECK_EQ(id, 6);
	CHECK_EQ(xa_alloc(&xa, &id, P(9), XA_LIMIT(3, 6), GFP_KERNEL), -EBUSY);

	/* NULL reserves: load sees NULL, the index stays busy. */
	CHECK_EQ(xa_alloc(&xa, &id, NULL, xa_limit_32b, GFP_KERNEL), 0);
	CHECK_EQ(id, 7);
	CHECK(xa_load(&xa, 7) == NULL);
	CHECK_EQ(xa_insert(&xa, 7, P(1), GFP_KERNEL), -EBUSY);
	CHECK(xa_cmpxchg(&xa, 7, NULL, P(1), GFP_KERNEL) == NULL);
	CHECK(xa_load(&xa, 7) == NULL);	/* reserved != NULL: no exchange */
	CHECK_EQ(xa_alloc(&xa, &id, P(8), xa_limit_32b, GFP_KERNEL), 0);
	CHECK_EQ(id, 8);
	/* The searches skip reserved entries. */
	n = 0;
	xa_for_each(&xa, idx, e)
		n++;
	CHECK_EQ(n, 8);
	/* Filling the reservation (i915's context ids). */
	CHECK(xa_store(&xa, 7, P(7), GFP_KERNEL) == NULL);
	CHECK(xa_load(&xa, 7) == P(7));
	/* In an allocating array, storing NULL reserves again. */
	CHECK(xa_store(&xa, 7, NULL, GFP_KERNEL) == P(7));
	CHECK(lkpi_xa_slot(&xa, 7) != NULL);
	CHECK_EQ(xa_alloc(&xa, &id, P(1), XA_LIMIT(7, 9), GFP_KERNEL), 0);
	CHECK_EQ(id, 9);
	xa_release(&xa, 7);
	CHECK(lkpi_xa_slot(&xa, 7) == NULL);
	xa_release(&xa, 8);		/* not reserved: no effect */
	CHECK(xa_load(&xa, 8) == P(8));
	CHECK_EQ(xa_alloc(&xa, &id, P(1), XA_LIMIT(7, 9), GFP_KERNEL), 0);
	CHECK_EQ(id, 7);
	xa_destroy(&xa);

	/* XA_FLAGS_ALLOC1: index 0 is never handed out. */
	xa_init_flags(&xa1, XA_FLAGS_ALLOC1);
	CHECK_EQ(xa_alloc(&xa1, &id, P(1), xa_limit_32b, GFP_KERNEL), 0);
	CHECK_EQ(id, 1);
	CHECK_EQ(xa_alloc(&xa1, &id, P(1), XA_LIMIT(0, 1), GFP_KERNEL),
	    -EBUSY);

	/* Cyclic: continues after the last, wraps (returning 1). */
	xa_destroy(&xa1);
	next = 0;
	CHECK_EQ(xa_alloc_cyclic(&xa1, &id, P(1), XA_LIMIT(1, 4), &next,
	    GFP_KERNEL), 0);
	CHECK_EQ(id, 1);
	CHECK_EQ(xa_alloc_cyclic(&xa1, &id, P(1), XA_LIMIT(1, 4), &next,
	    GFP_KERNEL), 0);
	CHECK_EQ(id, 2);
	CHECK(xa_erase(&xa1, 1) == P(1));
	CHECK_EQ(xa_alloc_cyclic(&xa1, &id, P(1), XA_LIMIT(1, 4), &next,
	    GFP_KERNEL), 0);
	CHECK_EQ(id, 3);
	CHECK_EQ(xa_alloc_cyclic(&xa1, &id, P(1), XA_LIMIT(1, 4), &next,
	    GFP_KERNEL), 0);
	CHECK_EQ(id, 4);
	CHECK_EQ(next, 5);
	CHECK_EQ(xa_alloc_cyclic(&xa1, &id, P(1), XA_LIMIT(1, 4), &next,
	    GFP_KERNEL), 1);
	CHECK_EQ(id, 1);
	CHECK_EQ(xa_alloc_cyclic(&xa1, &id, P(1), XA_LIMIT(1, 4), &next,
	    GFP_KERNEL), -EBUSY);
	/* *next wrapping through 0 reports the wrap on the next success. */
	xa_destroy(&xa1);
	next = UINT_MAX;
	CHECK_EQ(xa_alloc_cyclic(&xa1, &id, P(1), xa_limit_32b, &next,
	    GFP_KERNEL), 0);
	CHECK_EQ(id, UINT_MAX);
	CHECK_EQ(next, 0);
	CHECK_EQ(xa_alloc_cyclic(&xa1, &id, P(1), xa_limit_32b, &next,
	    GFP_KERNEL), 1);
	CHECK_EQ(id, 1);
	xa_destroy(&xa1);

	/* A plain array refuses allocation. */
	{
		struct xarray plain;

		xa_init(&plain);
		lkpi_host_cmn_quiet = 1;
		CHECK_EQ(xa_alloc(&plain, &id, P(1), xa_limit_32b, GFP_KERNEL),
		    -EINVAL);
		lkpi_host_cmn_quiet = 0;
		xa_destroy(&plain);
	}

	/* Allocation failure without sleeping. */
	{
		struct xarray f;

		xa_init(&f);
		lkpi_host_kmem_fail_nosleep = 1;
		e = xa_store(&f, 1, P(1), GFP_NOWAIT);
		CHECK(xa_is_err(e) && xa_err(e) == -ENOMEM);
		CHECK_EQ(xa_insert(&f, 1, P(1), GFP_ATOMIC), -ENOMEM);
		lkpi_host_kmem_fail_nosleep = 0;
		CHECK(xa_empty(&f));
		xa_destroy(&f);
	}
	CHECK_EQ(lkpi_host_kmem_live, base);
}

TEST(xarray_random)
{
	static void *ref[512];
	struct xarray xa;
	unsigned long long seed = 42;
	unsigned long base = lkpi_host_kmem_live, idx;
	bool ok = true;
	int round, i;

	xa_init(&xa);
	memset(ref, 0, sizeof (ref));
	for (round = 0; round < 20000; round++) {
		unsigned long k = t_rand(&seed) % 512;
		void *v = P(t_rand(&seed) % 64);
		void *old;

		switch (t_rand(&seed) % 4) {
		case 0:
		case 1:
			old = xa_store(&xa, k * 1000, v, GFP_KERNEL);
			if (old != ref[k])
				ok = false;
			ref[k] = v;
			break;
		case 2:
			old = xa_erase(&xa, k * 1000);
			if (old != ref[k])
				ok = false;
			ref[k] = NULL;
			break;
		default:
			if (xa_load(&xa, k * 1000) != ref[k])
				ok = false;
			break;
		}
	}
	CHECK(ok);
	/* xa_for_each visits exactly the reference entries, in order. */
	{
		void *e;
		unsigned long prev = 0, n = 0, want = 0;
		bool first = true;

		for (i = 0; i < 512; i++)
			want += ref[i] != NULL;
		xa_for_each(&xa, idx, e) {
			if (!first && idx <= prev)
				ok = false;
			if (idx % 1000 != 0 || ref[idx / 1000] != e)
				ok = false;
			prev = idx;
			first = false;
			n++;
		}
		CHECK(ok);
		CHECK_EQ(n, want);
		CHECK_EQ(xa.xa_count, want);
		CHECK_EQ(lkpi_host_kmem_live - base, want);
	}
	xa_destroy(&xa);
	CHECK_EQ(lkpi_host_kmem_live, base);
}

TEST(radix_tree)
{
	RADIX_TREE(rt, GFP_KERNEL);
	struct radix_tree_iter iter;
	void __rcu **slot;
	unsigned long want[] = { 3, 10, 400 };
	int n = 0;

	CHECK_EQ(radix_tree_insert(&rt, 10, P(10)), 0);
	CHECK_EQ(radix_tree_insert(&rt, 3, P(3)), 0);
	CHECK_EQ(radix_tree_insert(&rt, 400, P(4)), 0);
	CHECK_EQ(radix_tree_insert(&rt, 3, P(3)), -EBUSY);
	CHECK(radix_tree_lookup(&rt, 10) == P(10));
	slot = radix_tree_lookup_slot(&rt, 400);
	CHECK(slot != NULL && radix_tree_deref_slot(slot) == P(4));
	radix_tree_for_each_slot(slot, &rt, &iter, 0) {
		if (n < 3)
			CHECK_EQ(iter.index, want[n]);
		n++;
	}
	CHECK_EQ(n, 3);
	n = 0;
	radix_tree_for_each_slot(slot, &rt, &iter, 4)
		n++;
	CHECK_EQ(n, 2);
	CHECK(radix_tree_delete(&rt, 10) == P(10));
	CHECK(radix_tree_lookup(&rt, 10) == NULL);
	xa_destroy(&rt);
}

static int
idr_sum(int id, void *p, void *data)
{
	*(int *)data += id;
	return (id == 1000 ? 7 : 0);
}

TEST(idr)
{
	struct idr idr;
	unsigned long base = lkpi_host_kmem_live;
	void *e;
	int id, sum, n;

	idr_init(&idr);
	CHECK(idr_is_empty(&idr));
	CHECK_EQ(idr_alloc(&idr, P(1), 0, 0, GFP_KERNEL), 0);
	CHECK_EQ(idr_alloc(&idr, P(2), 0, 0, GFP_KERNEL), 1);
	CHECK_EQ(idr_alloc(&idr, P(3), 5, 7, GFP_KERNEL), 5);
	CHECK_EQ(idr_alloc(&idr, P(4), 5, 7, GFP_KERNEL), 6);
	CHECK_EQ(idr_alloc(&idr, P(5), 5, 7, GFP_KERNEL), -ENOSPC);
	lkpi_host_cmn_quiet = 1;
	CHECK_EQ(idr_alloc(&idr, P(5), -1, 7, GFP_KERNEL), -EINVAL);
	lkpi_host_cmn_quiet = 0;
	CHECK(idr_find(&idr, 5) == P(3));
	CHECK(idr_find(&idr, 4) == NULL);
	/* NULL pointers occupy their id; idr_get_next skips them. */
	CHECK_EQ(idr_alloc(&idr, NULL, 2, 0, GFP_KERNEL), 2);
	CHECK_EQ(idr_alloc(&idr, P(6), 2, 0, GFP_KERNEL), 3);
	id = 2;
	CHECK(idr_get_next(&idr, &id) == P(6));
	CHECK_EQ(id, 3);
	CHECK(idr_replace(&idr, P(7), 2) == NULL);
	CHECK(idr_find(&idr, 2) == P(7));
	CHECK(IS_ERR(idr_replace(&idr, P(7), 100)));
	CHECK_EQ(PTR_ERR(idr_replace(&idr, P(7), 100)), -ENOENT);
	n = 0;
	idr_for_each_entry(&idr, e, id)
		n++;
	CHECK_EQ(n, 6);
	CHECK(idr_remove(&idr, 1) == P(2));
	CHECK(idr_remove(&idr, 1) == NULL);
	CHECK_EQ(idr_alloc(&idr, P(1), 0, 0, GFP_KERNEL), 1);
	CHECK_EQ(idr_alloc(&idr, P(1), 1000, 0, GFP_KERNEL), 1000);
	sum = 0;
	CHECK_EQ(idr_for_each(&idr, idr_sum, &sum), 7);
	CHECK_EQ(sum, 0 + 1 + 2 + 3 + 5 + 6 + 1000);

	/* Cyclic ids continue after the last and wrap to start. */
	idr_destroy(&idr);
	CHECK(idr_is_empty(&idr));
	CHECK_EQ(idr_alloc_cyclic(&idr, P(1), 1, 4, GFP_KERNEL), 1);
	CHECK_EQ(idr_alloc_cyclic(&idr, P(1), 1, 4, GFP_KERNEL), 2);
	CHECK(idr_remove(&idr, 1) == P(1));
	CHECK_EQ(idr_alloc_cyclic(&idr, P(1), 1, 4, GFP_KERNEL), 3);
	CHECK_EQ(idr_alloc_cyclic(&idr, P(1), 1, 4, GFP_KERNEL), 1);
	CHECK_EQ(idr_alloc_cyclic(&idr, P(1), 1, 4, GFP_KERNEL), -ENOSPC);
	idr_destroy(&idr);

	/* A base offsets the ids. */
	idr_init_base(&idr, 1);
	CHECK_EQ(idr_alloc(&idr, P(1), 0, 0, GFP_KERNEL), 1);
	CHECK_EQ(idr_alloc(&idr, P(2), 0, 0, GFP_KERNEL), 2);
	CHECK(idr_find(&idr, 1) == P(1));
	CHECK(idr_find(&idr, 0) == NULL);
	id = 0;
	CHECK(idr_get_next(&idr, &id) == P(1) && id == 1);
	CHECK(idr_remove(&idr, 1) == P(1));
	CHECK_EQ(idr_alloc(&idr, P(3), 1, 2, GFP_KERNEL), 1);
	idr_destroy(&idr);
	CHECK_EQ(lkpi_host_kmem_live, base);
}

TEST(ida)
{
	DEFINE_IDA(ida);
	unsigned long base = lkpi_host_kmem_live;

	CHECK(ida_is_empty(&ida));
	CHECK_EQ(ida_alloc(&ida, GFP_KERNEL), 0);
	CHECK_EQ(ida_alloc(&ida, GFP_KERNEL), 1);
	CHECK_EQ(ida_alloc_min(&ida, 10, GFP_KERNEL), 10);
	CHECK_EQ(ida_alloc_range(&ida, 10, 11, GFP_KERNEL), 11);
	CHECK_EQ(ida_alloc_range(&ida, 10, 11, GFP_KERNEL), -ENOSPC);
	CHECK_EQ(ida_alloc_max(&ida, 1, GFP_KERNEL), -ENOSPC);
	CHECK_EQ(ida_alloc_range(&ida, 0x80000000U, ~0U, GFP_KERNEL),
	    -ENOSPC);
	ida_free(&ida, 0);
	CHECK_EQ(ida_alloc(&ida, GFP_KERNEL), 0);
	lkpi_host_cmn_quiet = 1;
	{
		unsigned long before = lkpi_host_cmn_count;

		ida_free(&ida, 5);	/* not allocated: warns */
		CHECK(lkpi_host_cmn_count == before + 1);
	}
	lkpi_host_cmn_quiet = 0;
	ida_destroy(&ida);
	CHECK(ida_is_empty(&ida));
	CHECK_EQ(lkpi_host_kmem_live, base);
}
