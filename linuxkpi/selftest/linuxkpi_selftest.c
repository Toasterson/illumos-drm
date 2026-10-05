/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * linuxkpi_selftest: an in-kernel test of the LinuxKPI layer, written as a
 * Linux PCI driver so that it goes through the layer's PCI glue like i915.
 * It binds QEMU's PCI test device (pci-testdev, pci1b36,5) and runs the
 * checks from probe(): locks, workqueues (plain, ordered, delayed,
 * cancel), timers, completions, wait queues, RCU, idr/xarray, slab
 * caches, pages, and DMA scatterlists and coherent memory on the device.
 * Every check logs "linuxkpi_selftest: ok <name>" or "FAIL <name>"; the
 * last line is "linuxkpi_selftest: done <passed> passed <failed> failed",
 * which tools/test/run-r5c-test.sh greps from the system log. The result
 * also becomes the device property "linuxkpi-selftest-failed".
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/rcupdate.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <lkpi/version.h>

static int st_passed, st_failed;

#define	CHECK(name, cond) do { \
	if (cond) { \
		st_passed++; \
		pr_info("linuxkpi_selftest: ok %s\n", name); \
	} else { \
		st_failed++; \
		pr_err("linuxkpi_selftest: FAIL %s (%s:%d)\n", name, \
		    __FILE__, __LINE__); \
	} \
	} while (0)

/* --- locks ------------------------------------------------------------- */

static DEFINE_MUTEX(st_static_mutex);

static void
test_locks(void)
{
	struct mutex m;
	spinlock_t s;
	unsigned long flags;

	mutex_init(&m);
	mutex_lock(&m);
	CHECK("mutex_is_locked", mutex_is_locked(&m));
	mutex_unlock(&m);
	CHECK("mutex_trylock", mutex_trylock(&m));
	CHECK("mutex_is_locked after trylock", mutex_is_locked(&m));
	mutex_unlock(&m);
	CHECK("mutex unlocked", !mutex_is_locked(&m));
	mutex_destroy(&m);

	mutex_lock(&st_static_mutex);
	CHECK("static mutex", mutex_is_locked(&st_static_mutex));
	mutex_unlock(&st_static_mutex);

	spin_lock_init(&s);
	spin_lock_irqsave(&s, flags);
	CHECK("spin_is_locked", spin_is_locked(&s));
	spin_unlock_irqrestore(&s, flags);
	CHECK("spin_trylock", spin_trylock(&s));
	spin_unlock(&s);
	CHECK("spinlock free", !spin_is_locked(&s));
}

/* --- work, timers, completions, wait queues --------------------------- */

struct st_work {
	struct work_struct work;
	struct delayed_work dwork;
	struct completion done;
	atomic_t runs;
	int order[8];
	atomic_t next;
	int id;
};

static void
st_work_fn(struct work_struct *w)
{
	struct st_work *sw = container_of(w, struct st_work, work);
	int slot = atomic_fetch_inc(&sw->next);

	if (slot < 8)
		sw->order[slot] = sw->id;
	atomic_inc(&sw->runs);
	complete(&sw->done);
}

static void
st_dwork_fn(struct work_struct *w)
{
	struct st_work *sw = container_of(to_delayed_work(w), struct st_work,
	    dwork);

	atomic_inc(&sw->runs);
	complete(&sw->done);
}

static void
test_work(void)
{
	struct workqueue_struct *wq, *owq;
	struct st_work *sw;
	int i, ok;

	sw = kzalloc(sizeof (*sw) * 4, GFP_KERNEL);
	if (sw == NULL) {
		CHECK("work: allocation", 0);
		return;
	}
	wq = alloc_workqueue("lkpi_st", 0, 4);
	owq = alloc_ordered_workqueue("lkpi_st_ord", 0);
	CHECK("alloc_workqueue", wq != NULL && owq != NULL);
	if (wq == NULL || owq == NULL)
		goto out;

	init_completion(&sw[0].done);
	INIT_WORK(&sw[0].work, st_work_fn);
	CHECK("queue_work", queue_work(wq, &sw[0].work));
	CHECK("wait_for_completion_timeout",
	    wait_for_completion_timeout(&sw[0].done, msecs_to_jiffies(5000)) > 0);
	flush_work(&sw[0].work);
	CHECK("work ran once", atomic_read(&sw[0].runs) == 1);

	/* Ordered queue: four items of one shared order record. */
	atomic_set(&sw[0].next, 0);
	for (i = 0; i < 4; i++) {
		sw[i].id = i;
		init_completion(&sw[i].done);
		INIT_WORK(&sw[i].work, st_work_fn);
	}
	for (i = 0; i < 4; i++) {
		/* All items record into sw[0]. */
		sw[i].work.func = st_work_fn;
	}
	for (i = 0; i < 4; i++)
		(void) queue_work(owq, &sw[i].work);
	flush_workqueue(owq);
	ok = 1;
	for (i = 0; i < 4; i++)
		ok &= atomic_read(&sw[i].runs) >= 1;
	CHECK("flush_workqueue ran all", ok);

	/* Delayed work fires after its delay; cancel before it fires. */
	init_completion(&sw[1].done);
	atomic_set(&sw[1].runs, 0);
	INIT_DELAYED_WORK(&sw[1].dwork, st_dwork_fn);
	CHECK("queue_delayed_work",
	    queue_delayed_work(wq, &sw[1].dwork, msecs_to_jiffies(50)));
	CHECK("delayed work fired",
	    wait_for_completion_timeout(&sw[1].done, msecs_to_jiffies(5000)) > 0);
	INIT_DELAYED_WORK(&sw[2].dwork, st_dwork_fn);
	atomic_set(&sw[2].runs, 0);
	(void) queue_delayed_work(wq, &sw[2].dwork, msecs_to_jiffies(10000));
	CHECK("cancel_delayed_work_sync of pending",
	    cancel_delayed_work_sync(&sw[2].dwork));
	CHECK("cancelled work did not run", atomic_read(&sw[2].runs) == 0);
out:
	if (owq != NULL)
		destroy_workqueue(owq);
	if (wq != NULL)
		destroy_workqueue(wq);
	kfree(sw);
}

struct st_timer {
	struct timer_list t;
	wait_queue_head_t wq;
	volatile int fired;
	unsigned long when;
};

static void
st_timer_fn(struct timer_list *t)
{
	struct st_timer *st = from_timer(st, t, t);

	st->when = jiffies;
	st->fired = 1;
	wake_up_all(&st->wq);
}

static void
test_timers_and_waits(void)
{
	struct st_timer st = { 0 };
	unsigned long start;
	long left;

	init_waitqueue_head(&st.wq);
	timer_setup(&st.t, st_timer_fn, 0);
	start = jiffies;
	CHECK("mod_timer not pending", mod_timer(&st.t, jiffies +
	    msecs_to_jiffies(100)) == 0);
	CHECK("timer_pending", timer_pending(&st.t));
	left = wait_event_timeout(st.wq, st.fired, msecs_to_jiffies(5000));
	CHECK("wait_event_timeout woken by timer", left > 0 && st.fired);
	CHECK("timer not early", time_after_eq(st.when, start + 90));
	CHECK("timer_delete_sync of fired timer", timer_delete_sync(&st.t) == 0);

	st.fired = 0;
	(void) mod_timer(&st.t, jiffies + msecs_to_jiffies(10000));
	CHECK("timer_delete of pending", timer_delete(&st.t) == 1);
	CHECK("deleted timer not pending", !timer_pending(&st.t));
	left = wait_event_timeout(st.wq, st.fired, msecs_to_jiffies(200));
	CHECK("wait_event_timeout times out", left == 0 && !st.fired);
	msleep(20);
	CHECK("jiffies advance", time_after(jiffies, start));
}

/* --- RCU ----------------------------------------------------------------- */

struct st_rcu {
	struct rcu_head rcu;
	atomic_t *counter;
};

static void
st_rcu_cb(struct rcu_head *h)
{
	struct st_rcu *r = container_of(h, struct st_rcu, rcu);

	atomic_inc(r->counter);
	kfree(r);
}

static void
test_rcu(void)
{
	atomic_t n = ATOMIC_INIT(0);
	int i;

	rcu_read_lock();
	rcu_read_lock();
	rcu_read_unlock();
	rcu_read_unlock();
	synchronize_rcu();
	CHECK("synchronize_rcu returns", 1);
	for (i = 0; i < 16; i++) {
		struct st_rcu *r = kmalloc(sizeof (*r), GFP_KERNEL);

		if (r == NULL)
			break;
		r->counter = &n;
		call_rcu(&r->rcu, st_rcu_cb);
	}
	rcu_barrier();
	CHECK("call_rcu callbacks ran before rcu_barrier returned",
	    atomic_read(&n) == 16);
}

/* --- idr, xarray, slab ----------------------------------------------------- */

static void
test_idr_xarray(void)
{
	struct idr idr;
	struct xarray xa;
	int a, b, c;
	u32 id;
	void *p;

	idr_init(&idr);
	a = idr_alloc(&idr, (void *)0x1000, 1, 0, GFP_KERNEL);
	b = idr_alloc(&idr, (void *)0x2000, 1, 0, GFP_KERNEL);
	CHECK("idr_alloc lowest free", a == 1 && b == 2);
	CHECK("idr_find", idr_find(&idr, 2) == (void *)0x2000);
	(void) idr_remove(&idr, 1);
	c = idr_alloc(&idr, (void *)0x3000, 1, 0, GFP_KERNEL);
	CHECK("idr_alloc reuses", c == 1);
	CHECK("idr range full", idr_alloc(&idr, (void *)0x4000, 1, 3,
	    GFP_KERNEL) == -ENOSPC);
	idr_destroy(&idr);

	xa_init_flags(&xa, XA_FLAGS_ALLOC1);
	CHECK("xa_alloc", xa_alloc(&xa, &id, (void *)0x5000, xa_limit_32b,
	    GFP_KERNEL) == 0 && id == 1);
	CHECK("xa_load", xa_load(&xa, id) == (void *)0x5000);
	p = xa_erase(&xa, id);
	CHECK("xa_erase", p == (void *)0x5000 && xa_empty(&xa));
	CHECK("xa_store value", !xa_is_err(xa_store(&xa, 42, xa_mk_value(7),
	    GFP_KERNEL)) && xa_to_value(xa_load(&xa, 42)) == 7);
	xa_destroy(&xa);
}

struct st_obj {
	u64 a, b;
	char pad[40];
};

static void
test_slab(void)
{
	struct kmem_cache *c = KMEM_CACHE(st_obj, SLAB_HWCACHE_ALIGN);
	struct st_obj *o;
	void *z;

	CHECK("KMEM_CACHE", c != NULL);
	if (c == NULL)
		return;
	o = kmem_cache_zalloc(c, GFP_KERNEL);
	CHECK("kmem_cache_zalloc zeroed", o != NULL && o->a == 0 && o->b == 0);
	if (o != NULL)
		kmem_cache_free(c, o);
	kmem_cache_destroy(c);
	z = kmalloc(0, GFP_KERNEL);
	CHECK("kmalloc(0) is ZERO_SIZE_PTR", z == ZERO_SIZE_PTR);
	kfree(z);
	z = kvzalloc(3 * PAGE_SIZE + 5, GFP_KERNEL);
	CHECK("kvzalloc", z != NULL && ((char *)z)[3 * PAGE_SIZE] == 0);
	kvfree(z);
}

/* --- pages and DMA ----------------------------------------------------------- */

#define	ST_PAGES	8

static void
test_dma(struct pci_dev *pdev)
{
	struct page *pages[ST_PAGES];
	struct sg_table st;
	struct scatterlist *sg;
	dma_addr_t handle = 0;
	void *coherent;
	int i, n, ok;

	CHECK("dma_set_mask_and_coherent(39)",
	    dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(39)) == 0);
	for (i = 0; i < ST_PAGES; i++) {
		pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (pages[i] == NULL)
			break;
		memset(page_address(pages[i]), i, PAGE_SIZE);
	}
	CHECK("alloc_page", i == ST_PAGES);
	if (i != ST_PAGES)
		goto free_pages;
	CHECK("sg_alloc_table", sg_alloc_table(&st, ST_PAGES, GFP_KERNEL) == 0);
	for_each_sg(st.sgl, sg, ST_PAGES, i)
		sg_set_page(sg, pages[i], PAGE_SIZE, 0);
	n = dma_map_sg(&pdev->dev, st.sgl, ST_PAGES, DMA_BIDIRECTIONAL);
	CHECK("dma_map_sg maps every entry", n == ST_PAGES);
	ok = n > 0;
	for_each_sg(st.sgl, sg, n, i) {
		ok &= sg_dma_address(sg) != 0 && sg_dma_len(sg) == PAGE_SIZE &&
		    (sg_dma_address(sg) & (PAGE_SIZE - 1)) == 0 &&
		    sg_dma_address(sg) < DMA_BIT_MASK(39);
	}
	CHECK("dma addresses page-aligned below the mask", ok);
	if (n > 0)
		dma_unmap_sg(&pdev->dev, st.sgl, ST_PAGES, DMA_BIDIRECTIONAL);
	sg_free_table(&st);

	coherent = dma_alloc_coherent(&pdev->dev, 3 * PAGE_SIZE, &handle,
	    GFP_KERNEL);
	CHECK("dma_alloc_coherent", coherent != NULL && handle != 0 &&
	    (handle & (PAGE_SIZE - 1)) == 0);
	if (coherent != NULL) {
		memset(coherent, 0x5a, 3 * PAGE_SIZE);
		dma_free_coherent(&pdev->dev, 3 * PAGE_SIZE, coherent, handle);
	}
free_pages:
	while (i-- > 0)
		__free_page(pages[i]);
}

/* --- the driver ------------------------------------------------------------ */

static int
st_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	u16 vendor = 0;

	st_passed = st_failed = 0;
	CHECK("probe ids", pdev->vendor == 0x1b36 && pdev->device == 0x0005);
	CHECK("pci_read_config_word", pci_read_config_word(pdev, PCI_VENDOR_ID,
	    &vendor) == 0 && vendor == 0x1b36);
	CHECK("dev_name", strlen(dev_name(&pdev->dev)) >= 7);
	test_locks();
	test_work();
	test_timers_and_waits();
	test_rcu();
	test_idr_xarray();
	test_slab();
	test_dma(pdev);
	pr_info("linuxkpi_selftest: done %d passed %d failed\n", st_passed,
	    st_failed);
	(void) ddi_prop_update_int(DDI_DEV_T_NONE, lkpi_dev_dip(&pdev->dev),
	    "linuxkpi-selftest-failed", st_failed);
	return (0);
}

static void
st_remove(struct pci_dev *pdev)
{
	(void) pdev;
}

static const struct pci_device_id st_ids[] = {
	{ PCI_DEVICE(0x1b36, 0x0005) },
	{ 0 }
};

static struct pci_driver st_pci_driver = {
	.name = "linuxkpi_selftest",
	.id_table = st_ids,
	.probe = st_probe,
	.remove = st_remove,
};

static int
st_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	return (lkpi_pci_attach(dip, cmd, &st_pci_driver));
}

static int
st_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	return (lkpi_pci_detach(dip, cmd, &st_pci_driver));
}

/* No minor nodes: nothing to translate. */
static int
st_getinfo(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg, void **result)
{
	return (DDI_FAILURE);
}

static struct cb_ops st_cb_ops = {
	.cb_open = nulldev,
	.cb_close = nulldev,
	.cb_strategy = nodev,
	.cb_print = nodev,
	.cb_dump = nodev,
	.cb_read = nodev,
	.cb_write = nodev,
	.cb_ioctl = nodev,
	.cb_devmap = nodev,
	.cb_mmap = nodev,
	.cb_segmap = nodev,
	.cb_chpoll = nochpoll,
	.cb_prop_op = ddi_prop_op,
	.cb_str = NULL,
	.cb_flag = D_NEW | D_MP,
	.cb_rev = CB_REV,
	.cb_aread = nodev,
	.cb_awrite = nodev
};

static struct dev_ops st_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_getinfo = st_getinfo,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = st_attach,
	.devo_detach = st_detach,
	.devo_reset = nodev,
	.devo_cb_ops = &st_cb_ops,
	.devo_quiesce = ddi_quiesce_not_needed,
};

static struct modldrv st_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = "LinuxKPI self-test",
	.drv_dev_ops = &st_dev_ops,
};

static struct modlinkage st_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &st_modldrv, NULL },
};

int
_init(void)
{
	int ret;

	if ((ret = linuxkpi_check("linuxkpi_selftest", LINUXKPI_ABI)) != 0)
		return (ret);
	if ((ret = pci_register_driver(&st_pci_driver)) != 0)
		return (EINVAL);
	if ((ret = mod_install(&st_modlinkage)) != 0)
		pci_unregister_driver(&st_pci_driver);
	return (ret);
}

int
_fini(void)
{
	int ret;

	if ((ret = mod_remove(&st_modlinkage)) != 0)
		return (ret);
	pci_unregister_driver(&st_pci_driver);
	return (0);
}

int
_info(struct modinfo *mi)
{
	return (mod_info(&st_modlinkage, mi));
}
