/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's IDR and IDA (include/linux/idr.h, GPL-2.0; reimplemented over
 * the layer's XArray, src/lkpi_xarray.c). idr_alloc() hands out the lowest
 * free id in [start, end) (end <= 0 means INT_MAX); idr_alloc_cyclic()
 * continues after the last id. idr_preload() is not needed (allocation
 * uses the given gfp) and does nothing.
 */
#ifndef _LINUX_IDR_H
#define	_LINUX_IDR_H

#include <linux/xarray.h>

struct idr {
	struct xarray idr_rt;
	unsigned int idr_base;
	unsigned int idr_next;
};

#define	IDR_INIT_BASE(name, base) \
	{ .idr_rt = XARRAY_INIT(name, XA_FLAGS_ALLOC), .idr_base = (base), .idr_next = 0 }
#define	IDR_INIT(name)		IDR_INIT_BASE(name, 0)
#define	DEFINE_IDR(name)	struct idr name = IDR_INIT(name)

extern void idr_init_base(struct idr *idr, int base);
extern int idr_alloc(struct idr *idr, void *ptr, int start, int end,
    gfp_t gfp);
extern int idr_alloc_u32(struct idr *idr, void *ptr, u32 *nextid,
    unsigned long max, gfp_t gfp);
extern int idr_alloc_cyclic(struct idr *idr, void *ptr, int start, int end,
    gfp_t gfp);
extern void *idr_find(const struct idr *idr, unsigned long id);
extern void *idr_remove(struct idr *idr, unsigned long id);
extern void *idr_replace(struct idr *idr, void *ptr, unsigned long id);
extern void *idr_get_next(struct idr *idr, int *nextid);
extern void *idr_get_next_ul(struct idr *idr, unsigned long *nextid);
extern int idr_for_each(const struct idr *idr,
    int (*fn)(int id, void *p, void *data), void *data);
extern void idr_destroy(struct idr *idr);

static inline void
idr_init(struct idr *idr)
{
	idr_init_base(idr, 0);
}

static inline bool
idr_is_empty(const struct idr *idr)
{
	return (xa_empty(&idr->idr_rt));
}

#define	idr_preload(gfp)	((void)(gfp))
#define	idr_preload_end()	((void)0)
#define	idr_lock(idr)		xa_lock(&(idr)->idr_rt)
#define	idr_unlock(idr)		xa_unlock(&(idr)->idr_rt)
#define	idr_for_each_entry(idr, entry, id) \
	for ((id) = 0; ((entry) = idr_get_next((idr), &(id))) != NULL; (id) += 1U)
#define	idr_for_each_entry_ul(idr, entry, tmp, id) \
	for ((tmp) = 0, (id) = 0; \
	    (tmp) <= (id) && ((entry) = idr_get_next_ul((idr), &(id))) != NULL; \
	    (tmp) = (id), ++(id))
#define	idr_for_each_entry_continue(idr, entry, id) \
	for ((entry) = idr_get_next((idr), &(id)); (entry) != NULL; \
	    ++(id), (entry) = idr_get_next((idr), &(id)))

struct ida {
	struct xarray xa;
};

#define	IDA_INIT(name)		{ .xa = XARRAY_INIT(name, XA_FLAGS_ALLOC) }
#define	DEFINE_IDA(name)	struct ida name = IDA_INIT(name)

extern int ida_alloc_range(struct ida *ida, unsigned int min,
    unsigned int max, gfp_t gfp);
extern void ida_free(struct ida *ida, unsigned int id);
extern void ida_destroy(struct ida *ida);

static inline void
ida_init(struct ida *ida)
{
	xa_init_flags(&ida->xa, XA_FLAGS_ALLOC);
}

#define	ida_alloc(ida, gfp)		ida_alloc_range((ida), 0, ~0U, (gfp))
#define	ida_alloc_min(ida, min, gfp)	ida_alloc_range((ida), (min), ~0U, (gfp))
#define	ida_alloc_max(ida, max, gfp)	ida_alloc_range((ida), 0, (max), (gfp))
#define	ida_is_empty(ida)		xa_empty(&(ida)->xa)

#endif
