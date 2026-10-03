/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's iosys_map (include/linux/iosys-map.h, GPL-2.0; reimplemented):
 * a kernel address that is either system memory or I/O memory.
 */
#ifndef _LINUX_IOSYS_MAP_H
#define	_LINUX_IOSYS_MAP_H

#include <linux/io.h>
#include <linux/string.h>

struct iosys_map {
	union {
		void __iomem *vaddr_iomem;
		void *vaddr;
	};
	bool is_iomem;
};

#define	IOSYS_MAP_INIT_VADDR(v)		{ .vaddr = (v), .is_iomem = false }
#define	IOSYS_MAP_INIT_VADDR_IOMEM(v)	{ .vaddr_iomem = (v), .is_iomem = true }
#define	IOSYS_MAP_INIT_OFFSET(m, off) ({ \
	struct iosys_map __m = *(m); iosys_map_incr(&__m, off); __m; })

static inline void
iosys_map_set_vaddr(struct iosys_map *m, void *v)
{
	m->vaddr = v;
	m->is_iomem = false;
}

static inline void
iosys_map_set_vaddr_iomem(struct iosys_map *m, void __iomem *v)
{
	m->vaddr_iomem = v;
	m->is_iomem = true;
}

static inline bool
iosys_map_is_null(const struct iosys_map *m)
{
	return (m->vaddr == NULL);
}

#define	iosys_map_is_set(m)	(!iosys_map_is_null(m))
#define	iosys_map_clear(m)	((m)->vaddr = NULL, (m)->is_iomem = false)
#define	iosys_map_is_equal(a, b) \
	((a)->is_iomem == (b)->is_iomem && (a)->vaddr == (b)->vaddr)

static inline void
iosys_map_incr(struct iosys_map *m, size_t incr)
{
	m->vaddr = (char *)m->vaddr + incr;
}

static inline void
iosys_map_memcpy_to(struct iosys_map *dst, size_t off, const void *src,
    size_t len)
{
	memcpy((char *)dst->vaddr + off, src, len);
}

static inline void
iosys_map_memcpy_from(void *dst, const struct iosys_map *src, size_t off,
    size_t len)
{
	memcpy(dst, (char *)src->vaddr + off, len);
}

static inline void
iosys_map_memset(struct iosys_map *dst, size_t off, int value, size_t len)
{
	memset((char *)dst->vaddr + off, value, len);
}

#define	iosys_map_rd(map, off, type) \
	(*(volatile type *)((char *)(map)->vaddr + (off)))
#define	iosys_map_wr(map, off, type, val) \
	(*(volatile type *)((char *)(map)->vaddr + (off)) = (val))
#define	iosys_map_rd_field(map, off, struct_type, field) \
	iosys_map_rd(map, (off) + offsetof(struct_type, field), \
	    typeof (((struct_type *)0)->field))
#define	iosys_map_wr_field(map, off, struct_type, field, val) \
	iosys_map_wr(map, (off) + offsetof(struct_type, field), \
	    typeof (((struct_type *)0)->field), val)

#endif
