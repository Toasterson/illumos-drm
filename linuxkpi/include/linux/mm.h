/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's memory-management interface, reduced (include/linux/mm.h,
 * GPL-2.0; reimplemented). Page helpers work on the layer's page model
 * (<linux/mm_types.h>). Linux user-space mapping (mmap of a struct file,
 * vm_area_struct faults, remap_pfn_range, vmf_insert_pfn, zap_vma_ptes)
 * is not supported: illumos maps device memory with devmap(9E), which the
 * DRM core owns; these functions fail (VM_FAULT_SIGBUS, -ENODEV) and
 * unmap_mapping_range() does nothing.
 */
#ifndef _LINUX_MM_H
#define	_LINUX_MM_H

#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/mm_types.h>
#include <linux/mmzone.h>
#include <linux/pfn.h>
#include <linux/err.h>
#include <linux/bug.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/page-flags.h>
#include <linux/page_ref.h>
#include <linux/pgtable.h>
#include <linux/shrinker.h>
#include <linux/uaccess.h>

static inline int
get_order(unsigned long size)
{
	if (size <= PAGE_SIZE)
		return (0);
	return ((int)(64 - __builtin_clzl((size - 1) >> PAGE_SHIFT)));
}

#define	PAGE_ALIGN(addr)	ALIGN((addr), PAGE_SIZE)
#define	PAGE_ALIGN_DOWN(addr)	ALIGN_DOWN((addr), PAGE_SIZE)
#define	PAGE_ALIGNED(addr)	IS_ALIGNED((unsigned long)(addr), PAGE_SIZE)
#define	offset_in_page(p)	((unsigned long)(p) & ~PAGE_MASK)
#define	offset_in_folio(f, p)	offset_in_page(p)
#define	page_to_virt(page)	((page)->lkpi_kva)
#define	page_address(page)	((page)->lkpi_kva)
#define	lowmem_page_address(page)	page_address(page)
#define	folio_address(f)	((f)->lkpi_kva)
#define	page_to_pfn(page)	((unsigned long)(page)->lkpi_kva >> PAGE_SHIFT)
#define	folio_pfn(f)		page_to_pfn(&(f)->page)
#define	page_to_phys(page)	lkpi_page_to_phys(page)
#define	page_folio(p)		((struct folio *)(p))
#define	folio_page(f, n)	(&(f)->page + (n))
#define	nth_page(page, n)	lkpi_nth_page((page), (n))
#define	folio_nr_pages(f)	1L
#define	folio_size(f)		PAGE_SIZE
#define	folio_shift(f)		PAGE_SHIFT
#define	folio_order(f)		0
#define	folio_test_large(f)	false
#define	compound_order(p)	0
#define	compound_nr(p)		1
#define	page_size(p)		PAGE_SIZE
#define	totalram_pages()	((unsigned long)physmem_lkpi())
#define	totalhigh_pages()	0UL
#define	nr_free_pages()		0UL
#define	is_vmalloc_or_module_addr(p)	false
#define	virt_addr_valid(p)	true
#define	FOLL_WRITE		0x01
#define	FOLL_FORCE		0x10
#define	FOLL_LONGTERM		0x100
#define	FOLL_PIN		0x40000

#define	VM_NONE		0x00000000
#define	VM_READ		0x00000001
#define	VM_WRITE	0x00000002
#define	VM_EXEC		0x00000004
#define	VM_SHARED	0x00000008
#define	VM_MAYREAD	0x00000010
#define	VM_MAYWRITE	0x00000020
#define	VM_MAYEXEC	0x00000040
#define	VM_MAYSHARE	0x00000080
#define	VM_PFNMAP	0x00000400
#define	VM_IO		0x00004000
#define	VM_DONTCOPY	0x00020000
#define	VM_DONTEXPAND	0x00040000
#define	VM_ACCOUNT	0x00100000
#define	VM_NORESERVE	0x00200000
#define	VM_DONTDUMP	0x04000000
#define	VM_MIXEDMAP	0x10000000

extern unsigned long physmem_lkpi(void);
extern u64 lkpi_page_to_phys(struct lx_page *page);
extern struct lx_page *lkpi_nth_page(struct lx_page *page, unsigned long n);
extern struct lx_page *lkpi_virt_to_page(const void *kva);
extern void lkpi_page_release(struct lx_page *page);

#define	virt_to_page(kva)	lkpi_virt_to_page(kva)
#define	pfn_to_page(pfn)	lkpi_virt_to_page((void *)((unsigned long)(pfn) << PAGE_SHIFT))
#define	pfn_valid(pfn)		true
#define	virt_to_folio(kva)	page_folio(lkpi_virt_to_page(kva))
#define	virt_to_phys(kva)	lkpi_page_to_phys(lkpi_virt_to_page(kva))
#define	page_mapping(p)		((p)->mapping)
#define	folio_mapping(f)	((f)->mapping)
#define	page_index(p)		((p)->index)
#define	folio_index(f)		((f)->index)
#define	folio_pos(f)		((loff_t)(f)->index << PAGE_SHIFT)

static inline void
get_page(struct lx_page *page)
{
	atomic_inc(&page->_refcount);
}

static inline void
put_page(struct lx_page *page)
{
	if (atomic_dec_and_test(&page->_refcount))
		lkpi_page_release(page);
}

#define	folio_get(f)		get_page(&(f)->page)
#define	folio_put(f)		put_page(&(f)->page)
#define	folio_try_get(f)	atomic_inc_not_zero(&(f)->page._refcount)
#define	get_page_unless_zero(p)	atomic_inc_not_zero(&(p)->_refcount)
#define	page_count(p)		atomic_read(&(p)->_refcount)
#define	folio_ref_count(f)	atomic_read(&(f)->page._refcount)
#define	set_page_count(p, v)	atomic_set(&(p)->_refcount, (v))
#define	init_page_count(p)	atomic_set(&(p)->_refcount, 1)

extern bool set_page_dirty(struct lx_page *page);
extern bool folio_mark_dirty(struct folio *folio);
#define	set_page_dirty_lock(p)	set_page_dirty(p)
#define	folio_mark_dirty_lock(f)	folio_mark_dirty(f)
#define	mark_page_accessed(p)	((void)(p))
#define	folio_mark_accessed(f)	((void)(f))

/* Linux mmap paths: not supported (see the header comment). */
extern void unmap_mapping_range(struct address_space *mapping,
    loff_t const holebegin, loff_t const holelen, int even_cows);
extern int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
    unsigned long pfn, unsigned long size, pgprot_t prot);
extern vm_fault_t vmf_insert_pfn(struct vm_area_struct *vma,
    unsigned long addr, unsigned long pfn);
extern vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma,
    unsigned long addr, unsigned long pfn, pgprot_t pgprot);
extern vm_fault_t vmf_insert_mixed(struct vm_area_struct *vma,
    unsigned long addr, unsigned long pfn);
#define	zap_special_vma_range(vma, a, s)	zap_vma_ptes(vma, a, s)
extern void zap_vma_ptes(struct vm_area_struct *vma, unsigned long address,
    unsigned long size);
extern int apply_to_page_range(struct mm_struct *mm, unsigned long addr,
    unsigned long size, int (*fn)(pte_t *, unsigned long, void *),
    void *data);
extern struct vm_area_struct *find_vma(struct mm_struct *mm,
    unsigned long addr);

/* VMA iteration over Linux address spaces: there are none (mm is NULL). */
struct vma_iterator {
	struct mm_struct *mm;
	unsigned long addr;
};
#define	VMA_ITERATOR(name, mm_, addr_) \
	struct vma_iterator name = { .mm = (mm_), .addr = (addr_) }
#define	for_each_vma_range(vmi, vma, end) \
	for ((vma) = NULL; (vma) != NULL; )
#define	for_each_vma(vmi, vma)	for ((vma) = NULL; (vma) != NULL; )
extern unsigned long vm_mmap(struct lx_file *file, unsigned long addr,
    unsigned long len, unsigned long prot, unsigned long flag,
    unsigned long offset);
extern long pin_user_pages_fast(unsigned long start, int nr_pages,
    unsigned int gup_flags, struct lx_page **pages);
extern void unpin_user_pages(struct lx_page **pages, unsigned long npages);
extern void unpin_user_pages_dirty_lock(struct lx_page **pages,
    unsigned long npages, bool make_dirty);
extern long get_user_pages_fast(unsigned long start, int nr_pages,
    unsigned int gup_flags, struct lx_page **pages);
#define	unpin_user_page(p)	unpin_user_pages(&(p), 1)
#define	vma_pages(vma)		(((vma)->vm_end - (vma)->vm_start) >> PAGE_SHIFT)
#define	vm_flags_set(vma, f)	((vma)->vm_flags |= (f))
#define	vm_flags_clear(vma, f)	((vma)->vm_flags &= ~(f))
#define	vma_set_file(vma, f)	((vma)->vm_file = (f))
#define	vm_get_page_prot(f)	PAGE_SHARED
#define	mmap_read_lock(mm)	down_read(&(mm)->mmap_lock)
#define	mmap_read_unlock(mm)	up_read(&(mm)->mmap_lock)
#define	mmap_write_lock(mm)	down_write(&(mm)->mmap_lock)
#define	mmap_write_unlock(mm)	up_write(&(mm)->mmap_lock)
#define	mmap_read_lock_killable(mm)	({ mmap_read_lock(mm); 0; })
#define	mmap_write_lock_killable(mm)	({ mmap_write_lock(mm); 0; })
#define	mmap_assert_locked(mm)	((void)(mm))
#define	is_cow_mapping(f)	(((f) & (VM_SHARED | VM_MAYWRITE)) == VM_MAYWRITE)
#define	si_mem_available()	((long)physmem_lkpi())
#define	want_init_on_free()	false

#include <linux/page_ref.h>

#endif
