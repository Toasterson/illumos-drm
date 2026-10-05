/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The layer's pages: alloc_pages(), the address index behind
 * virt_to_page(), page_to_phys() and vmap()/vmap_pfn() (the interfaces of
 * Linux's <linux/gfp.h>, <linux/mm.h> and <linux/vmalloc.h>; independent
 * implementation over illumos kmem, vmem and the kernel HAT), and
 * memremap() of <linux/io.h> over vmap_pfn(). See
 * <linux/mm_types.h> for the page model and src/lkpi_mem.h for the owners.
 *
 * alloc_pages() of order 0 takes a page from a page-aligned kmem cache and
 * a descriptor from a second cache; higher orders return NULL (illumos
 * kernel memory is not physically contiguous beyond a page).
 *
 * The address index maps the page-aligned kernel address of every
 * registered descriptor to it, so virt_to_page() of a layer page's
 * address (or of an address inside a vmap() range of layer pages) returns
 * that page. Any other mapped kernel address -- kmalloc() memory handed to
 * dma_map_single() or virt_to_page() -- gets a "foreign" descriptor,
 * created on first use and registered; its lifetime rules are:
 *	- the index holds one reference; get_page()/put_page() by users
 *	  nest inside it (it describes an address, not an allocation:
 *	  page_to_phys() asks the HAT each time, so it stays correct if the
 *	  memory behind the address is freed and reused);
 *	- kfree() and free_pages_exact() drop the index's reference on the
 *	  foreign descriptors of the pages their buffer covers entirely
 *	  (lkpi_page_forget_foreign()), so descriptors made by virt_to_page()
 *	  of kmalloc() memory (i915's GPU error capture, dma_map_single())
 *	  do not pile up. A page the buffer covers only in part may hold
 *	  other live allocations, whose users may keep the descriptor
 *	  without a reference (sg_set_page() does not take one): it stays
 *	  registered. Such pages are bounded by the kernel heap and reused
 *	  by address, one descriptor each;
 *	- a page creator (alloc_pages(), a shmem file, a user pin) that
 *	  registers the same address evicts it: the index drops its
 *	  reference and the descriptor is freed at its last put_page();
 *	- an unbalanced put_page() that drops the last reference unregisters
 *	  and frees it.
 *
 * page_to_phys() is hat_getpfnum() of the page's kernel address; nothing
 * else in the layer uses physical frames of pages (page_to_pfn() is the
 * kernel virtual frame, see <linux/mm.h>).
 *
 * vmap(): pages that are already virtually contiguous (a shmem file's pages
 * from one ddi_umem_alloc(9F) allocation, a user pin's bp_mapin(9F)
 * range) are returned at their own address; their kernel mapping is
 * write-back, and a write-combining or uncached request is not applied
 * there (x86 forbids mapping one frame with two cache types). Other pages
 * get a range of kernel heap (vmem_alloc(heap_arena)) loaded with their
 * frames by hat_devload(HAT_LOAD_LOCK) with the requested caching -- the
 * same scheme as illumos' gfx_private gfxp_map_kernel_space(); a WC or UC
 * mapping of RAM aliases the frame's WB kernel mapping as there.
 * HAT_LOAD_NOCONSIST keeps the HAT from tracking (and asserting on the
 * locking of) the page_t of each frame: the range is unloaded by vunmap()
 * only. Every vmap() is recorded, because vunmap() and vfree() (which
 * Linux allows on a vmap() address) must find it.
 */

#include <vm/seg_kmem.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/bug.h>
#include "lkpi_internal.h"
#include "lkpi_mem.h"

/*
 * From i86pc's <vm/hat_i86.h>, which is not installed: honour the caching
 * attribute for frames of RAM (hat_devload() makes them write-back
 * otherwise). gfx_private uses it the same way.
 */
#ifndef	HAT_PLAT_NOCACHE
#define	HAT_PLAT_NOCACHE	0x100000
#endif

/* A page descriptor; the struct lx_page comes first (page == descriptor). */
struct lkpi_pg {
	struct lx_page	lp_page;
	avl_node_t	lp_node;
	bool		lp_registered;
	struct lkpi_pg	*lp_next;	/* lkpi_page_forget_foreign()'s list */
};

static kmem_cache_t *lkpi_pg_cache;	/* descriptors */
static kmem_cache_t *lkpi_pgmem_cache;	/* alloc_pages() memory */
static krwlock_t lkpi_pg_lock;		/* the address index */
static avl_tree_t lkpi_pg_index;
/* Registered foreign descriptors (lkpi_pg_lock); kfree() skips at zero. */
static volatile unsigned long lkpi_pg_nforeign;

/* One vmap() or vmap_pfn(). */
struct lkpi_vmap {
	avl_node_t	vm_node;
	caddr_t		vm_va;
	uint64_t	vm_seq;		/* orders records of the same address */
	size_t		vm_size;
	bool		vm_loaded;	/* a heap range loaded by this layer */
	unsigned long	vm_flags;
	struct lx_page	**vm_pages;	/* NULL for a contiguous or pfn map */
	unsigned int	vm_count;
};

static kmutex_t lkpi_vmap_lock;
static avl_tree_t lkpi_vmap_index;
static uint64_t lkpi_vmap_seq;
static bool lkpi_vmap_ready;

static void lkpi_alloc_release(struct lkpi_page_owner *, struct lx_page *);
static void lkpi_foreign_release(struct lkpi_page_owner *, struct lx_page *);

static struct lkpi_page_owner lkpi_alloc_owner = {
	.lpo_release = lkpi_alloc_release,
	.lpo_name = "alloc_pages"
};

static struct lkpi_page_owner lkpi_foreign_owner = {
	.lpo_release = lkpi_foreign_release,
	.lpo_name = "foreign"
};

static int
lkpi_pg_cmp(const void *a, const void *b)
{
	uintptr_t x = (uintptr_t)((const struct lkpi_pg *)a)->lp_page.lkpi_kva;
	uintptr_t y = (uintptr_t)((const struct lkpi_pg *)b)->lp_page.lkpi_kva;

	return (x < y ? -1 : (x > y ? 1 : 0));
}

static int
lkpi_vmap_cmp(const void *a, const void *b)
{
	const struct lkpi_vmap *x = a, *y = b;

	if ((uintptr_t)x->vm_va != (uintptr_t)y->vm_va)
		return ((uintptr_t)x->vm_va < (uintptr_t)y->vm_va ? -1 : 1);
	if (x->vm_seq != y->vm_seq)
		return (x->vm_seq < y->vm_seq ? -1 : 1);
	return (0);
}

/* --- descriptors and the address index ------------------------------- */

static struct lkpi_pg *
lkpi_pg_alloc(void *kva, struct lkpi_page_owner *owner, int kmflag)
{
	struct lkpi_pg *pg;

	pg = (kmem_cache_alloc)(lkpi_pg_cache, kmflag);
	if (pg == NULL)
		return (NULL);
	bzero(pg, sizeof (*pg));
	pg->lp_page.lkpi_kva = kva;
	pg->lp_page.lkpi_owner = owner;
	INIT_LIST_HEAD(&pg->lp_page.lru);
	atomic_set(&pg->lp_page._refcount, 1);
	atomic_set(&pg->lp_page._mapcount, -1);
	return (pg);
}

struct lx_page *
lkpi_page_create(void *kva, struct lkpi_page_owner *owner, int kmflag)
{
	struct lkpi_pg *pg, *old;
	struct lx_page *evicted = NULL;
	avl_index_t where;

	if (!IS_P2ALIGNED(kva, PAGE_SIZE)) {
		WARN(1, "lkpi_page_create: unaligned address %p", kva);
		return (NULL);
	}
	if ((pg = lkpi_pg_alloc(kva, owner, kmflag)) == NULL)
		return (NULL);

	rw_enter(&lkpi_pg_lock, RW_WRITER);
	old = avl_find(&lkpi_pg_index, pg, &where);
	if (old != NULL && old->lp_page.lkpi_owner == &lkpi_foreign_owner) {
		avl_remove(&lkpi_pg_index, old);
		old->lp_registered = false;
		lkpi_pg_nforeign--;
		evicted = &old->lp_page;
		old = avl_find(&lkpi_pg_index, pg, &where);
	}
	if (old == NULL) {
		avl_insert(&lkpi_pg_index, pg, where);
		pg->lp_registered = true;
	}
	rw_exit(&lkpi_pg_lock);

	if (old != NULL) {
		WARN(1, "lkpi_page_create: %p is already a %s page", kva,
		    old->lp_page.lkpi_owner->lpo_name);
	}
	/* The index's reference on the foreign descriptor. */
	if (evicted != NULL)
		put_page(evicted);
	return (&pg->lp_page);
}

void
lkpi_page_destroy(struct lx_page *page)
{
	struct lkpi_pg *pg = (struct lkpi_pg *)page;

	rw_enter(&lkpi_pg_lock, RW_WRITER);
	if (pg->lp_registered) {
		avl_remove(&lkpi_pg_index, pg);
		pg->lp_registered = false;
		if (page->lkpi_owner == &lkpi_foreign_owner)
			lkpi_pg_nforeign--;
	}
	rw_exit(&lkpi_pg_lock);
	(kmem_cache_free)(lkpi_pg_cache, pg);
}

void
lkpi_page_forget_foreign(const void *addr, size_t len)
{
	uintptr_t start = P2ROUNDUP((uintptr_t)addr, PAGE_SIZE);
	uintptr_t end = P2ALIGN((uintptr_t)addr + len, PAGE_SIZE);
	struct lkpi_pg key, *pg, *next, *list = NULL;
	avl_index_t where;

	/* Unlocked: a virt_to_page() racing with the free is a bug anyway. */
	if (lkpi_pg_nforeign == 0 || start >= end)
		return;
	key.lp_page.lkpi_kva = (void *)start;
	rw_enter(&lkpi_pg_lock, RW_WRITER);
	if ((pg = avl_find(&lkpi_pg_index, &key, &where)) == NULL)
		pg = avl_nearest(&lkpi_pg_index, where, AVL_AFTER);
	for (; pg != NULL && (uintptr_t)pg->lp_page.lkpi_kva < end; pg = next) {
		next = AVL_NEXT(&lkpi_pg_index, pg);
		if (pg->lp_page.lkpi_owner != &lkpi_foreign_owner)
			continue;
		avl_remove(&lkpi_pg_index, pg);
		pg->lp_registered = false;
		lkpi_pg_nforeign--;
		pg->lp_next = list;
		list = pg;
	}
	rw_exit(&lkpi_pg_lock);
	/* The index's references; put_page() may take lkpi_pg_lock. */
	for (pg = list; pg != NULL; pg = next) {
		next = pg->lp_next;
		put_page(&pg->lp_page);
	}
}

struct lx_page *
lkpi_page_lookup(const void *kva)
{
	struct lkpi_pg key, *pg;

	key.lp_page.lkpi_kva = (void *)P2ALIGN((uintptr_t)kva, PAGE_SIZE);
	rw_enter(&lkpi_pg_lock, RW_READER);
	pg = avl_find(&lkpi_pg_index, &key, NULL);
	rw_exit(&lkpi_pg_lock);
	return (pg != NULL ? &pg->lp_page : NULL);
}

void
lkpi_page_release(struct lx_page *page)
{
	struct lkpi_page_owner *owner;

	if (page == NULL)
		return;
	owner = page->lkpi_owner;
	if (owner == NULL || owner->lpo_release == NULL) {
		WARN_ONCE(1, "put_page: page %p has no owner", page);
		return;
	}
	owner->lpo_release(owner, page);
}

static void
lkpi_alloc_release(struct lkpi_page_owner *owner, struct lx_page *page)
{
	void *kva = page->lkpi_kva;

	lkpi_page_destroy(page);
	(kmem_cache_free)(lkpi_pgmem_cache, kva);
}

static void
lkpi_foreign_release(struct lkpi_page_owner *owner, struct lx_page *page)
{
	lkpi_page_destroy(page);
}

/* The page of a non-contiguous vmap() range that contains pkva. */
static struct lx_page *
lkpi_vmap_page(caddr_t pkva)
{
	struct lkpi_vmap key, *vm;
	struct lx_page *page = NULL;
	avl_index_t where;

	if (!lkpi_vmap_ready)
		return (NULL);
	key.vm_va = pkva;
	key.vm_seq = UINT64_MAX;
	mutex_enter(&lkpi_vmap_lock);
	(void) avl_find(&lkpi_vmap_index, &key, &where);
	vm = avl_nearest(&lkpi_vmap_index, where, AVL_BEFORE);
	/* Contiguous records lie outside loaded ranges; skip them. */
	while (vm != NULL && !vm->vm_loaded)
		vm = AVL_PREV(&lkpi_vmap_index, vm);
	if (vm != NULL && vm->vm_pages != NULL && pkva >= vm->vm_va &&
	    pkva < vm->vm_va + vm->vm_size)
		page = vm->vm_pages[(pkva - vm->vm_va) >> PAGE_SHIFT];
	mutex_exit(&lkpi_vmap_lock);
	return (page);
}

/* A foreign descriptor for a mapped kernel page the layer did not create. */
static struct lx_page *
lkpi_page_foreign(caddr_t pkva)
{
	struct lkpi_pg *pg, *old;
	avl_index_t where;

	if (hat_getpfnum(kas.a_hat, pkva) == PFN_INVALID) {
		WARN_ONCE(1, "virt_to_page: %p is not mapped", (void *)pkva);
		return (NULL);
	}
	/*
	 * virt_to_page() may be called with spinlocks held. The layer's
	 * spinlocks are adaptive kmutexes (<linux/spinlock.h>), so blocking
	 * briefly on lkpi_pg_lock below is allowed; what must not happen is
	 * an unbounded wait for memory under a driver lock, hence KM_NOSLEEP.
	 */
	if ((pg = lkpi_pg_alloc(pkva, &lkpi_foreign_owner, KM_NOSLEEP)) ==
	    NULL) {
		WARN_ONCE(1, "virt_to_page: no memory for a page descriptor");
		return (NULL);
	}
	rw_enter(&lkpi_pg_lock, RW_WRITER);
	old = avl_find(&lkpi_pg_index, pg, &where);
	if (old == NULL) {
		avl_insert(&lkpi_pg_index, pg, where);
		pg->lp_registered = true;
		lkpi_pg_nforeign++;
	}
	rw_exit(&lkpi_pg_lock);
	if (old != NULL) {
		(kmem_cache_free)(lkpi_pg_cache, pg);
		return (&old->lp_page);
	}
	return (&pg->lp_page);
}

struct lx_page *
lkpi_virt_to_page(const void *kva)
{
	caddr_t pkva = (caddr_t)P2ALIGN((uintptr_t)kva, PAGE_SIZE);
	struct lx_page *page;

	if (kva == NULL)
		return (NULL);
	if ((page = lkpi_page_lookup(pkva)) != NULL)
		return (page);
	if ((page = lkpi_vmap_page(pkva)) != NULL)
		return (page);
	return (lkpi_page_foreign(pkva));
}

struct lx_page *
lkpi_nth_page(struct lx_page *page, unsigned long n)
{
	if (page == NULL) {
		WARN_ONCE(1, "nth_page: NULL page");
		return (NULL);
	}
	if (n == 0)
		return (page);
	return (lkpi_virt_to_page((caddr_t)page->lkpi_kva + LKPI_PTOB(n)));
}

u64
lkpi_page_to_phys(struct lx_page *page)
{
	pfn_t pfn;

	if (page == NULL)
		return (0);
	pfn = hat_getpfnum(kas.a_hat, page->lkpi_kva);
	if (pfn == PFN_INVALID) {
		WARN_ONCE(1, "page_to_phys: %p is not mapped", page->lkpi_kva);
		return (0);
	}
	return ((u64)pfn << PAGE_SHIFT);
}

/*
 * totalram_pages(): illumos' physmem, the number of pages of physical
 * memory the kernel manages. There is no DDI equivalent; i915 uses it to
 * size its shrinker targets and the largest object it accepts.
 */
unsigned long
physmem_lkpi(void)
{
	return ((unsigned long)physmem);
}

/* --- the page allocator ------------------------------------------------ */

struct lx_page *
alloc_pages(gfp_t gfp, unsigned int order)
{
	struct lx_page *page;
	void *kva;
	int kmflag = lkpi_kmflag(gfp);

	/* Higher orders are not supported (<linux/gfp.h>). */
	if (order != 0)
		return (NULL);
	if ((kva = (kmem_cache_alloc)(lkpi_pgmem_cache, kmflag)) == NULL)
		return (NULL);
	if (gfp & __GFP_ZERO)
		bzero(kva, PAGE_SIZE);
	if ((page = lkpi_page_create(kva, &lkpi_alloc_owner, kmflag)) ==
	    NULL) {
		(kmem_cache_free)(lkpi_pgmem_cache, kva);
		return (NULL);
	}
	return (page);
}

void
__free_pages(struct lx_page *page, unsigned int order)
{
	if (page != NULL)
		put_page(page);
}

unsigned long
__get_free_pages(gfp_t gfp, unsigned int order)
{
	struct lx_page *page = alloc_pages(gfp, order);

	return (page != NULL ? (unsigned long)page->lkpi_kva : 0);
}

unsigned long
get_zeroed_page(gfp_t gfp)
{
	return (__get_free_pages(gfp | __GFP_ZERO, 0));
}

void
free_pages(unsigned long addr, unsigned int order)
{
	struct lx_page *page;

	if (addr == 0)
		return;
	page = lkpi_page_lookup((void *)addr);
	if (page == NULL || page->lkpi_owner != &lkpi_alloc_owner) {
		WARN(1, "free_pages: %lx is not from __get_free_pages()", addr);
		return;
	}
	put_page(page);
}

/*
 * kmem_alloc(9F) of a multiple of the page size is page-aligned (those
 * sizes come from page-aligned caches or straight from the kernel heap);
 * the memory is virtually, not physically, contiguous.
 */
void *
alloc_pages_exact(size_t size, gfp_t gfp)
{
	size_t len = P2ROUNDUP(size, PAGE_SIZE);
	void *p;

	if (size == 0 || len < size)
		return (NULL);
	p = (gfp & __GFP_ZERO) ? kmem_zalloc(len, lkpi_kmflag(gfp)) :
	    kmem_alloc(len, lkpi_kmflag(gfp));
	if (p != NULL && !IS_P2ALIGNED(p, PAGE_SIZE)) {
		WARN_ONCE(1, "alloc_pages_exact: kmem returned %p", p);
		kmem_free(p, len);
		return (NULL);
	}
	return (p);
}

void
free_pages_exact(void *virt, size_t size)
{
	if (virt == NULL)
		return;
	lkpi_page_forget_foreign(virt, P2ROUNDUP(size, PAGE_SIZE));
	kmem_free(virt, P2ROUNDUP(size, PAGE_SIZE));
}

/* --- vmap -------------------------------------------------------------- */

static uint_t
lkpi_hat_attr(pgprot_t prot)
{
	switch (pgprot_val(prot) & LKPI_PROT_CACHE_MASK) {
	case LKPI_PROT_WC:
		return (PROT_READ | PROT_WRITE | HAT_MERGING_OK |
		    HAT_PLAT_NOCACHE);
	case LKPI_PROT_UC:
		return (PROT_READ | PROT_WRITE | HAT_STRICTORDER |
		    HAT_PLAT_NOCACHE);
	default:
		return (PROT_READ | PROT_WRITE | HAT_STORECACHING_OK);
	}
}

static void
lkpi_vmap_insert(struct lkpi_vmap *vm)
{
	mutex_enter(&lkpi_vmap_lock);
	vm->vm_seq = ++lkpi_vmap_seq;
	avl_add(&lkpi_vmap_index, vm);
	mutex_exit(&lkpi_vmap_lock);
}

/* Loads pfns[0..count) at va; unloads and fails on an invalid frame. */
static bool
lkpi_vmap_load(caddr_t va, const pfn_t *pfns, struct lx_page **pages,
    unsigned int count, pgprot_t prot)
{
	uint_t attr = lkpi_hat_attr(prot);
	unsigned int i;
	pfn_t pfn;

	for (i = 0; i < count; i++) {
		pfn = pfns != NULL ? pfns[i] :
		    hat_getpfnum(kas.a_hat, pages[i]->lkpi_kva);
		if (pfn == PFN_INVALID) {
			if (i > 0)
				hat_unload(kas.a_hat, va, LKPI_PTOB(i),
				    HAT_UNLOAD_UNLOCK);
			return (false);
		}
		hat_devload(kas.a_hat, va + LKPI_PTOB(i), PAGE_SIZE, pfn, attr,
		    HAT_LOAD_LOCK | HAT_LOAD_NOCONSIST);
	}
	return (true);
}

void *
vmap(struct lx_page **pages, unsigned int count, unsigned long flags,
    pgprot_t prot)
{
	struct lkpi_vmap *vm;
	size_t size = LKPI_PTOB((size_t)count);
	caddr_t base, va;
	bool contig = true;
	unsigned int i;

	if (pages == NULL || count == 0)
		return (NULL);
	for (i = 0; i < count; i++) {
		if (pages[i] == NULL) {
			WARN(1, "vmap: page %u is NULL", i);
			return (NULL);
		}
		if ((caddr_t)pages[i]->lkpi_kva !=
		    (caddr_t)pages[0]->lkpi_kva + LKPI_PTOB(i))
			contig = false;
	}
	base = pages[0]->lkpi_kva;
	/*
	 * Contiguous pages are already mapped write-back at their kernel
	 * address; a WC or UC request needs a mapping of its own (an alias
	 * with that caching, as Linux's vmap() always makes one).
	 */
	if ((pgprot_val(prot) & LKPI_PROT_CACHE_MASK) != LKPI_PROT_WB)
		contig = false;

	vm = kmem_zalloc(sizeof (*vm), KM_SLEEP);
	vm->vm_size = size;
	vm->vm_flags = flags;
	vm->vm_count = count;
	if (contig) {
		va = base;
		if (flags & VM_MAP_PUT_PAGES)
			vm->vm_pages = pages;
	} else {
		/* The heap is a VA arena: VM_NOSLEEP fails only when it is full. */
		va = vmem_alloc(heap_arena, size, VM_NOSLEEP);
		if (va == NULL)
			goto fail;
		if (!lkpi_vmap_load(va, NULL, pages, count, prot)) {
			WARN(1, "vmap: a page is not mapped");
			vmem_free(heap_arena, va, size);
			goto fail;
		}
		vm->vm_loaded = true;
		/* virt_to_page() inside the range needs the pages. */
		if (flags & VM_MAP_PUT_PAGES) {
			vm->vm_pages = pages;
		} else {
			vm->vm_pages = kmem_alloc(count * sizeof (*pages),
			    KM_SLEEP);
			bcopy(pages, vm->vm_pages, count * sizeof (*pages));
		}
	}
	vm->vm_va = va;
	lkpi_vmap_insert(vm);
	return (va);
fail:
	kmem_free(vm, sizeof (*vm));
	return (NULL);
}

void *
vmap_pfn(unsigned long *pfns, unsigned int count, pgprot_t prot)
{
	struct lkpi_vmap *vm;
	size_t size = LKPI_PTOB((size_t)count);
	pfn_t *frames;
	caddr_t va;
	unsigned int i;
	bool ok;

	if (pfns == NULL || count == 0)
		return (NULL);
	if ((va = vmem_alloc(heap_arena, size, VM_NOSLEEP)) == NULL)
		return (NULL);
	frames = kmem_alloc(count * sizeof (pfn_t), KM_SLEEP);
	for (i = 0; i < count; i++)
		frames[i] = (pfn_t)pfns[i];
	ok = lkpi_vmap_load(va, frames, NULL, count, prot);
	kmem_free(frames, count * sizeof (pfn_t));
	if (!ok) {
		vmem_free(heap_arena, va, size);
		return (NULL);
	}
	vm = kmem_zalloc(sizeof (*vm), KM_SLEEP);
	vm->vm_va = va;
	vm->vm_size = size;
	vm->vm_count = count;
	vm->vm_loaded = true;
	lkpi_vmap_insert(vm);
	return (va);
}

/*
 * memremap(): physical memory outside the driver's BARs, such as the ACPI
 * OpRegion and the VBT behind it (i915's display), mapped write-back with
 * vmap_pfn() into a kernel heap range. Linux's memremap(MEMREMAP_WB)
 * returns the direct-map address for System RAM and an ioremap_cache()
 * mapping otherwise; here both are a mapping of their own, write-back in
 * both cases: frames outside RAM (ACPI NVS and reserved ranges, where
 * firmware tables live) are loaded without a page_t, frames of RAM (a
 * firmware address pointing into usable memory) get a write-back alias
 * of the kernel's own write-back mapping, which only reads harmlessly
 * (a caller writes only to memory it owns). Only MEMREMAP_WB is
 * supported: a WC or WT mapping of RAM would give a frame two cache
 * types. Refused (NULL): other types, a zero size, a wrapping range,
 * more than 1 GiB, and frames above the CPU's physical address width,
 * whose page-table entries would fault (reserved bits) at the first
 * access.
 */
#define	LKPI_MEMREMAP_MAX	(1UL << 30)

void *
memremap(resource_size_t offset, size_t size, unsigned long flags)
{
	unsigned long *pfns, first, n, i;
	unsigned int bits = lkpi_phys_addr_bits();
	uint64_t end;
	caddr_t va;

	if ((flags & MEMREMAP_WB) == 0 || size == 0 ||
	    size > LKPI_MEMREMAP_MAX ||
	    (uint64_t)offset + size < (uint64_t)offset)
		return (NULL);
	end = (uint64_t)offset + size - 1;
	if (bits < 64 && (end >> bits) != 0)
		return (NULL);
	first = (unsigned long)(offset >> PAGE_SHIFT);
	n = (unsigned long)(end >> PAGE_SHIFT) - first + 1;
	pfns = kmem_alloc(n * sizeof (*pfns), KM_SLEEP);
	for (i = 0; i < n; i++)
		pfns[i] = first + i;
	va = vmap_pfn(pfns, (unsigned int)n, PAGE_KERNEL);
	kmem_free(pfns, n * sizeof (*pfns));
	if (va == NULL)
		return (NULL);
	return (va + (offset & (PAGE_SIZE - 1)));
}

/* Takes memremap()'s address (inside the first page of the mapping). */
void
memunmap(void *addr)
{
	if (addr == NULL)
		return;
	vunmap((const void *)((uintptr_t)addr & ~(uintptr_t)(PAGE_SIZE - 1)));
}

/* Undoes the newest vmap() at addr; false if there is none. */
static bool
lkpi_vmap_remove(const void *addr)
{
	struct lkpi_vmap key, *vm;
	avl_index_t where;
	unsigned int i;

	if (!lkpi_vmap_ready || addr == NULL)
		return (false);
	key.vm_va = (caddr_t)addr;
	key.vm_seq = UINT64_MAX;
	mutex_enter(&lkpi_vmap_lock);
	(void) avl_find(&lkpi_vmap_index, &key, &where);
	vm = avl_nearest(&lkpi_vmap_index, where, AVL_BEFORE);
	if (vm == NULL || vm->vm_va != (caddr_t)addr) {
		mutex_exit(&lkpi_vmap_lock);
		return (false);
	}
	avl_remove(&lkpi_vmap_index, vm);
	mutex_exit(&lkpi_vmap_lock);

	if (vm->vm_loaded) {
		hat_unload(kas.a_hat, vm->vm_va, vm->vm_size,
		    HAT_UNLOAD_UNLOCK);
		vmem_free(heap_arena, vm->vm_va, vm->vm_size);
	}
	if (vm->vm_flags & VM_MAP_PUT_PAGES) {
		for (i = 0; i < vm->vm_count; i++)
			put_page(vm->vm_pages[i]);
		kvfree(vm->vm_pages);
	} else if (vm->vm_pages != NULL) {
		kmem_free(vm->vm_pages, vm->vm_count * sizeof (*vm->vm_pages));
	}
	kmem_free(vm, sizeof (*vm));
	return (true);
}

/*
 * Whether addr is inside a live vmap()/vmap_pfn() mapping: one that starts
 * there, or the nearest one below it that covers it (is_vmalloc_addr()).
 */
bool
lkpi_is_vmap_addr(const void *addr)
{
	struct lkpi_vmap key, *vm;
	avl_index_t where;
	bool r;

	if (!lkpi_vmap_ready || addr == NULL)
		return (false);
	key.vm_va = (caddr_t)addr;
	key.vm_seq = UINT64_MAX;
	mutex_enter(&lkpi_vmap_lock);
	(void) avl_find(&lkpi_vmap_index, &key, &where);
	vm = avl_nearest(&lkpi_vmap_index, where, AVL_BEFORE);
	r = vm != NULL && (caddr_t)addr < vm->vm_va + vm->vm_size;
	mutex_exit(&lkpi_vmap_lock);
	return (r);
}

void
vunmap(const void *addr)
{
	if (addr != NULL && !lkpi_vmap_remove(addr))
		WARN(1, "vunmap: %p is not a vmap() address", addr);
}

bool
lkpi_vmap_free(const void *addr)
{
	return (lkpi_vmap_remove(addr));
}

struct lx_page *
vmalloc_to_page(const void *addr)
{
	return (lkpi_virt_to_page(addr));
}

/* --- module state ------------------------------------------------------ */

int
lkpi_page_init(void)
{
	lkpi_pg_cache = (kmem_cache_create)("lkpi_page",
	    sizeof (struct lkpi_pg), 0, NULL, NULL, NULL, NULL, NULL, 0);
	lkpi_pgmem_cache = (kmem_cache_create)("lkpi_page_mem", PAGE_SIZE,
	    PAGE_SIZE, NULL, NULL, NULL, NULL, NULL, 0);
	rw_init(&lkpi_pg_lock, NULL, RW_DRIVER, NULL);
	avl_create(&lkpi_pg_index, lkpi_pg_cmp, sizeof (struct lkpi_pg),
	    offsetof(struct lkpi_pg, lp_node));
	(mutex_init)(&lkpi_vmap_lock, NULL, MUTEX_DRIVER, NULL);
	avl_create(&lkpi_vmap_index, lkpi_vmap_cmp, sizeof (struct lkpi_vmap),
	    offsetof(struct lkpi_vmap, vm_node));
	lkpi_vmap_ready = true;
	lkpi_dma_init();
	lkpi_io_init();
	return (0);
}

void
lkpi_page_fini(void)
{
	struct lkpi_vmap *vm;
	struct lkpi_pg *pg;
	unsigned long leaked = 0;

	lkpi_io_fini();
	lkpi_dma_fini();

	/* Undo what the driver left mapped (it has detached). */
	while ((vm = avl_first(&lkpi_vmap_index)) != NULL) {
		leaked++;
		(void) lkpi_vmap_remove(vm->vm_va);
	}
	lkpi_vmap_ready = false;
	avl_destroy(&lkpi_vmap_index);
	(mutex_destroy)(&lkpi_vmap_lock);
	if (leaked != 0)
		cmn_err(CE_WARN, "linuxkpi: %lu vmap() ranges left", leaked);

	/*
	 * Foreign descriptors are the index's; anything else left here was
	 * leaked by the driver: free alloc_pages() memory, drop the rest.
	 */
	leaked = 0;
	while ((pg = avl_first(&lkpi_pg_index)) != NULL) {
		avl_remove(&lkpi_pg_index, pg);
		pg->lp_registered = false;
		if (pg->lp_page.lkpi_owner == &lkpi_alloc_owner) {
			(kmem_cache_free)(lkpi_pgmem_cache,
			    pg->lp_page.lkpi_kva);
		}
		if (pg->lp_page.lkpi_owner != &lkpi_foreign_owner)
			leaked++;
		(kmem_cache_free)(lkpi_pg_cache, pg);
	}
	if (leaked != 0)
		cmn_err(CE_WARN, "linuxkpi: %lu pages left", leaked);
	lkpi_pg_nforeign = 0;
	avl_destroy(&lkpi_pg_index);
	rw_destroy(&lkpi_pg_lock);
	(kmem_cache_destroy)(lkpi_pgmem_cache);
	(kmem_cache_destroy)(lkpi_pg_cache);
}
