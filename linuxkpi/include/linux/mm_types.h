/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The layer's page model (Linux's include/linux/mm_types.h, GPL-2.0, is
 * the interface reference; this is an independent design).
 *
 * A struct page (struct lx_page: the prologue renames the tag, illumos
 * has its own page_t) describes one PAGE_SIZE page of kernel memory by
 * its kernel virtual address. It is not the kernel's page frame: pages
 * come from alloc_pages() (a page-aligned kmem cache), from shmem files
 * (an illumos ddi_umem_alloc(9F) allocation per file, R1's backend for
 * GEM objects) or from pinned user memory (ddi_umem_lock(9F) plus
 * bp_mapin(9F)). Every page is physically contiguous within itself, so
 * DMA mapping gives each page one bus address. A folio is a single page
 * (folio_nr_pages() == 1). page_to_pfn() returns the kernel virtual frame
 * number, which is unique and orders pages by address but is not a
 * physical frame number; nothing in the layer maps by pfn except
 * vmap_pfn(), which takes real frames.
 */
#ifndef _LINUX_MM_TYPES_H
#define	_LINUX_MM_TYPES_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/mm_types_task.h>
#include <asm/pgtable_types.h>

struct address_space;
struct lkpi_page_owner;

#define	__LKPI_PAGE_FIELDS \
	unsigned long flags; \
	struct list_head lru; \
	struct address_space *mapping; \
	pgoff_t index; \
	unsigned long private; \
	atomic_t _refcount; \
	atomic_t _mapcount; \
	void *lkpi_kva; \
	struct lkpi_page_owner *lkpi_owner

struct lx_page {
	__LKPI_PAGE_FIELDS;
};

struct folio {
	union {
		struct {
			__LKPI_PAGE_FIELDS;
		};
		struct lx_page page;
	};
};

typedef unsigned int vm_flags_t;

/* Linux 7's opaque VMA flag set (only shmem_file_setup() takes one here). */
typedef struct {
	unsigned long __vma_flags[1];
} vma_flags_t;
#define	EMPTY_VMA_FLAGS		((vma_flags_t){ })
enum {
	VMA_READ_BIT = 0,
	VMA_WRITE_BIT = 1,
	VMA_SHARED_BIT = 3,
	VMA_NORESERVE_BIT = 21
};
#define	__lkpi_vma_bits(...)	({ unsigned int __b[] = { __VA_ARGS__ }; \
	vma_flags_t __f = EMPTY_VMA_FLAGS; size_t __i; \
	for (__i = 0; __i < sizeof (__b) / sizeof (__b[0]); __i++) \
		__f.__vma_flags[0] |= 1UL << __b[__i]; \
	__f; })
#define	mk_vma_flags(...)	__lkpi_vma_bits(__VA_ARGS__)

struct vm_area_struct;
struct vm_operations_struct;

struct mm_struct {
	struct rw_semaphore mmap_lock;
	atomic_t mm_users;
	atomic_t mm_count;
};

struct vm_area_struct {
	unsigned long vm_start;
	unsigned long vm_end;
	struct mm_struct *vm_mm;
	pgprot_t vm_page_prot;
	vm_flags_t vm_flags;
	unsigned long vm_pgoff;
	struct lx_file *vm_file;
	void *vm_private_data;
	const struct vm_operations_struct *vm_ops;
};

struct vm_fault {
	struct vm_area_struct *vma;
	unsigned int flags;
	gfp_t gfp_mask;
	pgoff_t pgoff;
	unsigned long address;
	unsigned long real_address;
	struct lx_page *page;
};

#define	VM_FAULT_OOM		0x000001
#define	VM_FAULT_SIGBUS		0x000002
#define	VM_FAULT_MAJOR		0x000004
#define	VM_FAULT_HWPOISON	0x000010
#define	VM_FAULT_SIGSEGV	0x000040
#define	VM_FAULT_NOPAGE		0x000100
#define	VM_FAULT_LOCKED		0x000200
#define	VM_FAULT_RETRY		0x000400
#define	VM_FAULT_FALLBACK	0x000800
#define	VM_FAULT_DONE_COW	0x001000
#define	VM_FAULT_ERROR		(VM_FAULT_OOM | VM_FAULT_SIGBUS | VM_FAULT_SIGSEGV | VM_FAULT_HWPOISON)

struct vm_operations_struct {
	void (*open)(struct vm_area_struct *area);
	void (*close)(struct vm_area_struct *area);
	vm_fault_t (*fault)(struct vm_fault *vmf);
	vm_fault_t (*huge_fault)(struct vm_fault *vmf, unsigned int order);
	int (*access)(struct vm_area_struct *vma, unsigned long addr,
	    void *buf, int len, int write);
	const char *(*name)(struct vm_area_struct *vma);
	int (*mremap)(struct vm_area_struct *area);
	int (*may_split)(struct vm_area_struct *area, unsigned long addr);
};

#endif
