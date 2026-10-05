/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux memory-management entry points other than allocation (the
 * interface of Linux's <linux/mm.h>; independent implementation): dirty
 * flags, pinning of user memory, and the Linux user-mapping paths, which
 * the layer does not support (see <linux/mm.h>).
 *
 * pin_user_pages_fast() locks the user range with ddi_umem_lock(9F) (the
 * calling process's address space, as Linux's current->mm), describes it
 * with ddi_umem_iosetup(9F) and maps it into the kernel with bp_mapin(9F).
 * Each pinned page gets a descriptor at its address in that kernel
 * mapping (so the pages of a pin are virtually contiguous, which DMA
 * mapping and vmap() use); each descriptor holds a reference on the pin
 * record. unpin_user_pages() (or put_page() for get_user_pages_fast())
 * drops the pages; with the last one the range is unmapped (bp_mapout()),
 * the buf freed (freerbuf(9F)) and the memory unlocked
 * (ddi_umem_unlock(9F)). A pin is all or nothing: it returns nr_pages or
 * a negative errno, never a partial count.
 *
 * User mappings belong to the DRM core (devmap(9E)): Linux's paths to
 * create or zap them fail or do nothing. unmap_mapping_range() and
 * zap_vma_ptes() say so once, because a driver that relies on them to
 * revoke a mapping does not get that here.
 */

#include <sys/buf.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/bug.h>
#include "lkpi_internal.h"
#include "lkpi_mem.h"

bool
set_page_dirty(struct lx_page *page)
{
	return (!test_and_set_bit(LKPI_PG_dirty, &page->flags));
}

bool
folio_mark_dirty(struct folio *folio)
{
	return (set_page_dirty(&folio->page));
}

/* --- pinned user memory ------------------------------------------------ */

struct lkpi_upin {
	struct lkpi_page_owner	up_owner;	/* first: owner of the pages */
	ddi_umem_cookie_t	up_cookie;
	struct buf		*up_bp;
	volatile uint32_t	up_refs;	/* live page descriptors */
};

static void
lkpi_upin_release(struct lkpi_page_owner *owner, struct lx_page *page)
{
	struct lkpi_upin *up = (struct lkpi_upin *)owner;

	lkpi_page_destroy(page);
	if (__atomic_sub_fetch(&up->up_refs, 1, __ATOMIC_SEQ_CST) != 0)
		return;
	bp_mapout(up->up_bp);
	freerbuf(up->up_bp);
	ddi_umem_unlock(up->up_cookie);
	kmem_free(up, sizeof (*up));
}

static long
lkpi_pin_user(unsigned long start, int nr_pages, unsigned int gup_flags,
    struct lx_page **pages)
{
	struct lkpi_upin *up;
	caddr_t addr = (caddr_t)P2ALIGN(start, PAGE_SIZE);
	size_t len;
	caddr_t kva;
	int flags, err, i;
	bool write = (gup_flags & FOLL_WRITE) != 0;

	if (nr_pages <= 0)
		return (0);
	if (pages == NULL)
		return (-EINVAL);
	len = LKPI_PTOB((size_t)nr_pages);

	flags = DDI_UMEMLOCK_READ | (write ? DDI_UMEMLOCK_WRITE : 0);
	up = kmem_zalloc(sizeof (*up), KM_SLEEP);
	if ((err = ddi_umem_lock(addr, len, flags, &up->up_cookie)) != 0) {
		kmem_free(up, sizeof (*up));
		return (err == ENOMEM || err == EAGAIN ? -ENOMEM : -EFAULT);
	}
	/* B_READ: the device may write the memory. */
	up->up_bp = ddi_umem_iosetup(up->up_cookie, 0, len,
	    write ? B_READ : B_WRITE, NODEV, 0, NULL, DDI_UMEM_SLEEP);
	if (up->up_bp == NULL) {
		ddi_umem_unlock(up->up_cookie);
		kmem_free(up, sizeof (*up));
		return (-ENOMEM);
	}
	bp_mapin(up->up_bp);
	kva = up->up_bp->b_un.b_addr;

	up->up_owner.lpo_release = lkpi_upin_release;
	up->up_owner.lpo_name = "user";
	up->up_refs = (uint32_t)nr_pages;
	for (i = 0; i < nr_pages; i++)
		pages[i] = lkpi_page_create(kva + LKPI_PTOB(i), &up->up_owner,
		    KM_SLEEP);
	return (nr_pages);
}

long
pin_user_pages_fast(unsigned long start, int nr_pages, unsigned int gup_flags,
    struct lx_page **pages)
{
	return (lkpi_pin_user(start, nr_pages, gup_flags, pages));
}

/* The pages come with a reference each, released with put_page(). */
long
get_user_pages_fast(unsigned long start, int nr_pages, unsigned int gup_flags,
    struct lx_page **pages)
{
	return (lkpi_pin_user(start, nr_pages, gup_flags, pages));
}

void
unpin_user_pages(struct lx_page **pages, unsigned long npages)
{
	unsigned long i;

	for (i = 0; i < npages; i++) {
		if (pages[i] != NULL)
			put_page(pages[i]);
	}
}

void
unpin_user_pages_dirty_lock(struct lx_page **pages, unsigned long npages,
    bool make_dirty)
{
	unsigned long i;

	if (make_dirty) {
		for (i = 0; i < npages; i++) {
			if (pages[i] != NULL)
				(void) set_page_dirty(pages[i]);
		}
	}
	unpin_user_pages(pages, npages);
}

/* --- Linux user mappings: not supported ------------------------------- */

void
unmap_mapping_range(struct address_space *mapping, loff_t const holebegin,
    loff_t const holelen, int even_cows)
{
	WARN_ONCE(1, "unmap_mapping_range: user mappings are the DRM core's; "
	    "not revoked");
}

void
zap_vma_ptes(struct vm_area_struct *vma, unsigned long address,
    unsigned long size)
{
	WARN_ONCE(1, "zap_vma_ptes: user mappings are the DRM core's; "
	    "not revoked");
}

int
remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
    unsigned long pfn, unsigned long size, pgprot_t prot)
{
	return (-ENODEV);
}

vm_fault_t
vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr,
    unsigned long pfn)
{
	return (VM_FAULT_SIGBUS);
}

vm_fault_t
vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr,
    unsigned long pfn, pgprot_t pgprot)
{
	return (VM_FAULT_SIGBUS);
}

vm_fault_t
vmf_insert_mixed(struct vm_area_struct *vma, unsigned long addr,
    unsigned long pfn)
{
	return (VM_FAULT_SIGBUS);
}

int
apply_to_page_range(struct mm_struct *mm, unsigned long addr,
    unsigned long size, int (*fn)(pte_t *, unsigned long, void *),
    void *data)
{
	return (-ENODEV);
}

struct vm_area_struct *
find_vma(struct mm_struct *mm, unsigned long addr)
{
	return (NULL);
}

/* An error is returned as a negative errno in the unsigned address. */
unsigned long
vm_mmap(struct lx_file *file, unsigned long addr, unsigned long len,
    unsigned long prot, unsigned long flag, unsigned long offset)
{
	return ((unsigned long)-ENODEV);
}

int
vfs_mmap(struct lx_file *file, struct vm_area_struct *vma)
{
	return (-ENODEV);
}
