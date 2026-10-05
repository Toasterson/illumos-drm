/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's struct resource (include/linux/ioport.h, GPL-2.0; reduced):
 * the PCI glue fills one per BAR from the device's "assigned-addresses".
 * Resource-tree operations (request_mem_region and friends) are not
 * supported: the illumos PCI nexus owns the address space; they succeed
 * without effect for ranges inside a BAR of the device.
 */
#ifndef _LINUX_IOPORT_H
#define	_LINUX_IOPORT_H

#include <linux/types.h>

struct resource {
	resource_size_t start;
	resource_size_t end;
	const char *name;
	unsigned long flags;
	unsigned long desc;
	struct resource *parent, *sibling, *child;
};

#define	IORESOURCE_IO		0x00000100
#define	IORESOURCE_MEM		0x00000200
#define	IORESOURCE_REG		0x00000300
#define	IORESOURCE_IRQ		0x00000400
#define	IORESOURCE_PREFETCH	0x00002000
#define	IORESOURCE_MEM_64	0x00100000
#define	IORESOURCE_UNSET	0x20000000
#define	IORESOURCE_BUSY		0x80000000
#define	DEFINE_RES_MEM(_start, _size) \
	((struct resource){ .start = (_start), .end = (_start) + (_size) - 1, \
	.flags = IORESOURCE_MEM })
#define	DEFINE_RES_NAMED(_start, _size, _name, _flags) \
	((struct resource){ .start = (_start), .end = (_start) + (_size) - 1, \
	.name = (_name), .flags = (_flags) })

static inline resource_size_t
resource_size(const struct resource *res)
{
	return (res->end - res->start + 1);
}

static inline bool
resource_contains(const struct resource *r1, const struct resource *r2)
{
	return (r1->start <= r2->start && r1->end >= r2->end);
}

static inline bool
resource_overlaps(const struct resource *r1, const struct resource *r2)
{
	return (r1->start <= r2->end && r1->end >= r2->start);
}

extern struct resource iomem_resource;

/*
 * The firmware-reserved ("stolen") memory of an Intel integrated GPU, as
 * Linux's x86 early quirk records it: the PCI glue fills it when an Intel
 * display-class device attaches, from the device's BDSM and GMCH control
 * registers (gen6 to gen9.5). Empty (resource_size() == 0) otherwise.
 */
extern struct resource intel_graphics_stolen_res;
#define	request_mem_region(start, n, name)	((struct resource *)&iomem_resource)
#define	release_mem_region(start, n)		((void)(start))
#define	request_resource(root, new_r)		0
#define	release_resource(r)			0
#define	insert_resource(p, r)			0
#define	resource_type(r)			((r)->flags & 0x00001f00)
#define	devm_request_mem_region(d, s, n, name)	((struct resource *)&iomem_resource)

#endif
