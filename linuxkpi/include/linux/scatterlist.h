/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's scatterlists (include/linux/scatterlist.h, GPL-2.0;
 * reimplemented; the chaining scheme of page_link low bits is Linux's
 * interface, the code is the layer's, src/lkpi_scatterlist.c). An entry
 * points at a layer page (<linux/mm_types.h>) or, after sg_set_buf(), at
 * a kernel address of its own. dma_map_sg() fills dma_address/dma_length
 * (<linux/dma-mapping.h>).
 */
#ifndef _LINUX_SCATTERLIST_H
#define	_LINUX_SCATTERLIST_H

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/bug.h>
#include <linux/string.h>

struct scatterlist {
	unsigned long page_link;
	unsigned int offset;
	unsigned int length;
	dma_addr_t dma_address;
	unsigned int dma_length;
	unsigned int dma_flags;
	void *lkpi_buf;
};

struct sg_table {
	struct scatterlist *sgl;
	unsigned int nents;
	unsigned int orig_nents;
	void *lkpi_dma;
};

struct sg_append_table {
	struct sg_table sgt;
	struct scatterlist *prv;
	unsigned int total_nents;
};

#define	SG_CHAIN		0x01UL
#define	SG_END			0x02UL
#define	SG_PAGE_LINK_MASK	(SG_CHAIN | SG_END)
#define	SG_MAX_SINGLE_ALLOC	(PAGE_SIZE / sizeof (struct scatterlist))
#define	SG_CHUNK_SIZE		128

#define	sg_is_chain(sg)		(((sg)->page_link & SG_CHAIN) != 0)
#define	sg_is_last(sg)		(((sg)->page_link & SG_END) != 0)
#define	sg_chain_ptr(sg) \
	((struct scatterlist *)((sg)->page_link & ~SG_PAGE_LINK_MASK))
#define	sg_dma_address(sg)	((sg)->dma_address)
#define	sg_dma_len(sg)		((sg)->dma_length)
#define	sg_dma_is_bus_address(sg)	false
#define	sg_dma_mark_bus_address(sg)	((void)(sg))

static inline struct lx_page *
sg_page(struct scatterlist *sg)
{
	return ((struct lx_page *)(sg->page_link & ~SG_PAGE_LINK_MASK));
}

static inline void
sg_assign_page(struct scatterlist *sg, struct lx_page *page)
{
	unsigned long flags = sg->page_link & SG_PAGE_LINK_MASK;

	sg->page_link = flags | (unsigned long)page;
	sg->lkpi_buf = NULL;
}

static inline void
sg_set_page(struct scatterlist *sg, struct lx_page *page, unsigned int len,
    unsigned int offset)
{
	sg_assign_page(sg, page);
	sg->offset = offset;
	sg->length = len;
}

static inline void
sg_set_folio(struct scatterlist *sg, struct folio *folio, size_t len,
    size_t offset)
{
	sg_set_page(sg, &folio->page, (unsigned int)len, (unsigned int)offset);
}

/* A buffer of kernel memory; the layer keeps its address, not a page. */
static inline void
sg_set_buf(struct scatterlist *sg, const void *buf, unsigned int buflen)
{
	sg->page_link &= SG_PAGE_LINK_MASK;
	sg->lkpi_buf = (void *)(uintptr_t)buf;
	sg->offset = 0;
	sg->length = buflen;
}

static inline void *
sg_virt(struct scatterlist *sg)
{
	if (sg->lkpi_buf != NULL)
		return ((char *)sg->lkpi_buf + sg->offset);
	return ((char *)page_address(sg_page(sg)) + sg->offset);
}

static inline void
sg_mark_end(struct scatterlist *sg)
{
	sg->page_link |= SG_END;
	sg->page_link &= ~SG_CHAIN;
}

static inline void
sg_unmark_end(struct scatterlist *sg)
{
	sg->page_link &= ~SG_END;
}

static inline void
sg_chain(struct scatterlist *prv, unsigned int prv_nents,
    struct scatterlist *sgl)
{
	prv[prv_nents - 1].offset = 0;
	prv[prv_nents - 1].length = 0;
	prv[prv_nents - 1].page_link = ((unsigned long)sgl | SG_CHAIN) & ~SG_END;
}

static inline struct scatterlist *
sg_next(struct scatterlist *sg)
{
	if (sg_is_last(sg))
		return (NULL);
	sg++;
	if (sg_is_chain(sg))
		sg = sg_chain_ptr(sg);
	return (sg);
}

static inline void
sg_init_table(struct scatterlist *sgl, unsigned int nents)
{
	memset(sgl, 0, sizeof (*sgl) * nents);
	sg_mark_end(&sgl[nents - 1]);
}

static inline void
sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen)
{
	sg_init_table(sg, 1);
	sg_set_buf(sg, buf, buflen);
}

static inline dma_addr_t
sg_phys(struct scatterlist *sg)
{
	return (lkpi_page_to_phys(sg_page(sg)) + sg->offset);
}

#define	for_each_sg(sglist, sg, nr, __i) \
	for ((__i) = 0, (sg) = (sglist); (__i) < (nr); (__i)++, (sg) = sg_next(sg))
#define	for_each_sgtable_sg(sgt, sg, i)	for_each_sg((sgt)->sgl, sg, (sgt)->orig_nents, i)
#define	for_each_sgtable_dma_sg(sgt, sg, i)	for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)

extern int sg_alloc_table(struct sg_table *table, unsigned int nents,
    gfp_t gfp);
extern void sg_free_table(struct sg_table *table);
extern int sg_alloc_table_from_pages_segment(struct sg_table *sgt,
    struct lx_page **pages, unsigned int n_pages, unsigned int offset,
    unsigned long size, unsigned int max_segment, gfp_t gfp);
extern int sg_nents(struct scatterlist *sg);
extern struct scatterlist *sg_last(struct scatterlist *s, unsigned int nents);
extern size_t sg_copy_from_buffer(struct scatterlist *sgl, unsigned int nents,
    const void *buf, size_t buflen);
extern size_t sg_copy_to_buffer(struct scatterlist *sgl, unsigned int nents,
    void *buf, size_t buflen);
extern size_t sg_pcopy_to_buffer(struct scatterlist *sgl, unsigned int nents,
    void *buf, size_t buflen, off_t skip);

static inline int
sg_alloc_table_from_pages(struct sg_table *sgt, struct lx_page **pages,
    unsigned int n_pages, unsigned int offset, unsigned long size, gfp_t gfp)
{
	return (sg_alloc_table_from_pages_segment(sgt, pages, n_pages, offset,
	    size, UINT_MAX, gfp));
}

/* Page iteration (one page per step). */
struct sg_page_iter {
	struct scatterlist *sg;
	unsigned int sg_pgoffset;
	unsigned int __nents;
	int __pg_advance;
};

struct sg_dma_page_iter {
	struct sg_page_iter base;
};

extern bool __sg_page_iter_next(struct sg_page_iter *piter);
extern bool __sg_page_iter_dma_next(struct sg_dma_page_iter *dma_iter);
extern void __sg_page_iter_start(struct sg_page_iter *piter,
    struct scatterlist *sglist, unsigned int nents, unsigned long pgoffset);

static inline struct lx_page *
sg_page_iter_page(struct sg_page_iter *piter)
{
	return (lkpi_nth_page(sg_page(piter->sg), piter->sg_pgoffset));
}

static inline dma_addr_t
sg_page_iter_dma_address(struct sg_dma_page_iter *dma_iter)
{
	return (sg_dma_address(dma_iter->base.sg) +
	    ((dma_addr_t)dma_iter->base.sg_pgoffset << PAGE_SHIFT));
}

#define	for_each_sg_page(sglist, piter, nents, pgoffset) \
	for (__sg_page_iter_start((piter), (sglist), (nents), (pgoffset)); \
	    __sg_page_iter_next(piter);)
#define	for_each_sg_dma_page(sglist, dma_iter, dma_nents, pgoffset) \
	for (__sg_page_iter_start(&(dma_iter)->base, sglist, dma_nents, pgoffset); \
	    __sg_page_iter_dma_next(dma_iter);)
#define	for_each_sgtable_page(sgt, piter, pgoffset) \
	for_each_sg_page((sgt)->sgl, piter, (sgt)->orig_nents, pgoffset)
#define	for_each_sgtable_dma_page(sgt, dma_iter, pgoffset) \
	for_each_sg_dma_page((sgt)->sgl, dma_iter, (sgt)->nents, pgoffset)

#endif
