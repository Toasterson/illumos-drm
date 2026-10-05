/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Scatterlist tables (the interface of Linux's <linux/scatterlist.h>;
 * independent implementation).
 *
 * sg_alloc_table() allocates the entries as one array with kmalloc()
 * (kmem_alloc(9F) is virtually contiguous for any size), so a table built
 * here is never chained; sg_next() still follows chains that drivers make
 * with sg_chain(). Pages are "contiguous" for
 * sg_alloc_table_from_pages_segment() when their kernel addresses are,
 * which is the layer's page_to_pfn() order (<linux/mm.h>).
 */

#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/bug.h>
#include "lkpi_internal.h"
#include "lkpi_mem.h"

int
sg_alloc_table(struct sg_table *table, unsigned int nents, gfp_t gfp)
{
	struct scatterlist *sgl;

	bzero(table, sizeof (*table));
	if (nents == 0)
		return (-EINVAL);
	sgl = kmalloc_array(nents, sizeof (*sgl), gfp);
	if (sgl == NULL)
		return (-ENOMEM);
	sg_init_table(sgl, nents);
	table->sgl = sgl;
	table->nents = table->orig_nents = nents;
	return (0);
}

void
sg_free_table(struct sg_table *table)
{
	if (table == NULL || table->sgl == NULL)
		return;
	lkpi_dma_sgl_release(table->sgl);
	kfree(table->sgl);
	table->sgl = NULL;
	table->nents = table->orig_nents = 0;
	table->lkpi_dma = NULL;
}

static bool
lkpi_pages_adjacent(struct lx_page *a, struct lx_page *b)
{
	return ((caddr_t)b->lkpi_kva == (caddr_t)a->lkpi_kva + PAGE_SIZE);
}

/*
 * One entry per run of adjacent pages of at most max_segment bytes (a
 * whole number of pages); the first entry starts `offset` bytes into
 * pages[0], and the entries cover `size` bytes.
 */
int
sg_alloc_table_from_pages_segment(struct sg_table *sgt, struct lx_page **pages,
    unsigned int n_pages, unsigned int offset, unsigned long size,
    unsigned int max_segment, gfp_t gfp)
{
	unsigned int max_pages, nchunks, i, j, run;
	struct scatterlist *sg;
	unsigned long len;
	int err;

	max_segment = (unsigned int)P2ALIGN(max_segment, PAGE_SIZE);
	if (max_segment == 0 || pages == NULL || n_pages == 0 ||
	    offset >= PAGE_SIZE) {
		WARN_ON(max_segment == 0);
		return (-EINVAL);
	}
	/*
	 * Virtually adjacent pages need not be physically adjacent, and
	 * dma_map_sg() cannot map an entry that spans two DMA cookies: an
	 * entry holds one page here (dma_max_mapping_size() is PAGE_SIZE).
	 */
	max_pages = 1;

	nchunks = 0;
	for (i = 0; i < n_pages; i += run) {
		for (run = 1; i + run < n_pages && run < max_pages &&
		    lkpi_pages_adjacent(pages[i + run - 1], pages[i + run]);
		    run++)
			;
		nchunks++;
	}

	if ((err = sg_alloc_table(sgt, nchunks, gfp)) != 0)
		return (err);

	sg = sgt->sgl;
	for (i = 0; i < n_pages; i += run) {
		for (run = 1; i + run < n_pages && run < max_pages &&
		    lkpi_pages_adjacent(pages[i + run - 1], pages[i + run]);
		    run++)
			;
		len = ((unsigned long)run << PAGE_SHIFT) - offset;
		sg_set_page(sg, pages[i], (unsigned int)MIN(size, len), offset);
		size -= MIN(size, len);
		offset = 0;
		j = i + run;
		sg = (j < n_pages) ? sg_next(sg) : sg;
	}
	return (0);
}

int
sg_nents(struct scatterlist *sg)
{
	int n;

	for (n = 0; sg != NULL; sg = sg_next(sg))
		n++;
	return (n);
}

struct scatterlist *
sg_last(struct scatterlist *s, unsigned int nents)
{
	struct scatterlist *last = NULL;
	unsigned int i;

	for (i = 0; i < nents && s != NULL; i++, s = sg_next(s))
		last = s;
	return (last);
}

/*
 * Copies between buf and the list's memory from byte `skip` on; every
 * entry has a kernel address (sg_virt()).
 */
static size_t
lkpi_sg_copy(struct scatterlist *sgl, unsigned int nents, void *buf,
    size_t buflen, off_t skip, bool to_buffer)
{
	struct scatterlist *sg;
	size_t done = 0, n, off;
	unsigned int i;

	for (i = 0, sg = sgl; i < nents && sg != NULL && done < buflen;
	    i++, sg = sg_next(sg)) {
		if ((size_t)skip >= sg->length) {
			skip -= sg->length;
			continue;
		}
		off = (size_t)skip;
		skip = 0;
		n = MIN(sg->length - off, buflen - done);
		if (to_buffer)
			bcopy((char *)sg_virt(sg) + off, (char *)buf + done, n);
		else
			bcopy((char *)buf + done, (char *)sg_virt(sg) + off, n);
		done += n;
	}
	return (done);
}

size_t
sg_copy_from_buffer(struct scatterlist *sgl, unsigned int nents,
    const void *buf, size_t buflen)
{
	return (lkpi_sg_copy(sgl, nents, (void *)buf, buflen, 0, false));
}

size_t
sg_copy_to_buffer(struct scatterlist *sgl, unsigned int nents, void *buf,
    size_t buflen)
{
	return (lkpi_sg_copy(sgl, nents, buf, buflen, 0, true));
}

size_t
sg_pcopy_to_buffer(struct scatterlist *sgl, unsigned int nents, void *buf,
    size_t buflen, off_t skip)
{
	return (lkpi_sg_copy(sgl, nents, buf, buflen, skip, true));
}

/* --- page iteration ---------------------------------------------------- */

void
__sg_page_iter_start(struct sg_page_iter *piter, struct scatterlist *sglist,
    unsigned int nents, unsigned long pgoffset)
{
	piter->sg = sglist;
	piter->sg_pgoffset = (unsigned int)pgoffset;
	piter->__nents = nents;
	piter->__pg_advance = 0;
}

/* The pages an entry spans: its offset counts into the first one. */
static unsigned int
lkpi_sg_npages(unsigned int offset, unsigned int length)
{
	return ((unsigned int)(P2ROUNDUP((uint64_t)offset + length,
	    PAGE_SIZE) >> PAGE_SHIFT));
}

static bool
lkpi_sg_iter_next(struct sg_page_iter *piter, bool dma)
{
	unsigned int npages;

	if (piter->__nents == 0 || piter->sg == NULL)
		return (false);
	piter->sg_pgoffset += piter->__pg_advance;
	piter->__pg_advance = 1;
	for (;;) {
		npages = lkpi_sg_npages(piter->sg->offset,
		    dma ? sg_dma_len(piter->sg) : piter->sg->length);
		if (piter->sg_pgoffset < npages)
			return (true);
		piter->sg_pgoffset -= npages;
		if (--piter->__nents == 0)
			return (false);
		if ((piter->sg = sg_next(piter->sg)) == NULL)
			return (false);
	}
}

bool
__sg_page_iter_next(struct sg_page_iter *piter)
{
	return (lkpi_sg_iter_next(piter, false));
}

bool
__sg_page_iter_dma_next(struct sg_dma_page_iter *dma_iter)
{
	return (lkpi_sg_iter_next(&dma_iter->base, true));
}
