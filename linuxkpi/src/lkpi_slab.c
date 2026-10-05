/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * kmalloc() and slab caches over illumos kmem (the interface of Linux's
 * <linux/slab.h>; independent implementation, see the header for the
 * contract).
 *
 * kmalloc() memory is kmem_alloc(9F) memory behind a 16-byte header that
 * records the requested size (kfree() has none) and a magic number, so a
 * kfree() of memory kmalloc() did not return (a slab object, a stack or
 * static address, a double free) is reported and refused instead of
 * corrupting kmem. kmem_alloc() returns 8-byte aligned memory; 8 bytes of
 * slack let the header start on a 16-byte boundary, which makes the
 * returned memory 16-byte aligned. kfree() of a page-aligned address
 * first asks the vmap() records: Linux allows vfree() of a vmap() address
 * and vfree() is kfree() here.
 *
 * A slab cache is an illumos kmem cache. The Linux constructor runs on
 * every allocation, which satisfies Linux's contract (objects are freed in
 * their constructed state, so constructing again is idempotent).
 *
 * SLAB_TYPESAFE_BY_RCU: Linux keeps such objects' memory a valid object
 * of the type until a grace period after kmem_cache_free(), so RCU readers
 * may still look at a freed object (i915's requests, dma-fence lookups).
 * illumos kmem would hand a freed buffer to the next allocation at once,
 * so the layer delays the free itself: each object carries a deferred-free
 * record after its Linux size (never seen by the driver), which
 * kmem_cache_free() passes to call_rcu(); the callback returns the buffer
 * to kmem after the grace period. kmem_cache_destroy() waits for pending
 * callbacks with rcu_barrier().
 */

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/rcupdate.h>
#include <linux/bug.h>
#include <linux/limits.h>
#include "lkpi_internal.h"
#include "lkpi_mem.h"

#define	LKPI_KM_MAGIC	0x6c6b6d61U	/* "lkma" */
#define	LKPI_KM_SLACK	8		/* kmem_alloc() aligns to 8 bytes */
/* sys/kmem_impl.h: KMEM_CACHE_NAMELEN; longer names are cut there. */
#define	LKPI_CACHE_NAMELEN	31

struct lkpi_kmhdr {
	uint64_t	kh_size;	/* the requested size */
	uint32_t	kh_magic;
	uint32_t	kh_shift;	/* bytes from the kmem buffer to here */
};

_Static_assert(sizeof (struct lkpi_kmhdr) == 16, "kmalloc header size");

static inline size_t
lkpi_km_total(size_t size)
{
	return (size + sizeof (struct lkpi_kmhdr) + LKPI_KM_SLACK);
}

/* The header of kmalloc() memory, or NULL (with a warning) for anything else. */
static struct lkpi_kmhdr *
lkpi_km_hdr(const void *p, const char *who)
{
	struct lkpi_kmhdr *h = (struct lkpi_kmhdr *)p - 1;

	if (!IS_P2ALIGNED(p, 16) || h->kh_magic != LKPI_KM_MAGIC ||
	    (h->kh_shift != 0 && h->kh_shift != LKPI_KM_SLACK)) {
		WARN(1, "%s: %p is not kmalloc() memory", who, p);
		return (NULL);
	}
	return (h);
}

void *
lkpi_kmalloc(size_t size, gfp_t gfp)
{
	struct lkpi_kmhdr *h;
	size_t total;
	char *base;
	int kmflag = lkpi_kmflag(gfp);

	if (size == 0)
		return (ZERO_SIZE_PTR);
	/*
	 * Linux refuses sizes above INT_MAX; more than physical memory would
	 * block a KM_SLEEP allocation forever.
	 */
	if (size > INT_MAX || LKPI_BTOP(size) >= (size_t)physmem) {
		WARN_ONCE(!(gfp & __GFP_NOWARN), "kmalloc: %zu bytes", size);
		return (NULL);
	}
	total = lkpi_km_total(size);
	base = (gfp & __GFP_ZERO) ? kmem_zalloc(total, kmflag) :
	    kmem_alloc(total, kmflag);
	if (base == NULL)
		return (NULL);
	h = (struct lkpi_kmhdr *)(base + ((uintptr_t)base & 15));
	h->kh_size = size;
	h->kh_magic = LKPI_KM_MAGIC;
	h->kh_shift = (uint32_t)((uintptr_t)base & 15);
	return (h + 1);
}

void
lkpi_kfree(const void *p)
{
	struct lkpi_kmhdr *h;

	if (ZERO_OR_NULL_PTR(p))
		return;
	if (IS_P2ALIGNED(p, PAGE_SIZE) && lkpi_vmap_free(p))
		return;
	if ((h = lkpi_km_hdr(p, "kfree")) == NULL)
		return;
	h->kh_magic = 0;
	kmem_free((char *)h - h->kh_shift, lkpi_km_total(h->kh_size));
}

size_t
ksize(const void *p)
{
	struct lkpi_kmhdr *h;

	if (ZERO_OR_NULL_PTR(p))
		return (0);
	if ((h = lkpi_km_hdr(p, "ksize")) == NULL)
		return (0);
	return (h->kh_size);
}

/* Always moves: the recorded size is what kfree() gives kmem_free(). */
void *
lkpi_krealloc(const void *p, size_t new_size, gfp_t gfp)
{
	struct lkpi_kmhdr *h;
	void *n;

	if (ZERO_OR_NULL_PTR(p))
		return (lkpi_kmalloc(new_size, gfp));
	if ((h = lkpi_km_hdr(p, "krealloc")) == NULL)
		return (NULL);
	if (new_size == 0) {
		lkpi_kfree(p);
		return (ZERO_SIZE_PTR);
	}
	if ((n = lkpi_kmalloc(new_size, gfp)) == NULL)
		return (NULL);
	bcopy(p, n, MIN(h->kh_size, new_size));
	lkpi_kfree(p);
	return (n);
}

/* --- slab caches ------------------------------------------------------- */

struct lkpi_kmem_cache {
	kmem_cache_t	*kc_cache;
	void		(*kc_ctor)(void *);
	slab_flags_t	kc_flags;
	unsigned int	kc_size;
	size_t		kc_rcu_off;	/* SLAB_TYPESAFE_BY_RCU: the record */
	char		kc_name[LKPI_CACHE_NAMELEN + 1];
};

/* The deferred-free record after a SLAB_TYPESAFE_BY_RCU object. */
struct lkpi_slab_rcu {
	struct rcu_head		sr_head;
	struct lkpi_kmem_cache	*sr_cache;
};

/* illumos cache names are C identifiers (kstat names). */
static void
lkpi_cache_name(char *dst, size_t len, const char *name)
{
	size_t i;
	char c;

	if (name == NULL || name[0] == '\0')
		name = "lkpi_cache";
	for (i = 0; i + 1 < len && name[i] != '\0'; i++) {
		c = name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9' && i > 0) || c == '_'))
			c = '_';
		dst[i] = c;
	}
	dst[i] = '\0';
}

struct lkpi_kmem_cache *
lkpi_kmem_cache_create(const char *name, unsigned int size,
    unsigned int align, slab_flags_t flags, void (*ctor)(void *))
{
	struct lkpi_kmem_cache *c;
	size_t csize = size;

	if (align == 0)
		align = 8;
	if (flags & SLAB_HWCACHE_ALIGN)
		align = MAX(align, 64);
	/* kmem_cache_create() panics on these. */
	if (size == 0 || !ISP2(align) || align > PAGE_SIZE) {
		WARN(1, "kmem_cache_create(%s): size %u align %u", name, size,
		    align);
		return (NULL);
	}
	c = kmem_zalloc(sizeof (*c), KM_SLEEP);
	lkpi_cache_name(c->kc_name, sizeof (c->kc_name), name);
	c->kc_ctor = ctor;
	c->kc_flags = flags;
	c->kc_size = size;
	if (flags & SLAB_TYPESAFE_BY_RCU) {
		c->kc_rcu_off = P2ROUNDUP((size_t)size, sizeof (void *));
		csize = c->kc_rcu_off + sizeof (struct lkpi_slab_rcu);
	}
	c->kc_cache = (kmem_cache_create)(c->kc_name, csize, align, NULL, NULL,
	    NULL, NULL, NULL, 0);
	return (c);
}

void
lkpi_kmem_cache_destroy(struct lkpi_kmem_cache *c)
{
	if (c == NULL)
		return;
	if (c->kc_flags & SLAB_TYPESAFE_BY_RCU)
		rcu_barrier();
	(kmem_cache_destroy)(c->kc_cache);
	kmem_free(c, sizeof (*c));
}

void *
lkpi_kmem_cache_alloc(struct lkpi_kmem_cache *c, gfp_t gfp)
{
	void *p;

	if (c == NULL)
		return (NULL);
	if ((p = (kmem_cache_alloc)(c->kc_cache, lkpi_kmflag(gfp))) == NULL)
		return (NULL);
	if (gfp & __GFP_ZERO)
		bzero(p, c->kc_size);
	if (c->kc_ctor != NULL)
		c->kc_ctor(p);
	return (p);
}

static void
lkpi_slab_rcu_free(struct rcu_head *head)
{
	struct lkpi_slab_rcu *r = container_of(head, struct lkpi_slab_rcu,
	    sr_head);
	struct lkpi_kmem_cache *c = r->sr_cache;

	(kmem_cache_free)(c->kc_cache, (char *)r - c->kc_rcu_off);
}

void
lkpi_kmem_cache_free(struct lkpi_kmem_cache *c, void *p)
{
	struct lkpi_slab_rcu *r;

	if (c == NULL || p == NULL)
		return;
	if (c->kc_flags & SLAB_TYPESAFE_BY_RCU) {
		r = (struct lkpi_slab_rcu *)((char *)p + c->kc_rcu_off);
		r->sr_cache = c;
		call_rcu(&r->sr_head, lkpi_slab_rcu_free);
		return;
	}
	(kmem_cache_free)(c->kc_cache, p);
}

/* illumos reaps caches itself when memory runs low; nothing to do here. */
int
lkpi_kmem_cache_shrink(struct lkpi_kmem_cache *c)
{
	return (0);
}

/* kmalloc() and the caches keep no global state of their own. */
int
lkpi_slab_init(void)
{
	return (0);
}

void
lkpi_slab_fini(void)
{
}
