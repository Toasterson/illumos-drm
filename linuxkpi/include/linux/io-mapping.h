/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's io_mapping (include/linux/io-mapping.h, GPL-2.0;
 * reimplemented): a write-combining mapping of a BAR range made once with
 * ioremap_wc(); the map functions return addresses inside it and the
 * unmap functions do nothing.
 */
#ifndef _LINUX_IO_MAPPING_H
#define	_LINUX_IO_MAPPING_H

#include <linux/io.h>
#include <linux/slab.h>
#include <asm/pgtable_types.h>

struct io_mapping {
	resource_size_t base;
	unsigned long size;
	pgprot_t prot;
	void __iomem *iomem;
};

static inline struct io_mapping *
io_mapping_init_wc(struct io_mapping *iomap, resource_size_t base,
    unsigned long size)
{
	iomap->iomem = ioremap_wc(base, size);
	if (iomap->iomem == NULL)
		return (NULL);
	iomap->base = base;
	iomap->size = size;
	iomap->prot = pgprot_writecombine(PAGE_KERNEL);
	return (iomap);
}

static inline void
io_mapping_fini(struct io_mapping *mapping)
{
	iounmap(mapping->iomem);
}

static inline struct io_mapping *
io_mapping_create_wc(resource_size_t base, unsigned long size)
{
	struct io_mapping *iomap = kmalloc(sizeof (*iomap), GFP_KERNEL);

	if (iomap != NULL && io_mapping_init_wc(iomap, base, size) == NULL) {
		kfree(iomap);
		iomap = NULL;
	}
	return (iomap);
}

static inline void
io_mapping_free(struct io_mapping *iomap)
{
	io_mapping_fini(iomap);
	kfree(iomap);
}

static inline void __iomem *
io_mapping_map_wc(struct io_mapping *mapping, unsigned long offset,
    unsigned long size)
{
	return ((char __iomem *)mapping->iomem + offset);
}

#define	io_mapping_map_atomic_wc(m, off)	io_mapping_map_wc(m, off, PAGE_SIZE)
#define	io_mapping_map_local_wc(m, off)		io_mapping_map_wc(m, off, PAGE_SIZE)
#define	io_mapping_unmap(v)			((void)(v))
#define	io_mapping_unmap_atomic(v)		((void)(v))
#define	io_mapping_unmap_local(v)		((void)(v))
#define	io_mapping_map_user(m, vma, addr, pfn, size)	(-ENODEV)

#endif
