/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Tests of src/lkpi_misc.c and src/lkpi_kobject.c: refcount_t semantics
 * (saturation instead of overflow/underflow, dec_and_test, the
 * dec_and_lock forms), kref, sort()/sort_r(), bsearch(), random bytes,
 * uuid_is_valid(), kobjects and sysfs_emit(), and lkpi_errno() (the
 * Linux error numbers that must not reach user space).
 */

#include <linux/kernel.h>
#include <linux/refcount.h>
#include <linux/kref.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/sort.h>
#include <linux/bsearch.h>
#include <linux/random.h>
#include <linux/uuid.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include "test.h"

TEST(refcount)
{
	refcount_t r = REFCOUNT_INIT(1);
	spinlock_t lock;
	struct lx_mutex mx;
	unsigned long flags;
	unsigned long w0;

	spin_lock_init(&lock);
	(mutex_init)(&mx.m, NULL, MUTEX_DRIVER, NULL);

	refcount_inc(&r);
	CHECK_EQ(refcount_read(&r), 2);
	CHECK(!refcount_dec_and_test(&r));
	CHECK(refcount_dec_and_test(&r));
	CHECK_EQ(refcount_read(&r), 0);

	/* Increment from zero: a use after free; the count saturates. */
	lkpi_host_cmn_quiet = 1;
	w0 = lkpi_host_cmn_count;
	refcount_inc(&r);
	CHECK_EQ((int)refcount_read(&r), REFCOUNT_SATURATED);
	CHECK(lkpi_host_cmn_count == w0 + 1);
	/* A saturated count never reaches zero again. */
	CHECK(!refcount_dec_and_test(&r));
	CHECK(refcount_inc_not_zero(&r));
	CHECK_EQ((int)refcount_read(&r), REFCOUNT_SATURATED);
	lkpi_host_cmn_quiet = 0;

	/* Overflow saturates. */
	refcount_set(&r, INT_MAX);
	lkpi_host_cmn_quiet = 1;
	refcount_inc(&r);
	lkpi_host_cmn_quiet = 0;
	CHECK_EQ((int)refcount_read(&r), REFCOUNT_SATURATED);

	/* Underflow from 1 by refcount_dec() leaks (saturates). */
	refcount_set(&r, 1);
	lkpi_host_cmn_quiet = 1;
	refcount_dec(&r);
	lkpi_host_cmn_quiet = 0;
	CHECK_EQ((int)refcount_read(&r), REFCOUNT_SATURATED);

	refcount_set(&r, 0);
	CHECK(!refcount_inc_not_zero(&r));
	CHECK_EQ(refcount_read(&r), 0);
	refcount_set(&r, 3);
	CHECK(refcount_inc_not_zero(&r));
	CHECK_EQ(refcount_read(&r), 4);
	CHECK(refcount_sub_and_test(4, &r));

	refcount_set(&r, 1);
	CHECK(refcount_dec_if_one(&r));
	CHECK(!refcount_dec_if_one(&r));
	refcount_set(&r, 2);
	CHECK(refcount_dec_not_one(&r));
	CHECK(!refcount_dec_not_one(&r));

	/* dec_and_lock: the lock is held only on the final drop. */
	refcount_set(&r, 2);
	CHECK(!refcount_dec_and_lock(&r, &lock));
	CHECK(!MUTEX_HELD(&lock.rlock));
	CHECK(refcount_dec_and_lock(&r, &lock));
	CHECK(MUTEX_HELD(&lock.rlock));
	spin_unlock(&lock);
	refcount_set(&r, 2);
	CHECK(!refcount_dec_and_lock_irqsave(&r, &lock, &flags));
	CHECK(refcount_dec_and_lock_irqsave(&r, &lock, &flags));
	spin_unlock_irqrestore(&lock, flags);
	refcount_set(&r, 2);
	CHECK(!refcount_dec_and_mutex_lock(&r, &mx));
	CHECK(!MUTEX_HELD(&mx.m));
	CHECK(refcount_dec_and_mutex_lock(&r, &mx));
	CHECK(MUTEX_HELD(&mx.m));
	mutex_unlock(&mx);
}

static int kref_released;

static void
kref_rel(struct kref *k)
{
	kref_released++;
}

TEST(kref)
{
	struct kref k;

	kref_init(&k);
	kref_get(&k);
	CHECK_EQ(kref_read(&k), 2);
	CHECK_EQ(kref_put(&k, kref_rel), 0);
	CHECK_EQ(kref_put(&k, kref_rel), 1);
	CHECK_EQ(kref_released, 1);
	CHECK(!kref_get_unless_zero(&k));
}

static int
cmp_int_p(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;

	return (cmp_int(x, y));
}

static int
cmp_rev_r(const void *a, const void *b, const void *priv)
{
	(*(int *)(uintptr_t)priv)++;
	return (-cmp_int_p(a, b));
}

struct rec {
	char pad[3];
	int key;
	char tail[5];
};

static int
cmp_rec(const void *a, const void *b)
{
	return (cmp_int(((const struct rec *)a)->key,
	    ((const struct rec *)b)->key));
}

static int swaps;

static void
swap_int(void *a, void *b, int size)
{
	int t = *(int *)a;

	*(int *)a = *(int *)b;
	*(int *)b = t;
	swaps++;
}

TEST(sort_bsearch)
{
	static int v[5000];
	static struct rec recs[777];
	unsigned long long seed = 5;
	int i, calls = 0, key;
	bool ok = true;
	int *hit;

	for (i = 0; i < 5000; i++)
		v[i] = (int)(t_rand(&seed) % 1000) - 500;
	sort(v, 5000, sizeof (int), cmp_int_p, NULL);
	for (i = 1; i < 5000; i++) {
		if (v[i - 1] > v[i])
			ok = false;
	}
	CHECK(ok);
	sort_r(v, 5000, sizeof (int), cmp_rev_r, NULL, &calls);
	for (i = 1; i < 5000; i++) {
		if (v[i - 1] < v[i])
			ok = false;
	}
	CHECK(ok);
	CHECK(calls > 0);
	sort(v, 5000, sizeof (int), cmp_int_p, swap_int);
	CHECK(swaps > 0);
	for (i = 1; i < 5000; i++) {
		if (v[i - 1] > v[i])
			ok = false;
	}
	CHECK(ok);
	/* Odd-sized elements use the byte swap. */
	for (i = 0; i < 777; i++) {
		recs[i].key = (int)(t_rand(&seed) % 100000);
		memset(recs[i].tail, i & 0x7f, sizeof (recs[i].tail));
	}
	sort(recs, 777, sizeof (struct rec), cmp_rec, NULL);
	for (i = 1; i < 777; i++) {
		if (recs[i - 1].key > recs[i].key)
			ok = false;
	}
	CHECK(ok);
	sort(v, 0, sizeof (int), cmp_int_p, NULL);
	sort(v, 1, sizeof (int), cmp_int_p, NULL);

	/* bsearch on the sorted array. */
	for (i = 0; i < 5000; i += 37) {
		key = v[i];
		hit = bsearch(&key, v, 5000, sizeof (int), cmp_int_p);
		if (hit == NULL || *hit != key)
			ok = false;
	}
	CHECK(ok);
	key = 100000;
	CHECK(bsearch(&key, v, 5000, sizeof (int), cmp_int_p) == NULL);
	key = -100000;
	CHECK(bsearch(&key, v, 0, sizeof (int), cmp_int_p) == NULL);
}

TEST(random_uuid)
{
	u8 a[33], b[33];
	int i, same = 0;

	memset(a, 0, sizeof (a));
	memset(b, 0, sizeof (b));
	get_random_bytes(a, sizeof (a));
	get_random_bytes(b, sizeof (b));
	for (i = 0; i < 33; i++)
		same += a[i] == b[i];
	CHECK(same < 33);
	/* The fallback when the PRNG is unavailable still varies. */
	lkpi_host_random_fail = 1;
	CHECK(get_random_u64() != get_random_u64());
	get_random_bytes(a, 3);
	lkpi_host_random_fail = 0;
	CHECK(get_random_u32_below(1) == 0);
	CHECK(get_random_u32_below(10) < 10);

	CHECK(uuid_is_valid("123e4567-e89b-12d3-a456-426614174000"));
	CHECK(uuid_is_valid("123E4567-E89B-12D3-A456-426614174000"));
	CHECK(!uuid_is_valid("123e4567e89b-12d3-a456-426614174000-"));
	CHECK(!uuid_is_valid("123e4567-e89b-12d3-a456-42661417400g"));
	CHECK(!uuid_is_valid("123e4567-e89b-12d3-a456-42661417400"));
}

static int ktype_released;

static void
k_release(struct kobject *k)
{
	ktype_released++;
}

static const struct kobj_type k_type = { .release = k_release };

static ssize_t
k_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return (sysfs_emit(buf, "%s=%d\n", kobject_name(k), 7));
}

TEST(kobject_sysfs)
{
	struct kobject parent, child;
	struct kobject *dyn;
	struct kobj_attribute attr = __ATTR(val, 0444, k_show, NULL);
	char *page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
	int n;

	memset(&parent, 0, sizeof (parent));
	memset(&child, 0, sizeof (child));
	CHECK_EQ(kobject_init_and_add(&parent, &k_type, NULL, "card%d", 0), 0);
	CHECK_STR(kobject_name(&parent), "card0");
	CHECK_EQ(kobject_init_and_add(&child, &k_type, &parent, "gt"), 0);
	CHECK(child.parent == &parent);
	CHECK_EQ(kref_read(&parent.kref), 2);
	dyn = kobject_create_and_add("dyn", &child);
	CHECK(dyn != NULL && strcmp(kobject_name(dyn), "dyn") == 0);
	CHECK_EQ(kref_read(&child.kref), 2);

	n = (int)kobj_sysfs_ops.show(&child, &attr.attr, page);
	CHECK_STR(page, "gt=7\n");
	CHECK_EQ(n, 5);
	n = sysfs_emit_at(page, n, "more");
	CHECK_STR(page, "gt=7\nmore");
	lkpi_host_cmn_quiet = 1;
	CHECK_EQ(sysfs_emit(page + 1, "x"), 0);
	CHECK_EQ(sysfs_emit_at(page, PAGE_SIZE, "x"), 0);
	lkpi_host_cmn_quiet = 0;
	CHECK_STR(power_group_name, "power");

	/* Releases cascade from the leaves up. */
	kobject_put(dyn);
	CHECK_EQ(kref_read(&child.kref), 1);
	kobject_put(&parent);
	CHECK_EQ(ktype_released, 0);
	kobject_put(&child);
	CHECK_EQ(ktype_released, 2);
	free(page);
}

TEST(errno_map)
{
	/* illumos' own numbers pass unchanged. */
	CHECK_EQ(lkpi_errno(EINVAL), EINVAL);
	CHECK_EQ(lkpi_errno(ENOTTY), ENOTTY);
	CHECK_EQ(lkpi_errno(EINTR), EINTR);
	/* An interrupted wait or lock: EINTR, as Linux's syscall exit. */
	CHECK_EQ(lkpi_errno(ERESTARTSYS), EINTR);
	CHECK_EQ(lkpi_errno(ERESTARTNOINTR), EINTR);
	CHECK_EQ(lkpi_errno(ERESTARTNOHAND), EINTR);
	CHECK_EQ(lkpi_errno(ERESTART_RESTARTBLOCK), EINTR);
	CHECK_EQ(lkpi_errno(ENOIOCTLCMD), ENOTTY);
	CHECK_EQ(lkpi_errno(EPROBE_DEFER), EAGAIN);
	CHECK_EQ(lkpi_errno(ENOTSUPP), ENOTSUP);
	CHECK_EQ(lkpi_errno(EREMOTEIO), EIO);
	/* Nothing from 512 up stays. */
	for (int e = 512; e < 700; e++)
		CHECK(lkpi_errno(e) < 512);
}
