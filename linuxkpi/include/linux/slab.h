/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's kmalloc() family and slab caches over illumos kmem (include/
 * linux/slab.h, GPL-2.0; reimplemented in src/lkpi_slab.c).
 *
 * kmalloc() and kvmalloc() allocate with kmem_alloc(9F) after a 16-byte
 * header that records the size, because kfree() has no size argument;
 * the returned memory is 16-byte aligned. ksize() returns the requested
 * size. A gfp without __GFP_DIRECT_RECLAIM allocates with KM_NOSLEEP and
 * may fail; with it, KM_SLEEP never fails. kmalloc(0) returns
 * ZERO_SIZE_PTR, as on Linux. Slab caches are illumos kmem caches
 * (constructors are run per allocation, not per slab, which is what
 * Linux's constructor contract allows). illumos' own kmem_cache_*()
 * functions are renamed by function-like macros here; layer sources call
 * them as (kmem_cache_create)(...) with the kmem_cache_t type.
 */
#ifndef _LINUX_SLAB_H
#define	_LINUX_SLAB_H

#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/overflow.h>
#include <linux/cleanup.h>
#include <linux/err.h>

#define	ZERO_SIZE_PTR		((void *)16)
#define	ZERO_OR_NULL_PTR(x)	((unsigned long)(x) <= (unsigned long)ZERO_SIZE_PTR)
#define	ARCH_KMALLOC_MINALIGN	16
#define	ARCH_DMA_MINALIGN	64
#define	KMALLOC_MAX_SIZE	(1UL << 30)

#define	SLAB_HWCACHE_ALIGN	0x00002000u
#define	SLAB_PANIC		0x00040000u
#define	SLAB_TYPESAFE_BY_RCU	0x00080000u
#define	SLAB_RECLAIM_ACCOUNT	0x00020000u
#define	SLAB_ACCOUNT		0x04000000u
#define	SLAB_NO_MERGE		0x01000000u

extern void *lkpi_kmalloc(size_t size, gfp_t gfp);
extern void *lkpi_krealloc(const void *p, size_t new_size, gfp_t gfp);
extern void lkpi_kfree(const void *p);
extern size_t ksize(const void *p);

#define	kmalloc(size, gfp)		lkpi_kmalloc((size), (gfp))
#define	kzalloc(size, gfp)		lkpi_kmalloc((size), (gfp) | __GFP_ZERO)
#define	kmalloc_node(size, gfp, n)	kmalloc(size, gfp)
#define	kzalloc_node(size, gfp, n)	kzalloc(size, gfp)
#define	kmalloc_array(n, size, gfp)	kmalloc(size_mul((n), (size)), (gfp))
#define	kcalloc(n, size, gfp)		kzalloc(size_mul((n), (size)), (gfp))
#define	kmalloc_array_node(n, s, g, nd)	kmalloc_array(n, s, g)
#define	kcalloc_node(n, s, g, nd)	kcalloc(n, s, g)
#define	krealloc(p, size, gfp)		lkpi_krealloc((p), (size), (gfp))
#define	krealloc_array(p, n, s, gfp)	lkpi_krealloc((p), size_mul((n), (s)), (gfp))
#define	kfree(p)			lkpi_kfree(p)
#define	kfree_sensitive(p)		lkpi_kfree(p)
#define	kvmalloc(size, gfp)		lkpi_kmalloc((size), (gfp))
#define	kvzalloc(size, gfp)		lkpi_kmalloc((size), (gfp) | __GFP_ZERO)
#define	kvmalloc_node(size, gfp, n)	kvmalloc(size, gfp)
#define	kvzalloc_node(size, gfp, n)	kvzalloc(size, gfp)
#define	kvmalloc_array(n, size, gfp)	kvmalloc(size_mul((n), (size)), (gfp))
#define	kvcalloc(n, size, gfp)		kvzalloc(size_mul((n), (size)), (gfp))
#define	kvrealloc(p, size, gfp)		lkpi_krealloc((p), (size), (gfp))
#define	kvfree(p)			lkpi_kfree(p)
#define	kvfree_sensitive(p, len)	lkpi_kfree(p)
#define	kmalloc_obj(p, gfp)		kmalloc(sizeof (*(p)), gfp)
#define	kzalloc_obj(p, gfp)		kzalloc(sizeof (*(p)), gfp)
#define	kmalloc_size_roundup(s)		(s)

DEFINE_FREE(kfree, void *, if (!IS_ERR_OR_NULL(_T)) kfree(_T))
DEFINE_FREE(kvfree, void *, if (!IS_ERR_OR_NULL(_T)) kvfree(_T))

/* --- slab caches --------------------------------------------------------- */

#define	kmem_cache_create(...)	lkpi_kmem_cache_create(__VA_ARGS__)
#define	kmem_cache_destroy(...)	lkpi_kmem_cache_destroy(__VA_ARGS__)
#define	kmem_cache_alloc(...)	lkpi_kmem_cache_alloc(__VA_ARGS__)
#define	kmem_cache_zalloc(...)	lkpi_kmem_cache_zalloc(__VA_ARGS__)
#define	kmem_cache_free(...)	lkpi_kmem_cache_free(__VA_ARGS__)
#define	kmem_cache_shrink(...)	lkpi_kmem_cache_shrink(__VA_ARGS__)

struct lkpi_kmem_cache;

extern struct lkpi_kmem_cache *lkpi_kmem_cache_create(const char *name,
    unsigned int size, unsigned int align, slab_flags_t flags,
    void (*ctor)(void *));
extern void lkpi_kmem_cache_destroy(struct lkpi_kmem_cache *c);
extern void *lkpi_kmem_cache_alloc(struct lkpi_kmem_cache *c, gfp_t gfp);
extern void lkpi_kmem_cache_free(struct lkpi_kmem_cache *c, void *p);
extern int lkpi_kmem_cache_shrink(struct lkpi_kmem_cache *c);

static inline void *
lkpi_kmem_cache_zalloc(struct lkpi_kmem_cache *c, gfp_t gfp)
{
	return (lkpi_kmem_cache_alloc(c, gfp | __GFP_ZERO));
}

/* Linux names the cache type struct kmem_cache. */
#define	kmem_cache	lkpi_kmem_cache
#define	KMEM_CACHE(__struct, __flags) \
	lkpi_kmem_cache_create(#__struct, sizeof (struct __struct), \
	    __alignof__(struct __struct), (__flags), NULL)
#define	KMEM_CACHE_USERCOPY(s, f, field)	KMEM_CACHE(s, f)
#define	kmem_cache_alloc_node(c, g, n)	lkpi_kmem_cache_alloc(c, g)

#endif
