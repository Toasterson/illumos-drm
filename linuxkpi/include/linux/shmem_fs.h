/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's shmem files (include/linux/shmem_fs.h, GPL-2.0; independent
 * implementation in src/lkpi_shmem.c). A shmem file's memory is one
 * ddi_umem_alloc(9F) allocation of the file's size (page-aligned,
 * zero-filled, locked: illumos kernel memory is not paged), made when
 * the first page is read, so its pages are virtually contiguous and the
 * DRM core can map the same cookie to user space. shmem_truncate_range()
 * of the whole file frees the memory (a later read sees zeros); partial
 * truncation zeroes the range. There is no swap: writeback is "not
 * supported" and leaves the pages in place (returns 0, the folio stays).
 */
#ifndef _LINUX_SHMEM_FS_H
#define	_LINUX_SHMEM_FS_H

#include <linux/fs.h>
#include <linux/pagemap.h>

#define	VM_NORESERVE_SHMEM	0x00200000

extern struct lx_file *shmem_file_setup(const char *name, loff_t size,
    vma_flags_t flags);
extern struct lx_file *shmem_file_setup_with_mnt(struct vfsmount *mnt,
    const char *name, loff_t size, vma_flags_t flags);
extern struct folio *shmem_read_folio_gfp(struct address_space *mapping,
    pgoff_t index, gfp_t gfp);
extern struct lx_page *shmem_read_mapping_page_gfp(
    struct address_space *mapping, pgoff_t index, gfp_t gfp);
extern void shmem_truncate_range(struct inode *inode, loff_t start,
    loff_t end);
extern int shmem_writeout(struct folio *folio, struct swap_iocb **plug,
    struct list_head *folio_list);
extern ddi_umem_cookie_t lkpi_shmem_umem(struct lx_file *file,
    void **kvap, size_t *sizep);

#define	shmem_read_mapping_page(m, idx) \
	shmem_read_mapping_page_gfp((m), (idx), mapping_gfp_mask(m))
#define	shmem_read_folio(m, idx) \
	shmem_read_folio_gfp((m), (idx), mapping_gfp_mask(m))
#define	shmem_mapping(m)	((m)->lkpi_shmem != NULL)
#define	SHMEM_HUGE_NEVER	0
#define	SHMEM_HUGE_ALWAYS	1
#define	SHMEM_HUGE_WITHIN_SIZE	2
#define	SHMEM_HUGE_ADVISE	3

#endif
