/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * linuxkpi_selftest: the memory parts of the layer under contention.
 *
 *	shmem		readers, whole and partial truncation and the last
 *			fput() race on one shmem file; a page outlives the
 *			file's release
 *	vmap		two threads vmap()/vunmap()/vfree() the same pages
 *			(heap-loaded, contiguous write-back, write-combining
 *			alias); VM_MAP_PUT_PAGES
 *	dma		two threads dma_map_sg()/dma_unmap_sg()/
 *			dma_map_sgtable()/sg_free_table() of the same pages
 *			(also with a mapping left for sg_free_table());
 *			bindings that would need a copy buffer are refused
 *	typesafe RCU	a reader keeps a freed SLAB_TYPESAFE_BY_RCU object
 *			valid across reallocation and kmem_cache_destroy()
 *	shrinker	shrinker_free() during a running scan; one pass at a
 *			time; the rate limit; register/free under allocation
 *	kmalloc		krealloc(), virt_to_page() of kmalloc() memory from
 *			two threads, and kfree() dropping its descriptor
 *
 * pin_user_pages() is not tested: probe() runs in a kernel thread without
 * a user address space to pin.
 *
 * Workers are layer kthreads released together by one completion. Every
 * wait is bounded; after a timeout a test reports FAIL and leaks its state
 * (a worker may still use it) instead of freeing it.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/rcupdate.h>
#include <linux/scatterlist.h>
#include <linux/sched.h>
#include <linux/shmem_fs.h>
#include <linux/shrinker.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "st.h"

#define	ST_WAIT_MS	5000	/* a wait that should succeed */
#define	ST_BLOCK_MS	300	/* a wait that should time out */
#define	ST_MAXW		4

/* --- workers ------------------------------------------------------------ */

struct st_gang;

struct st_worker {
	struct st_gang		*g;
	int			id;
	int			errors;
	struct task_struct	*task;
	struct completion	done;
};

struct st_gang {
	struct completion	go;
	int			(*fn)(struct st_worker *w);
	void			*arg;
	int			n;
	struct st_worker	w[ST_MAXW];
};

static int
st_worker_main(void *data)
{
	struct st_worker *w = data;

	if (wait_for_completion_timeout(&w->g->go,
	    msecs_to_jiffies(ST_WAIT_MS)) == 0)
		w->errors++;
	else
		w->errors += w->g->fn(w);
	complete(&w->done);
	return (0);
}

/* Starts n workers of fn and releases them together; false if one failed. */
static bool
st_gang_start(struct st_gang *g, int n, int (*fn)(struct st_worker *),
    void *arg)
{
	int i;

	init_completion(&g->go);
	g->fn = fn;
	g->arg = arg;
	g->n = 0;
	for (i = 0; i < n && i < ST_MAXW; i++) {
		struct st_worker *w = &g->w[i];

		w->g = g;
		w->id = i;
		w->errors = 0;
		init_completion(&w->done);
		w->task = kthread_run(st_worker_main, w, "st_mem_%d", i);
		if (IS_ERR(w->task))
			break;
		g->n++;
	}
	complete_all(&g->go);
	return (g->n == n);
}

/*
 * Waits for the started workers; the sum of their errors, or -1 if one
 * did not finish (the gang must then be leaked).
 */
static int
st_gang_wait(struct st_gang *g)
{
	int i, errors = 0;
	bool all = true;

	for (i = 0; i < g->n; i++) {
		struct st_worker *w = &g->w[i];

		if (wait_for_completion_timeout(&w->done,
		    msecs_to_jiffies(4 * ST_WAIT_MS)) == 0) {
			all = false;
			continue;
		}
		(void) kthread_stop(w->task);
		errors += w->errors;
	}
	return (all ? errors : -1);
}

/* --- shmem: read, truncate, put_page, fput -------------------------------- */

#define	ST_SHM_PAGES	64
#define	ST_SHM_READS	2000
#define	ST_SHM_TRUNCS	400

struct st_shm {
	struct st_gang		g;
	struct lx_file		*file;
	struct address_space	*mapping;
	struct inode		*inode;
};

/*
 * Workers 0 and 1 read pages of their own parity (below the last page)
 * and check that the page they hold keeps their pattern, whatever
 * truncation does meanwhile; workers 2 and 3 truncate the whole file and
 * (partially) its last page, which the readers never touch.
 */
static int
st_shm_worker(struct st_worker *w)
{
	struct st_shm *t = w->g->arg;
	struct lx_page *page;
	u32 *p, pat;
	int it, errors = 0;
	pgoff_t idx;

	if (w->id < 2) {
		for (it = 0; it < ST_SHM_READS; it++) {
			idx = ((it * 7) % (ST_SHM_PAGES / 2 - 1)) * 2 + w->id;
			page = shmem_read_mapping_page(t->mapping, idx);
			if (IS_ERR(page)) {
				errors++;
				continue;
			}
			p = page_address(page);
			pat = ((u32)w->id << 24) | (u32)it;
			if (p == NULL || page->index != idx) {
				errors++;
			} else {
				p[0] = pat;
				p[PAGE_SIZE / sizeof (u32) - 1] = pat;
				if ((it & 15) == 0)
					(void) cond_resched();
				if (p[0] != pat ||
				    p[PAGE_SIZE / sizeof (u32) - 1] != pat)
					errors++;
			}
			put_page(page);
		}
	} else {
		for (it = 0; it < ST_SHM_TRUNCS; it++) {
			if ((it & 3) == 3) {
				shmem_truncate_range(t->inode,
				    (loff_t)(ST_SHM_PAGES - 1) * PAGE_SIZE,
				    (loff_t)ST_SHM_PAGES * PAGE_SIZE - 1);
			} else {
				shmem_truncate_range(t->inode, 0, (loff_t)-1);
			}
			if ((it & 7) == 0)
				(void) cond_resched();
		}
	}
	/* The worker's file reference; the last one releases the file. */
	fput(t->file);
	return (errors);
}

static void
st_shmem(void)
{
	struct st_shm *t;
	struct lx_file *f;
	struct lx_page *page, *again;
	u32 *p;
	int i, errors;
	bool ok;

	/* Alone: data stays, whole truncation zeroes, a page outlives fput. */
	f = shmem_file_setup("st_mem", 4 * PAGE_SIZE, EMPTY_VMA_FLAGS);
	CHECK("shmem_file_setup", !IS_ERR(f));
	if (IS_ERR(f))
		return;
	page = shmem_read_mapping_page(f->f_mapping, 1);
	CHECK("shmem_read_mapping_page", !IS_ERR(page));
	if (IS_ERR(page)) {
		fput(f);
		return;
	}
	p = page_address(page);
	p[3] = 0x5eed;
	again = shmem_read_mapping_page(f->f_mapping, 1);
	CHECK("shmem: same page, same data", again == page && p[3] == 0x5eed);
	if (!IS_ERR(again))
		put_page(again);
	shmem_truncate_range(file_inode(f), 0, (loff_t)-1);
	CHECK("shmem: a held page keeps its memory over truncation",
	    p[3] == 0x5eed);
	again = shmem_read_mapping_page(f->f_mapping, 1);
	CHECK("shmem: truncation gives a new zeroed page", !IS_ERR(again) &&
	    again != page && ((u32 *)page_address(again))[3] == 0);
	if (!IS_ERR(again))
		put_page(again);
	fput(f);
	p[4] = 0xfeed;
	CHECK("shmem: a held page outlives the file", p[3] == 0x5eed &&
	    p[4] == 0xfeed);
	put_page(page);

	/* Contended. */
	t = kzalloc(sizeof (*t), GFP_KERNEL);
	if (t == NULL) {
		CHECK("shmem: state", 0);
		return;
	}
	t->file = shmem_file_setup("st_mem_c", ST_SHM_PAGES * PAGE_SIZE,
	    EMPTY_VMA_FLAGS);
	if (IS_ERR(t->file)) {
		CHECK("shmem: contended file", 0);
		kfree(t);
		return;
	}
	t->mapping = t->file->f_mapping;
	t->inode = file_inode(t->file);
	/* One reference per worker, taken while ours keeps the file. */
	for (i = 0; i < ST_MAXW; i++)
		(void) get_file(t->file);
	ok = st_gang_start(&t->g, ST_MAXW, st_shm_worker, t);
	/* Ours goes now: the last fput() races with the workers. */
	fput(t->file);
	/* References of workers that did not start. */
	for (i = t->g.n; i < ST_MAXW; i++)
		fput(t->file);
	CHECK("shmem: workers started", ok);
	errors = st_gang_wait(&t->g);
	CHECK("shmem: read/truncate/put_page/fput race", errors == 0);
	if (errors >= 0)
		kfree(t);
}

/* --- vmap ----------------------------------------------------------------- */

#define	ST_VMAP_ITERS	300
#define	ST_PAT_A	0xa5a5a5a5U
#define	ST_PAT_B	0x5b5b5b5bU

struct st_vm {
	struct st_gang		g;
	struct lx_page		*a, *b;
	struct lx_page		*heap[4];	/* a, b, a, b: not contiguous */
	struct lx_page		*shm[4];	/* virtually contiguous */
	struct lx_file		*file;
};

static int
st_vm_worker(struct st_worker *w)
{
	struct st_vm *t = w->g->arg;
	volatile u32 *wc32;
	char *va, *wb, *wc;
	int it, errors = 0;
	u32 v;

	for (it = 0; it < ST_VMAP_ITERS; it++) {
		/* Heap-loaded: page a appears at 0 and 2, b at 1 and 3. */
		va = vmap(t->heap, 4, VM_MAP, PAGE_KERNEL);
		if (va == NULL) {
			errors++;
		} else {
			/* i915 unmaps an object's mapping only if this holds. */
			if (!is_vmalloc_addr(va) ||
			    !is_vmalloc_addr(va + 3 * PAGE_SIZE + 10))
				errors++;
			if (((u32 *)va)[0] != ST_PAT_A ||
			    ((u32 *)(va + PAGE_SIZE))[5] != ST_PAT_B ||
			    ((u32 *)(va + 2 * PAGE_SIZE))[7] != ST_PAT_A ||
			    vmalloc_to_page(va + 3 * PAGE_SIZE + 10) != t->b)
				errors++;
			/* Linux allows vfree() of a vmap() address. */
			if (it % 5 == 0)
				vfree(va);
			else
				vunmap(va);
		}

		/* Contiguous write-back: the pages' own address. */
		wb = vmap(t->shm, 4, VM_MAP, PAGE_KERNEL);
		if (wb != page_address(t->shm[0]) || !is_vmalloc_addr(wb))
			errors++;
		if (wb != NULL)
			vunmap(wb);

		/* Write-combining: a mapping of its own, a page per worker. */
		if (it % 3 == 0) {
			wc = vmap(t->shm, 4, VM_MAP,
			    pgprot_writecombine(PAGE_KERNEL));
			if (wc == NULL || wc == page_address(t->shm[0])) {
				errors++;
			} else {
				wc32 = (volatile u32 *)(wc +
				    (2 + w->id) * PAGE_SIZE) +
				    it % (PAGE_SIZE / sizeof (u32));
				v = ((u32)w->id << 16) | (u32)it;
				*wc32 = v;
				wmb();
				if (*wc32 != v || vmalloc_to_page(wc +
				    PAGE_SIZE) != t->shm[1])
					errors++;
			}
			if (wc != NULL)
				vunmap(wc);
		}
		if ((it & 15) == 0)
			(void) cond_resched();
	}
	return (errors);
}

static void
st_vmap(void)
{
	struct st_vm *t;
	struct lx_page **arr;
	char *va;
	int i, before, errors = -1;
	bool contig = true;

	t = kzalloc(sizeof (*t), GFP_KERNEL);
	if (t == NULL) {
		CHECK("vmap: state", 0);
		return;
	}
	t->a = alloc_page(GFP_KERNEL);
	t->b = alloc_page(GFP_KERNEL);
	t->file = shmem_file_setup("st_vmap", 4 * PAGE_SIZE, EMPTY_VMA_FLAGS);
	if (t->a == NULL || t->b == NULL || IS_ERR(t->file)) {
		CHECK("vmap: pages", 0);
		goto out;
	}
	for (i = 0; i < PAGE_SIZE / sizeof (u32); i++) {
		((u32 *)page_address(t->a))[i] = ST_PAT_A;
		((u32 *)page_address(t->b))[i] = ST_PAT_B;
	}
	t->heap[0] = t->heap[2] = t->a;
	t->heap[1] = t->heap[3] = t->b;
	for (i = 0; i < 4; i++) {
		t->shm[i] = shmem_read_mapping_page(t->file->f_mapping, i);
		if (IS_ERR(t->shm[i])) {
			t->shm[i] = NULL;
			CHECK("vmap: shmem pages", 0);
			goto out;
		}
		if (page_address(t->shm[i]) !=
		    (char *)page_address(t->shm[0]) + i * PAGE_SIZE)
			contig = false;
	}
	CHECK("shmem pages are virtually contiguous", contig);

	CHECK("vmap: workers started", st_gang_start(&t->g, 2, st_vm_worker,
	    t));
	errors = st_gang_wait(&t->g);
	CHECK("vmap/vunmap/vfree of the same pages (WB, WC) race",
	    errors == 0);
	if (errors < 0)
		return;

	/* is_vmalloc_addr(): live mappings only (i915's unmap_object()). */
	va = vmap(t->heap, 4, VM_MAP, PAGE_KERNEL);
	CHECK("is_vmalloc_addr() of a vmap() address",
	    va != NULL && is_vmalloc_addr(va));
	if (va != NULL) {
		vunmap(va);
		CHECK("is_vmalloc_addr() after vunmap() is false",
		    !is_vmalloc_addr(va));
	}
	va = kmalloc(64, GFP_KERNEL);
	CHECK("is_vmalloc_addr() of kmalloc() memory is false",
	    va != NULL && !is_vmalloc_addr(va));
	kfree(va);

	/* VM_MAP_PUT_PAGES: vfree() puts the pages and frees the array. */
	arr = kmalloc_array(2, sizeof (*arr), GFP_KERNEL);
	if (arr != NULL) {
		get_page(t->a);
		get_page(t->b);
		before = page_count(t->a);
		arr[0] = t->a;
		arr[1] = t->b;
		va = vmap(arr, 2, VM_MAP | VM_MAP_PUT_PAGES, PAGE_KERNEL);
		CHECK("vmap(VM_MAP_PUT_PAGES)", va != NULL);
		if (va != NULL) {
			vfree(va);
			CHECK("vfree of a VM_MAP_PUT_PAGES vmap puts the pages",
			    page_count(t->a) == before - 1);
		} else {
			put_page(t->a);
			put_page(t->b);
			kfree(arr);
		}
	}
out:
	for (i = 0; i < 4; i++) {
		if (t->shm[i] != NULL)
			put_page(t->shm[i]);
	}
	if (!IS_ERR_OR_NULL(t->file))
		fput(t->file);
	if (t->a != NULL)
		__free_page(t->a);
	if (t->b != NULL)
		__free_page(t->b);
	kfree(t);
}

/* --- DMA ------------------------------------------------------------------ */

#define	ST_DMA_PAGES	8
#define	ST_DMA_ITERS	200

struct st_dm {
	struct st_gang		g;
	struct device		*dev;
	struct lx_page		*pages[ST_DMA_PAGES];
};

static int
st_dm_worker(struct st_worker *w)
{
	struct st_dm *t = w->g->arg;
	struct scatterlist *sg;
	struct sg_table st;
	int it, j, n, errors = 0;

	for (it = 0; it < ST_DMA_ITERS; it++) {
		if (sg_alloc_table(&st, ST_DMA_PAGES, GFP_KERNEL) != 0) {
			errors++;
			continue;
		}
		for_each_sg(st.sgl, sg, ST_DMA_PAGES, j) {
			sg_set_page(sg, t->pages[(j + w->id) % ST_DMA_PAGES],
			    PAGE_SIZE, 0);
		}
		if (it & 1) {
			if (dma_map_sgtable(t->dev, &st, DMA_TO_DEVICE, 0) !=
			    0)
				errors++;
			else
				dma_unmap_sgtable(t->dev, &st, DMA_TO_DEVICE,
				    0);
		} else {
			n = dma_map_sg(t->dev, st.sgl, ST_DMA_PAGES,
			    DMA_BIDIRECTIONAL);
			if (n != ST_DMA_PAGES)
				errors++;
			for_each_sg(st.sgl, sg, n, j) {
				if (sg_dma_address(sg) == 0 ||
				    sg_dma_len(sg) != PAGE_SIZE ||
				    (sg_dma_address(sg) & (PAGE_SIZE - 1)) != 0)
					errors++;
			}
			/* Now and then sg_free_table() drops the mapping. */
			if (n > 0 && it % 16 != 14)
				dma_unmap_sg(t->dev, st.sgl, ST_DMA_PAGES,
				    DMA_BIDIRECTIONAL);
		}
		sg_free_table(&st);
		if ((it & 15) == 0)
			(void) cond_resched();
	}
	return (errors);
}

static void
st_dma(struct pci_dev *pdev)
{
	struct st_dm *t;
	struct sg_table st;
	struct device *dev = &pdev->dev;
	dma_addr_t addr;
	u64 pa, mask = DMA_BIT_MASK(20);
	bool beyond;
	int i, n, errors;

	t = kzalloc(sizeof (*t), GFP_KERNEL);
	if (t == NULL) {
		CHECK("dma: state", 0);
		return;
	}
	t->dev = dev;
	for (i = 0; i < ST_DMA_PAGES; i++) {
		if ((t->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO)) ==
		    NULL)
			break;
	}
	if (i != ST_DMA_PAGES) {
		CHECK("dma: pages", 0);
		goto out;
	}

	/*
	 * A page beyond the device's reach would be bounced through a copy
	 * buffer, which i915 never syncs: the layer refuses the binding.
	 * Whether the page is beyond a 1 MiB mask is checked, not assumed.
	 */
	pa = page_to_phys(t->pages[0]);
	beyond = pa + PAGE_SIZE > mask;
	CHECK("dma_set_mask(20)", dma_set_mask(dev, mask) == 0);
	addr = dma_map_page(dev, t->pages[0], 0, PAGE_SIZE, DMA_TO_DEVICE);
	CHECK("dma_map_page beyond the mask fails (no copy buffer)",
	    beyond ? addr == DMA_MAPPING_ERROR : addr != DMA_MAPPING_ERROR);
	if (addr != DMA_MAPPING_ERROR)
		dma_unmap_page(dev, addr, PAGE_SIZE, DMA_TO_DEVICE);
	if (sg_alloc_table(&st, 1, GFP_KERNEL) == 0) {
		sg_set_page(st.sgl, t->pages[0], PAGE_SIZE, 0);
		n = dma_map_sg(dev, st.sgl, 1, DMA_TO_DEVICE);
		CHECK("dma_map_sg beyond the mask fails (no copy buffer)",
		    beyond ? n == 0 : n == 1);
		if (n > 0)
			dma_unmap_sg(dev, st.sgl, 1, DMA_TO_DEVICE);
		sg_free_table(&st);
	}
	CHECK("dma_set_mask(39) again",
	    dma_set_mask(dev, DMA_BIT_MASK(39)) == 0);

	CHECK("dma: workers started", st_gang_start(&t->g, 2, st_dm_worker,
	    t));
	errors = st_gang_wait(&t->g);
	CHECK("dma_map_sg/dma_unmap_sg/sg_free_table race", errors == 0);
	if (errors < 0)
		return;
out:
	for (i = 0; i < ST_DMA_PAGES; i++) {
		if (t->pages[i] != NULL)
			__free_page(t->pages[i]);
	}
	kfree(t);
}

/* --- SLAB_TYPESAFE_BY_RCU ------------------------------------------------- */

#define	ST_TS_MAGIC	0x7e57ab1eU
#define	ST_TS_OTHERS	32

struct st_tobj {
	u32	magic;
	u32	pad[15];
};

struct st_ts {
	struct lkpi_kmem_cache	*c;
	struct st_tobj		*obj;
	struct completion	in_reader, freed, reader_done;
	struct completion	gp_done, destroyed;
	bool			reader_ok;
	struct task_struct	*reader, *gp, *destroyer;
};

/*
 * Holds a read section across the free, the reallocations and the
 * kmem_cache_destroy() of the main thread. It sleeps inside it: the
 * layer's readers are a count (src/lkpi_rcu.c), unlike Linux's.
 */
static int
st_ts_reader(void *arg)
{
	struct st_ts *t = arg;
	struct st_tobj *o;

	rcu_read_lock();
	o = READ_ONCE(t->obj);
	complete(&t->in_reader);
	(void) wait_for_completion_timeout(&t->freed,
	    msecs_to_jiffies(4 * ST_WAIT_MS));
	t->reader_ok = o->magic == ST_TS_MAGIC;
	rcu_read_unlock();
	complete(&t->reader_done);
	return (0);
}

static int
st_ts_gp(void *arg)
{
	struct st_ts *t = arg;

	synchronize_rcu();
	complete(&t->gp_done);
	return (0);
}

static int
st_ts_destroyer(void *arg)
{
	struct st_ts *t = arg;

	kmem_cache_destroy(t->c);
	complete(&t->destroyed);
	return (0);
}

static void
st_typesafe(void)
{
	struct st_tobj *others[ST_TS_OTHERS];
	struct st_ts *t;
	bool reused = false, ok = true;
	int i, n;

	t = kzalloc(sizeof (*t), GFP_KERNEL);
	if (t == NULL) {
		CHECK("typesafe: state", 0);
		return;
	}
	init_completion(&t->in_reader);
	init_completion(&t->freed);
	init_completion(&t->reader_done);
	init_completion(&t->gp_done);
	init_completion(&t->destroyed);
	t->c = kmem_cache_create("st_typesafe", sizeof (struct st_tobj), 0,
	    SLAB_TYPESAFE_BY_RCU, NULL);
	CHECK("kmem_cache_create(SLAB_TYPESAFE_BY_RCU)", t->c != NULL);
	if (t->c == NULL) {
		kfree(t);
		return;
	}
	t->obj = kmem_cache_alloc(t->c, GFP_KERNEL);
	if (t->obj == NULL) {
		CHECK("typesafe: object", 0);
		kmem_cache_destroy(t->c);
		kfree(t);
		return;
	}
	t->obj->magic = ST_TS_MAGIC;

	t->reader = kthread_run(st_ts_reader, t, "st_ts_reader");
	if (IS_ERR(t->reader)) {
		CHECK("typesafe: reader", 0);
		kmem_cache_free(t->c, t->obj);
		kmem_cache_destroy(t->c);
		kfree(t);
		return;
	}
	if (wait_for_completion_timeout(&t->in_reader,
	    msecs_to_jiffies(ST_WAIT_MS)) == 0) {
		CHECK("typesafe: reader entered its section", 0);
		return;		/* leak: the reader may still run */
	}
	t->gp = kthread_run(st_ts_gp, t, "st_ts_gp");

	/* The free is deferred: the buffer must not come back yet. */
	kmem_cache_free(t->c, t->obj);
	for (n = 0; n < ST_TS_OTHERS; n++) {
		if ((others[n] = kmem_cache_alloc(t->c, GFP_KERNEL)) == NULL)
			break;
		if (others[n] == t->obj)
			reused = true;
		others[n]->magic = 0;
	}
	CHECK("typesafe: freed object not reused within the grace period",
	    !reused && n == ST_TS_OTHERS);
	if (!IS_ERR(t->gp)) {
		CHECK("synchronize_rcu waits for the reader",
		    wait_for_completion_timeout(&t->gp_done,
		    msecs_to_jiffies(ST_BLOCK_MS)) == 0);
	}
	for (i = 0; i < n; i++)
		kmem_cache_free(t->c, others[i]);

	/* destroy with frees pending and the reader inside: it must wait. */
	t->destroyer = kthread_run(st_ts_destroyer, t, "st_ts_destroy");
	if (!IS_ERR(t->destroyer)) {
		CHECK("kmem_cache_destroy waits for the grace period",
		    wait_for_completion_timeout(&t->destroyed,
		    msecs_to_jiffies(ST_BLOCK_MS)) == 0);
	}
	complete(&t->freed);
	if (wait_for_completion_timeout(&t->reader_done,
	    msecs_to_jiffies(ST_WAIT_MS)) == 0)
		ok = false;
	CHECK("typesafe: the reader saw the object intact", ok &&
	    t->reader_ok);
	if (!IS_ERR(t->gp) && wait_for_completion_timeout(&t->gp_done,
	    msecs_to_jiffies(ST_WAIT_MS)) == 0)
		ok = false;
	if (IS_ERR(t->destroyer)) {
		kmem_cache_destroy(t->c);
	} else if (wait_for_completion_timeout(&t->destroyed,
	    msecs_to_jiffies(ST_WAIT_MS)) == 0) {
		ok = false;
	}
	/* Reaching this line means no deferred free hit a destroyed cache. */
	CHECK("typesafe: grace period and destroy completed", ok);
	if (!ok)
		return;		/* leak: a thread may still run */
	(void) kthread_stop(t->reader);
	if (!IS_ERR(t->gp))
		(void) kthread_stop(t->gp);
	if (!IS_ERR(t->destroyer))
		(void) kthread_stop(t->destroyer);
	kfree(t);
}

/* --- shrinkers ------------------------------------------------------------ */

struct st_sh {
	struct st_gang		g;
	struct completion	entered, release, freed;
	atomic_t		counts, scans, inside, max_inside;
	unsigned long		nr_to_scan;
	bool			block;
	struct shrinker		*s;
};

static unsigned long
st_sh_count(struct shrinker *s, struct shrink_control *sc)
{
	struct st_sh *t = s->private_data;

	atomic_inc(&t->counts);
	return (100);
}

static unsigned long
st_sh_scan(struct shrinker *s, struct shrink_control *sc)
{
	struct st_sh *t = s->private_data;
	int in = atomic_inc_return(&t->inside);

	if (in > atomic_read(&t->max_inside))
		atomic_set(&t->max_inside, in);
	t->nr_to_scan = sc->nr_to_scan;
	atomic_inc(&t->scans);
	complete(&t->entered);
	if (t->block)
		(void) wait_for_completion_timeout(&t->release,
		    msecs_to_jiffies(ST_WAIT_MS));
	atomic_dec(&t->inside);
	return (sc->nr_to_scan);
}

static int
st_sh_freer(void *arg)
{
	struct st_sh *t = arg;

	shrinker_free(t->s);
	complete(&t->freed);
	return (0);
}

static struct shrinker *
st_sh_new(struct st_sh *t, int n)
{
	struct shrinker *s = shrinker_alloc(0, "st_mem-%d", n);

	if (s != NULL) {
		s->count_objects = st_sh_count;
		s->scan_objects = st_sh_scan;
		s->private_data = t;
		s->batch = 16;
	}
	return (s);
}

/* Allocation churn while shrinkers come and go. */
static int
st_sh_worker(struct st_worker *w)
{
	int it, errors = 0;
	char *p;

	for (it = 0; it < 1000; it++) {
		p = kmalloc(64 + (it % 3) * PAGE_SIZE, GFP_KERNEL);
		if (p == NULL) {
			errors++;
			continue;
		}
		p[0] = (char)it;
		kfree(p);
		if ((it & 63) == 0)
			(void) cond_resched();
	}
	return (errors);
}

static void
st_shrinker(void)
{
	struct task_struct *freer;
	struct shrinker *s;
	struct st_sh *t;
	int i, errors, scans;

	t = kzalloc(sizeof (*t), GFP_KERNEL);
	if (t == NULL) {
		CHECK("shrinker: state", 0);
		return;
	}
	init_completion(&t->entered);
	init_completion(&t->release);
	init_completion(&t->freed);
	t->block = true;
	if ((t->s = st_sh_new(t, 0)) == NULL) {
		CHECK("shrinker_alloc", 0);
		kfree(t);
		return;
	}
	shrinker_register(t->s);
	shrinker_register(t->s);	/* a second register is a no-op */

	/* A pass runs the scan on the layer's task queue. */
	lkpi_shrink_request(true);
	if (wait_for_completion_timeout(&t->entered,
	    msecs_to_jiffies(ST_WAIT_MS)) == 0) {
		CHECK("shrink pass calls count and scan", 0);
		shrinker_free(t->s);
		kfree(t);
		return;
	}
	CHECK("shrink pass calls count and scan",
	    atomic_read(&t->counts) >= 1 && t->nr_to_scan == 16);
	/* A pass is running: these are dropped. */
	lkpi_shrink_request(true);
	lkpi_shrink_request(false);

	freer = kthread_run(st_sh_freer, t, "st_sh_free");
	if (IS_ERR(freer)) {
		CHECK("shrinker: freer thread", 0);
		complete(&t->release);
		msleep(100);
		shrinker_free(t->s);
	} else {
		CHECK("shrinker_free waits for a running scan",
		    wait_for_completion_timeout(&t->freed,
		    msecs_to_jiffies(ST_BLOCK_MS)) == 0);
		complete(&t->release);
		if (wait_for_completion_timeout(&t->freed,
		    msecs_to_jiffies(ST_WAIT_MS)) == 0) {
			CHECK("shrinker_free returns after the scan", 0);
			return;		/* leak */
		}
		CHECK("shrinker_free returns after the scan", 1);
		(void) kthread_stop(freer);
	}
	CHECK("one shrink pass at a time", atomic_read(&t->max_inside) == 1 &&
	    atomic_read(&t->scans) == 1);
	scans = atomic_read(&t->scans);
	lkpi_shrink_request(true);
	msleep(200);
	CHECK("a freed shrinker is not called",
	    atomic_read(&t->scans) == scans);

	/* Register and free under allocation churn, with forced passes. */
	t->block = false;
	CHECK("shrinker: workers started", st_gang_start(&t->g, 2,
	    st_sh_worker, t));
	for (i = 1; i <= 50; i++) {
		if ((s = st_sh_new(t, i)) == NULL)
			continue;
		shrinker_register(s);
		lkpi_shrink_request(true);
		if (i & 1)
			msleep(1);
		shrinker_free(s);
	}
	errors = st_gang_wait(&t->g);
	CHECK("shrinker register/free under allocation", errors == 0);
	if (errors < 0)
		return;

	/*
	 * The rate limit: a forced pass, then (once it has surely ended) a
	 * request as kmem makes it is dropped, while a forced one runs.
	 */
	if ((t->s = st_sh_new(t, 100)) == NULL) {
		kfree(t);
		return;
	}
	reinit_completion(&t->entered);
	shrinker_register(t->s);
	lkpi_shrink_request(true);
	(void) wait_for_completion_timeout(&t->entered,
	    msecs_to_jiffies(ST_WAIT_MS));
	msleep(100);
	scans = atomic_read(&t->scans);
	lkpi_shrink_request(false);
	msleep(200);
	CHECK("shrink requests from kmem are rate-limited",
	    atomic_read(&t->scans) == scans);
	reinit_completion(&t->entered);
	lkpi_shrink_request(true);
	CHECK("a forced pass runs within the interval",
	    wait_for_completion_timeout(&t->entered,
	    msecs_to_jiffies(ST_WAIT_MS)) > 0);
	shrinker_free(t->s);
	shrinker_free(NULL);
	kfree(t);
}

/* --- kmalloc, krealloc, virt_to_page ------------------------------------ */

#define	ST_V2P_BUFS	16
#define	ST_V2P_ITERS	200

struct st_v2p {
	struct st_gang		g;
	char			*bufs[ST_V2P_BUFS];
	struct lx_page		*seen[2][ST_V2P_BUFS];
};

static int
st_v2p_worker(struct st_worker *w)
{
	struct st_v2p *t = w->g->arg;
	struct lx_page *pg;
	int it, k, errors = 0;
	char *q;

	for (it = 0; it < ST_V2P_ITERS; it++) {
		/* The same addresses from both workers: one descriptor each. */
		for (k = 0; k < ST_V2P_BUFS; k++) {
			pg = virt_to_page(t->bufs[k] + PAGE_SIZE + 8);
			if (it == 0)
				t->seen[w->id][k] = pg;
			else if (pg != t->seen[w->id][k])
				errors++;
		}
		/* Private memory: virt_to_page() then kfree() drops it. */
		q = kmalloc(2 * PAGE_SIZE + it % PAGE_SIZE, GFP_KERNEL);
		if (q == NULL) {
			errors++;
			continue;
		}
		pg = virt_to_page(q + PAGE_SIZE);
		if (pg == NULL || page_address(pg) !=
		    (void *)P2ALIGN((uintptr_t)(q + PAGE_SIZE), PAGE_SIZE))
			errors++;
		kfree(q);
		if ((it & 15) == 0)
			(void) cond_resched();
	}
	return (errors);
}

static void
st_kmalloc(void)
{
	struct st_v2p *t;
	struct lx_page *pg, *pg2;
	char *p, *q, *r;
	int i, errors;
	bool ok;

	/* krealloc() keeps the contents and the recorded size. */
	p = kmalloc(100, GFP_KERNEL);
	if (p == NULL) {
		CHECK("kmalloc", 0);
		return;
	}
	for (i = 0; i < 100; i++)
		p[i] = (char)i;
	q = krealloc(p, 5000, GFP_KERNEL);
	ok = q != NULL && ksize(q) == 5000;
	for (i = 0; ok && i < 100; i++)
		ok = q[i] == (char)i;
	CHECK("krealloc grows and keeps the data", ok);
	if (q == NULL)
		q = p;
	r = krealloc(q, 10, GFP_KERNEL);
	ok = r != NULL && ksize(r) == 10;
	for (i = 0; ok && i < 10; i++)
		ok = r[i] == (char)i;
	CHECK("krealloc shrinks and keeps the data", ok);
	if (r == NULL)
		r = q;
	CHECK("krealloc(p, 0) frees", krealloc(r, 0, GFP_KERNEL) ==
	    ZERO_SIZE_PTR);
	p = krealloc(NULL, 32, GFP_KERNEL);
	CHECK("krealloc(NULL) allocates", p != NULL && ksize(p) == 32);
	kfree(p);

	/* virt_to_page() of kmalloc() memory, and kfree() dropping it. */
	p = kmalloc(3 * PAGE_SIZE, GFP_KERNEL);
	if (p == NULL) {
		CHECK("kmalloc(3 pages)", 0);
		return;
	}
	pg = virt_to_page(p + PAGE_SIZE);
	pg2 = virt_to_page(p + PAGE_SIZE + 100);
	CHECK("virt_to_page of kmalloc memory", pg != NULL && pg == pg2 &&
	    page_address(pg) ==
	    (void *)P2ALIGN((uintptr_t)(p + PAGE_SIZE), PAGE_SIZE));
	CHECK("virt_to_phys of kmalloc memory", pg != NULL &&
	    page_to_phys(pg) != 0 && virt_to_phys(p + PAGE_SIZE) ==
	    page_to_phys(pg) + offset_in_page(p + PAGE_SIZE));
	if (pg != NULL) {
		get_page(pg);
		CHECK("foreign page: the index's and our reference",
		    page_count(pg) == 2);
		kfree(p);
		CHECK("kfree drops the foreign descriptor's index reference",
		    page_count(pg) == 1);
		put_page(pg);
	} else {
		kfree(p);
	}

	/* Two threads, the same addresses, plus their own churn. */
	t = kzalloc(sizeof (*t), GFP_KERNEL);
	if (t == NULL) {
		CHECK("virt_to_page: state", 0);
		return;
	}
	for (i = 0; i < ST_V2P_BUFS; i++) {
		if ((t->bufs[i] = kmalloc(2 * PAGE_SIZE, GFP_KERNEL)) == NULL)
			break;
	}
	if (i == ST_V2P_BUFS) {
		CHECK("virt_to_page: workers started", st_gang_start(&t->g, 2,
		    st_v2p_worker, t));
		errors = st_gang_wait(&t->g);
		ok = errors == 0;
		for (i = 0; ok && i < ST_V2P_BUFS; i++)
			ok = t->seen[0][i] != NULL &&
			    t->seen[0][i] == t->seen[1][i];
		CHECK("virt_to_page race: one descriptor per address", ok);
		if (errors < 0)
			return;
	} else {
		CHECK("virt_to_page: buffers", 0);
	}
	for (i = 0; i < ST_V2P_BUFS; i++)
		kfree(t->bufs[i]);
	kfree(t);
}

void
st_mem_tests(struct pci_dev *pdev)
{
	st_shmem();
	st_vmap();
	st_dma(pdev);
	st_typesafe();
	st_shrinker();
	st_kmalloc();
	pr_info("linuxkpi_selftest: skip pin_user_pages (probe has no user "
	    "address space)\n");
}
