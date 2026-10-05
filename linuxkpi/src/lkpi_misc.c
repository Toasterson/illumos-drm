/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Small Linux library functions without state: list_sort() (a stable,
 * iterative bottom-up merge sort of the list: O(n log n) comparisons,
 * no recursion, no allocation), sort()/sort_r() (an in-place heap sort),
 * bsearch(), the refcount_t slow paths, random numbers over
 * random_get_pseudo_bytes() (<sys/random.h>) and uuid_is_valid().
 */

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/sort.h>
#include <linux/bsearch.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/uuid.h>
#include <linux/ctype.h>
#include <linux/bug.h>
#include <sys/random.h>

/* --- list_sort() ---------------------------------------------------------- */

/*
 * Runs of length `run` (1, 2, 4, ...) of the NULL-terminated singly linked
 * list (through ->next) are merged pairwise until one pass merges only
 * once. A merge takes from the left run while cmp(a, b) <= 0, so equal
 * elements keep their order. The ->prev links are rebuilt at the end.
 */
void
list_sort(void *priv, struct list_head *head,
    int (*cmp)(void *priv, const struct list_head *a,
    const struct list_head *b))
{
	struct list_head *list, *p, *q, *e, *tail, *prev;
	size_t run = 1, psize, qsize, i, merges;

	if (list_empty(head) || head->next == head->prev)
		return;
	list = head->next;
	head->prev->next = NULL;

	for (;;) {
		p = list;
		list = NULL;
		tail = NULL;
		merges = 0;
		while (p != NULL) {
			merges++;
			q = p;
			psize = 0;
			for (i = 0; i < run && q != NULL; i++) {
				psize++;
				q = q->next;
			}
			qsize = run;
			while (psize > 0 || (qsize > 0 && q != NULL)) {
				if (psize == 0) {
					e = q;
					q = q->next;
					qsize--;
				} else if (qsize == 0 || q == NULL ||
				    cmp(priv, p, q) <= 0) {
					e = p;
					p = p->next;
					psize--;
				} else {
					e = q;
					q = q->next;
					qsize--;
				}
				if (tail != NULL)
					tail->next = e;
				else
					list = e;
				tail = e;
			}
			p = q;
		}
		tail->next = NULL;
		if (merges <= 1)
			break;
		run *= 2;
	}

	prev = head;
	for (e = list; e != NULL; e = e->next) {
		e->prev = prev;
		prev->next = e;
		prev = e;
	}
	prev->next = head;
	head->prev = prev;
}

/* --- sort(), sort_r() ---------------------------------------------------- */

struct lkpi_sort_ops {
	cmp_func_t cmp;
	cmp_r_func_t cmp_r;
	swap_func_t sw;
	swap_r_func_t sw_r;
	const void *priv;
	size_t size;
};

static void
lkpi_swap_bytes(void *a, void *b, size_t size)
{
	if (((uintptr_t)a | (uintptr_t)b | size) % sizeof (u64) == 0) {
		u64 *x = a, *y = b, t;

		for (; size != 0; size -= sizeof (u64)) {
			t = *x;
			*x++ = *y;
			*y++ = t;
		}
	} else if (((uintptr_t)a | (uintptr_t)b | size) % sizeof (u32) == 0) {
		u32 *x = a, *y = b, t;

		for (; size != 0; size -= sizeof (u32)) {
			t = *x;
			*x++ = *y;
			*y++ = t;
		}
	} else {
		u8 *x = a, *y = b, t;

		for (; size != 0; size--) {
			t = *x;
			*x++ = *y;
			*y++ = t;
		}
	}
}

static inline int
lkpi_sort_cmp(const struct lkpi_sort_ops *o, const void *a, const void *b)
{
	if (o->cmp_r != NULL)
		return (o->cmp_r(a, b, o->priv));
	return (o->cmp(a, b));
}

static inline void
lkpi_sort_swap(const struct lkpi_sort_ops *o, void *a, void *b)
{
	if (o->sw_r != NULL)
		o->sw_r(a, b, (int)o->size, o->priv);
	else if (o->sw != NULL)
		o->sw(a, b, (int)o->size);
	else
		lkpi_swap_bytes(a, b, o->size);
}

/* Moves element i down the max-heap of the first n elements. */
static void
lkpi_sift_down(char *base, size_t i, size_t n, const struct lkpi_sort_ops *o)
{
	size_t c;

	while ((c = 2 * i + 1) < n) {
		if (c + 1 < n && lkpi_sort_cmp(o, base + c * o->size,
		    base + (c + 1) * o->size) < 0)
			c++;
		if (lkpi_sort_cmp(o, base + i * o->size,
		    base + c * o->size) >= 0)
			return;
		lkpi_sort_swap(o, base + i * o->size, base + c * o->size);
		i = c;
	}
}

static void
lkpi_heapsort(void *base, size_t num, const struct lkpi_sort_ops *o)
{
	char *b = base;
	size_t i;

	if (num < 2 || o->size == 0)
		return;
	for (i = num / 2; i-- > 0; )
		lkpi_sift_down(b, i, num, o);
	for (i = num - 1; i > 0; i--) {
		lkpi_sort_swap(o, b, b + i * o->size);
		lkpi_sift_down(b, 0, i, o);
	}
}

void
sort(void *base, size_t num, size_t size, cmp_func_t cmp, swap_func_t swap)
{
	struct lkpi_sort_ops o = { .cmp = cmp, .sw = swap, .size = size };

	lkpi_heapsort(base, num, &o);
}

void
sort_r(void *base, size_t num, size_t size, cmp_r_func_t cmp,
    swap_r_func_t swap, const void *priv)
{
	struct lkpi_sort_ops o = { .cmp_r = cmp, .sw_r = swap,
	    .priv = priv, .size = size };

	lkpi_heapsort(base, num, &o);
}

/* --- bsearch() ----------------------------------------------------------- */

void *
lkpi_bsearch(const void *key, const void *base, size_t num, size_t size,
    int (*cmp)(const void *key, const void *elt))
{
	const char *b = base;
	size_t lo = 0, hi = num;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int r = cmp(key, b + mid * size);

		if (r == 0)
			return ((void *)(b + mid * size));
		if (r < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return (NULL);
}

/* --- refcount_t ---------------------------------------------------------- */

/*
 * A misused count is pinned at REFCOUNT_SATURATED: the object leaks
 * rather than being freed twice or used after free.
 */
void
refcount_warn_saturate(refcount_t *r, enum refcount_saturation_type t)
{
	refcount_set(r, REFCOUNT_SATURATED);
	switch (t) {
	case REFCOUNT_ADD_NOT_ZERO_OVF:
	case REFCOUNT_ADD_OVF:
		WARN_ONCE(1, "refcount_t %p: saturated; leaking memory", r);
		break;
	case REFCOUNT_ADD_UAF:
		WARN_ONCE(1, "refcount_t %p: addition on 0; use-after-free",
		    r);
		break;
	case REFCOUNT_SUB_UAF:
		WARN_ONCE(1, "refcount_t %p: underflow; use-after-free", r);
		break;
	case REFCOUNT_DEC_LEAK:
		WARN_ONCE(1, "refcount_t %p: decrement hit 0; leaking memory",
		    r);
		break;
	default:
		WARN_ONCE(1, "refcount_t %p: unknown saturation event %d", r,
		    (int)t);
		break;
	}
}

/* Drops a reference; on the last one returns true with the lock held. */
bool
refcount_dec_and_mutex_lock(refcount_t *r, struct lx_mutex *lock)
{
	if (refcount_dec_not_one(r))
		return (false);
	mutex_lock(lock);
	if (!refcount_dec_and_test(r)) {
		mutex_unlock(lock);
		return (false);
	}
	return (true);
}

bool
refcount_dec_and_lock(refcount_t *r, struct spinlock *lock)
{
	if (refcount_dec_not_one(r))
		return (false);
	spin_lock(lock);
	if (!refcount_dec_and_test(r)) {
		spin_unlock(lock);
		return (false);
	}
	return (true);
}

bool
refcount_dec_and_lock_irqsave(refcount_t *r, struct spinlock *lock,
    unsigned long *flags)
{
	if (refcount_dec_not_one(r))
		return (false);
	spin_lock_irqsave(lock, *flags);
	if (!refcount_dec_and_test(r)) {
		spin_unlock_irqrestore(lock, *flags);
		return (false);
	}
	return (true);
}

/* --- random numbers ------------------------------------------------------ */

/*
 * random_get_pseudo_bytes() only fails without a usable provider; then
 * the bytes come from a splitmix64 sequence seeded from gethrtime(9F)
 * (not cryptographic, as get_random_* need not be for the drivers).
 */
void
get_random_bytes(void *buf, size_t len)
{
	static volatile uint64_t lkpi_rnd_state;
	u8 *p = buf;

	if (len == 0 || random_get_pseudo_bytes(p, len) == 0)
		return;
	while (len > 0) {
		uint64_t z = __atomic_add_fetch(&lkpi_rnd_state,
		    0x9e3779b97f4a7c15ULL + (uint64_t)gethrtime(),
		    __ATOMIC_RELAXED);
		size_t i;

		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
		z ^= z >> 31;
		for (i = 0; i < sizeof (z) && len > 0; i++, len--)
			*p++ = (u8)(z >> (i * 8));
	}
}

u32
get_random_u32(void)
{
	u32 v;

	get_random_bytes(&v, sizeof (v));
	return (v);
}

u64
get_random_u64(void)
{
	u64 v;

	get_random_bytes(&v, sizeof (v));
	return (v);
}

/* --- uuid ---------------------------------------------------------------- */

/* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" with hex digits. */
bool
uuid_is_valid(const char *uuid)
{
	int i;

	for (i = 0; i < UUID_STRING_LEN; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (uuid[i] != '-')
				return (false);
		} else if (!isxdigit(uuid[i])) {
			return (false);
		}
	}
	return (true);
}
