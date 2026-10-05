/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Tests of the kobject parent/children links of src/lkpi_kobject.c and of
 * lkpi_kobject_orphan_children() as a layer device uses it
 * (lkpi_device_fini(), src/lkpi_device.c): a kobject embedded in memory
 * that its owner frees, with children left under it, including children
 * deleted concurrently with the orphaning. Freed memory is detected by
 * AddressSanitizer.
 */

#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include "test.h"

/* --- a layer device's kobject, as src/lkpi_device.c sets it up ---------- */

struct t_dev {
	char pad[24];
	struct kobject kobj;
	int released;
};

static void
t_dev_kobj_release(struct kobject *kobj)
{
	container_of(kobj, struct t_dev, kobj)->released++;
}

static const struct kobj_type t_dev_ktype = {
	.release = t_dev_kobj_release,
	.sysfs_ops = &kobj_sysfs_ops
};

static struct t_dev *
t_dev_new(const char *name)
{
	struct t_dev *d = calloc(1, sizeof (*d));

	kobject_init(&d->kobj, &t_dev_ktype);
	d->kobj.name = kstrdup(name, GFP_KERNEL);
	return (d);
}

/*
 * lkpi_device_fini() and the device's release: orphan, the last put,
 * free. Returns the number of children orphaned.
 */
static int
t_dev_fini(struct t_dev *d, unsigned int *refs_before_put)
{
	int n = lkpi_kobject_orphan_children(&d->kobj);

	*refs_before_put = kref_read(&d->kobj.kref);
	kobject_put(&d->kobj);
	return (n);
}

/* --- driver kobjects --------------------------------------------------- */

static volatile int t_child_released;

struct t_child {
	struct kobject kobj;
	int id;
};

static void
t_child_release(struct kobject *kobj)
{
	__atomic_add_fetch(&t_child_released, 1, __ATOMIC_SEQ_CST);
	free(container_of(kobj, struct t_child, kobj));
}

static const struct kobj_type t_child_ktype = {
	.release = t_child_release,
	.sysfs_ops = &kobj_sysfs_ops
};

static struct t_child *
t_child_add(struct kobject *parent, int id)
{
	struct t_child *c = calloc(1, sizeof (*c));

	c->id = id;
	if (kobject_init_and_add(&c->kobj, &t_child_ktype, parent, "c%d",
	    id) != 0) {
		kobject_put(&c->kobj);
		return (NULL);
	}
	return (c);
}

/* Whether kobj is on parent's list of children. */
static bool
t_linked(struct kobject *parent, struct kobject *kobj)
{
	struct kobject *k;

	list_for_each_entry(k, &parent->lkpi_children, lkpi_sibling) {
		if (k == kobj)
			return (true);
	}
	return (false);
}

TEST(kobject_children)
{
	struct t_dev *d = t_dev_new("0000:00:02.0");
	struct kobject *gt, *gt0;
	struct t_child *c;
	unsigned int refs;
	unsigned long w0;

	CHECK_STR(kobject_name(&d->kobj), "0000:00:02.0");
	CHECK_EQ(kref_read(&d->kobj.kref), 1);
	CHECK(list_empty(&d->kobj.lkpi_children));

	/* As i915_setup_sysfs(): "gt" under the device. */
	gt = kobject_create_and_add("gt", &d->kobj);
	CHECK(gt != NULL && gt->parent == &d->kobj);
	CHECK_EQ(kref_read(&d->kobj.kref), 2);
	CHECK(t_linked(&d->kobj, gt));
	gt0 = kobject_create_and_add("gt0", gt);
	CHECK(gt0 != NULL && t_linked(gt, gt0) && !t_linked(&d->kobj, gt0));

	/* kobject_del() unlinks and drops the parent reference, once. */
	kobject_del(gt0);
	CHECK(gt0->parent == NULL && !t_linked(gt, gt0));
	CHECK(list_empty(&gt0->lkpi_sibling));
	CHECK_EQ(kref_read(&gt->kref), 1);
	kobject_del(gt0);
	CHECK_EQ(kref_read(&gt->kref), 1);
	/* A deleted kobject may be added again. */
	CHECK_EQ(kobject_add(gt0, gt, NULL), 0);
	CHECK(t_linked(gt, gt0) && kref_read(&gt->kref) == 2);
	kobject_put(gt0);
	CHECK(list_empty(&gt->lkpi_children));

	/* Adding an added kobject is refused and changes no link. */
	c = t_child_add(&d->kobj, 1);
	CHECK(c != NULL);
	lkpi_host_cmn_quiet = 1;
	w0 = lkpi_host_cmn_count;
	CHECK_EQ(kobject_add(&c->kobj, gt, NULL), -EINVAL);
	CHECK(lkpi_host_cmn_count == w0 + 1);
	lkpi_host_cmn_quiet = 0;
	CHECK(c->kobj.parent == &d->kobj && t_linked(&d->kobj, &c->kobj));
	CHECK_EQ(kref_read(&gt->kref), 1);
	CHECK_EQ(kref_read(&d->kobj.kref), 3);
	kobject_put(&c->kobj);

	/* As i915_teardown_sysfs(): every child put, nothing to orphan. */
	kobject_put(gt);
	CHECK_EQ(kref_read(&d->kobj.kref), 1);
	CHECK_EQ(t_dev_fini(d, &refs), 0);
	CHECK_EQ(refs, 1);
	CHECK_EQ(d->released, 1);
	CHECK(d->kobj.name == NULL && !d->kobj.state_initialized);
	free(d);
}

TEST(kobject_uninitialised_parent)
{
	struct kobject parent;
	struct t_child *c = calloc(1, sizeof (*c));
	unsigned long w0;

	memset(&parent, 0, sizeof (parent));
	kobject_init(&c->kobj, &t_child_ktype);
	lkpi_host_cmn_quiet = 1;
	w0 = lkpi_host_cmn_count;
	CHECK_EQ(kobject_add(&c->kobj, &parent, "c"), -EINVAL);
	CHECK(kobject_create_and_add("x", &parent) == NULL);
	CHECK(lkpi_host_cmn_count == w0 + 2);
	lkpi_host_cmn_quiet = 0;
	CHECK(c->kobj.parent == NULL && list_empty(&c->kobj.lkpi_sibling));
	CHECK_EQ(kref_read(&parent.kref), 0);
	/* A zero-filled kobject has nothing to delete or orphan. */
	kobject_del(&parent);
	CHECK_EQ(lkpi_kobject_orphan_children(&parent), 0);
	t_child_released = 0;
	kobject_put(&c->kobj);
	CHECK_EQ(t_child_released, 1);
}

/*
 * The i915 case: "engine" is left under the device with its own children;
 * the device is freed; the leftovers are put afterwards (ASan reports a
 * use of the freed device memory).
 */
TEST(kobject_orphan_leftover)
{
	struct t_dev *d = t_dev_new("0000:00:02.0");
	struct kobject *gt, *engine;
	struct t_child *rcs0, *bcs0, *x;
	unsigned int refs;
	unsigned long w0;

	gt = kobject_create_and_add("gt", &d->kobj);
	engine = kobject_create_and_add("engine", &d->kobj);
	rcs0 = t_child_add(engine, 0);
	bcs0 = t_child_add(engine, 1);
	x = t_child_add(&d->kobj, 9);
	CHECK(gt != NULL && engine != NULL && rcs0 != NULL && bcs0 != NULL &&
	    x != NULL);
	CHECK_EQ(kref_read(&d->kobj.kref), 4);
	CHECK_EQ(kref_read(&engine->kref), 3);
	kobject_put(gt);
	CHECK_EQ(kref_read(&d->kobj.kref), 3);

	t_child_released = 0;
	CHECK_EQ(t_dev_fini(d, &refs), 2);
	CHECK_EQ(refs, 1);
	CHECK_EQ(d->released, 1);
	CHECK(engine->parent == NULL && x->kobj.parent == NULL);
	CHECK(list_empty(&engine->lkpi_sibling));
	CHECK(list_empty(&x->kobj.lkpi_sibling));
	/* The orphans' own subtrees are untouched. */
	CHECK(rcs0->kobj.parent == engine && t_linked(engine, &rcs0->kobj));
	CHECK_EQ(kref_read(&engine->kref), 3);
	memset(d, 0xa5, sizeof (*d));
	free(d);

	/* The driver puts its leftovers later: none touches the device. */
	kobject_put(&rcs0->kobj);
	kobject_put(&bcs0->kobj);
	CHECK_EQ(kref_read(&engine->kref), 1);
	kobject_put(engine);
	kobject_del(&x->kobj);
	kobject_put(&x->kobj);
	CHECK_EQ(t_child_released, 3);

	/* Orphaning on behalf of a caller without a reference is warned. */
	d = t_dev_new("dev");
	x = t_child_add(&d->kobj, 1);
	kobject_put(&d->kobj);
	CHECK_EQ(kref_read(&d->kobj.kref), 1);
	lkpi_host_cmn_quiet = 1;
	w0 = lkpi_host_cmn_count;
	CHECK_EQ(lkpi_kobject_orphan_children(&d->kobj), 1);
	CHECK(lkpi_host_cmn_count == w0 + 1);
	lkpi_host_cmn_quiet = 0;
	CHECK(x->kobj.parent == NULL);
	CHECK_EQ(kref_read(&d->kobj.kref), 1);
	CHECK_EQ(d->released, 0);
	kobject_put(&d->kobj);
	CHECK_EQ(d->released, 1);
	free(d);
	kobject_put(&x->kobj);
}

/* --- concurrency ------------------------------------------------------- */

#define	T_THREADS	4
#define	T_PER_THREAD	8

struct t_worker {
	pthread_t thread;
	struct kobject *parent;
	struct t_child *child[T_PER_THREAD];
	volatile int *go;
	int rounds;		/* > 0: add/put loop; 0: put the children */
	int failed;
};

static void *
t_worker_main(void *arg)
{
	struct t_worker *w = arg;
	int i, r;

	while (!__atomic_load_n(w->go, __ATOMIC_ACQUIRE))
		;
	if (w->rounds > 0) {
		for (r = 0; r < w->rounds; r++) {
			struct t_child *c = t_child_add(w->parent, r);

			if (c == NULL) {
				w->failed++;
				continue;
			}
			if ((r & 1) != 0)
				kobject_del(&c->kobj);
			kobject_put(&c->kobj);
		}
		return (NULL);
	}
	/* Delete some, put the others (the last put deletes). */
	for (i = 0; i < T_PER_THREAD; i++) {
		if ((i & 1) != 0)
			kobject_del(&w->child[i]->kobj);
		kobject_put(&w->child[i]->kobj);
	}
	return (NULL);
}

/* Threads adding and deleting children of one parent: links stay intact. */
TEST(kobject_concurrent_add_del)
{
	struct t_dev *d = t_dev_new("dev");
	struct t_worker w[T_THREADS];
	volatile int go = 0;
	unsigned int refs;
	int i, failed = 0;

	t_child_released = 0;
	memset(w, 0, sizeof (w));
	for (i = 0; i < T_THREADS; i++) {
		w[i].parent = &d->kobj;
		w[i].go = &go;
		w[i].rounds = 2000;
		pthread_create(&w[i].thread, NULL, t_worker_main, &w[i]);
	}
	__atomic_store_n(&go, 1, __ATOMIC_RELEASE);
	for (i = 0; i < T_THREADS; i++) {
		pthread_join(w[i].thread, NULL);
		failed += w[i].failed;
	}
	CHECK_EQ(failed, 0);
	CHECK_EQ(t_child_released, T_THREADS * 2000);
	CHECK(list_empty(&d->kobj.lkpi_children));
	CHECK_EQ(t_dev_fini(d, &refs), 0);
	CHECK_EQ(refs, 1);
	CHECK_EQ(d->released, 1);
	free(d);
}

/*
 * The device's release while other threads delete and put the children
 * left under it: once orphaning returns, the device's kobject has only
 * the owner's reference, and the device memory may be freed while the
 * threads still run (ASan reports a later access).
 */
TEST(kobject_concurrent_orphan)
{
	struct t_worker w[T_THREADS];
	struct t_dev *d;
	volatile int go;
	unsigned int refs;
	int i, j, round, orphaned = 0, bad_refs = 0, bad_release = 0;

	for (round = 0; round < 300; round++) {
		d = t_dev_new("dev");
		go = 0;
		t_child_released = 0;
		memset(w, 0, sizeof (w));
		for (i = 0; i < T_THREADS; i++) {
			w[i].parent = &d->kobj;
			w[i].go = &go;
			for (j = 0; j < T_PER_THREAD; j++)
				w[i].child[j] = t_child_add(&d->kobj, j);
			pthread_create(&w[i].thread, NULL, t_worker_main,
			    &w[i]);
		}
		__atomic_store_n(&go, 1, __ATOMIC_RELEASE);
		/* Let a varying number of deletions go first. */
		for (j = 0; j < (round % 7) * 50; j++)
			__asm__ volatile("" ::: "memory");
		orphaned += t_dev_fini(d, &refs);
		bad_refs += refs != 1;
		bad_release += d->released != 1;
		memset(d, 0xa5, sizeof (*d));
		free(d);
		for (i = 0; i < T_THREADS; i++)
			pthread_join(w[i].thread, NULL);
		CHECK_EQ(t_child_released, T_THREADS * T_PER_THREAD);
	}
	CHECK_EQ(bad_refs, 0);
	CHECK_EQ(bad_release, 0);
	/* Informational: how many children the orphaning found. */
	printf("     kobject_concurrent_orphan: %d of %d children orphaned\n",
	    orphaned, 300 * T_THREADS * T_PER_THREAD);
}
