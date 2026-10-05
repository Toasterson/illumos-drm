/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's atomic_t, atomic64_t and atomic_long_t operations and the
 * generic cmpxchg()/xchg() (include/linux/atomic/*, arch/x86/include/asm/
 * atomic*.h, cmpxchg.h; GPL-2.0; reimplemented over gcc's __atomic
 * builtins, which are lock-prefixed instructions on x86). Every
 * value-returning operation is fully ordered, as on Linux; the _relaxed,
 * _acquire and _release forms are the same operations (x86 orders them
 * at least as strongly).
 */
#ifndef _LINUX_ATOMIC_H
#define	_LINUX_ATOMIC_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <asm/barrier.h>

#define	__LKPI_SC	__ATOMIC_SEQ_CST

#define	cmpxchg(ptr, o, n) ({ \
	typeof (*(ptr)) __o = (o); \
	(void)__atomic_compare_exchange_n((ptr), &__o, (n), false, \
	    __LKPI_SC, __LKPI_SC); \
	__o; })
#define	cmpxchg_relaxed(p, o, n)	cmpxchg(p, o, n)
#define	cmpxchg_acquire(p, o, n)	cmpxchg(p, o, n)
#define	cmpxchg_release(p, o, n)	cmpxchg(p, o, n)
#define	cmpxchg64(p, o, n)		cmpxchg(p, o, n)
#define	cmpxchg64_relaxed(p, o, n)	cmpxchg(p, o, n)
#define	cmpxchg_local(p, o, n)		cmpxchg(p, o, n)
#define	try_cmpxchg(ptr, po, n) \
	__atomic_compare_exchange_n((ptr), (po), (n), false, __LKPI_SC, __LKPI_SC)
#define	try_cmpxchg_relaxed(p, po, n)	try_cmpxchg(p, po, n)
#define	try_cmpxchg_acquire(p, po, n)	try_cmpxchg(p, po, n)
#define	try_cmpxchg_release(p, po, n)	try_cmpxchg(p, po, n)
#define	try_cmpxchg64(p, po, n)		try_cmpxchg(p, po, n)
#define	xchg(ptr, v)			__atomic_exchange_n((ptr), (v), __LKPI_SC)
#define	xchg_relaxed(p, v)		xchg(p, v)
#define	xchg_acquire(p, v)		xchg(p, v)
#define	xchg_release(p, v)		xchg(p, v)

/*
 * One family per counter type: atomic (int), atomic64 (long long),
 * atomic_long (long).
 */
#define	__LKPI_ATOMIC_FAMILY(pfx, T, V) \
static inline V pfx##_read(const T *v) { return (__atomic_load_n(&v->counter, __ATOMIC_RELAXED)); } \
static inline V pfx##_read_acquire(const T *v) { return (__atomic_load_n(&v->counter, __ATOMIC_ACQUIRE)); } \
static inline void pfx##_set(T *v, V i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELAXED); } \
static inline void pfx##_set_release(T *v, V i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELEASE); } \
static inline void pfx##_add(V i, T *v) { (void)__atomic_fetch_add(&v->counter, i, __LKPI_SC); } \
static inline void pfx##_sub(V i, T *v) { (void)__atomic_fetch_sub(&v->counter, i, __LKPI_SC); } \
static inline void pfx##_inc(T *v) { pfx##_add(1, v); } \
static inline void pfx##_dec(T *v) { pfx##_sub(1, v); } \
static inline void pfx##_or(V i, T *v) { (void)__atomic_fetch_or(&v->counter, i, __LKPI_SC); } \
static inline void pfx##_and(V i, T *v) { (void)__atomic_fetch_and(&v->counter, i, __LKPI_SC); } \
static inline void pfx##_andnot(V i, T *v) { (void)__atomic_fetch_and(&v->counter, ~i, __LKPI_SC); } \
static inline void pfx##_xor(V i, T *v) { (void)__atomic_fetch_xor(&v->counter, i, __LKPI_SC); } \
static inline V pfx##_fetch_add(V i, T *v) { return (__atomic_fetch_add(&v->counter, i, __LKPI_SC)); } \
static inline V pfx##_fetch_sub(V i, T *v) { return (__atomic_fetch_sub(&v->counter, i, __LKPI_SC)); } \
static inline V pfx##_fetch_or(V i, T *v) { return (__atomic_fetch_or(&v->counter, i, __LKPI_SC)); } \
static inline V pfx##_fetch_and(V i, T *v) { return (__atomic_fetch_and(&v->counter, i, __LKPI_SC)); } \
static inline V pfx##_fetch_andnot(V i, T *v) { return (__atomic_fetch_and(&v->counter, ~i, __LKPI_SC)); } \
static inline V pfx##_fetch_xor(V i, T *v) { return (__atomic_fetch_xor(&v->counter, i, __LKPI_SC)); } \
static inline V pfx##_fetch_inc(T *v) { return (pfx##_fetch_add(1, v)); } \
static inline V pfx##_fetch_dec(T *v) { return (pfx##_fetch_sub(1, v)); } \
static inline V pfx##_add_return(V i, T *v) { return (__atomic_add_fetch(&v->counter, i, __LKPI_SC)); } \
static inline V pfx##_sub_return(V i, T *v) { return (__atomic_sub_fetch(&v->counter, i, __LKPI_SC)); } \
static inline V pfx##_inc_return(T *v) { return (pfx##_add_return(1, v)); } \
static inline V pfx##_dec_return(T *v) { return (pfx##_sub_return(1, v)); } \
static inline bool pfx##_dec_and_test(T *v) { return (pfx##_dec_return(v) == 0); } \
static inline bool pfx##_inc_and_test(T *v) { return (pfx##_inc_return(v) == 0); } \
static inline bool pfx##_sub_and_test(V i, T *v) { return (pfx##_sub_return(i, v) == 0); } \
static inline bool pfx##_add_negative(V i, T *v) { return (pfx##_add_return(i, v) < 0); } \
static inline V pfx##_xchg(T *v, V n) { return (__atomic_exchange_n(&v->counter, n, __LKPI_SC)); } \
static inline V pfx##_cmpxchg(T *v, V o, V n) { \
	(void)__atomic_compare_exchange_n(&v->counter, &o, n, false, __LKPI_SC, __LKPI_SC); \
	return (o); } \
static inline bool pfx##_try_cmpxchg(T *v, V *o, V n) { \
	return (__atomic_compare_exchange_n(&v->counter, o, n, false, __LKPI_SC, __LKPI_SC)); } \
static inline V pfx##_fetch_add_unless(T *v, V a, V u) { \
	V c = pfx##_read(v); \
	do { if (c == u) break; } while (!pfx##_try_cmpxchg(v, &c, c + a)); \
	return (c); } \
static inline bool pfx##_add_unless(T *v, V a, V u) { return (pfx##_fetch_add_unless(v, a, u) != u); } \
static inline bool pfx##_inc_not_zero(T *v) { return (pfx##_add_unless(v, 1, 0)); } \
static inline V pfx##_dec_if_positive(T *v) { \
	V c = pfx##_read(v), d; \
	do { d = c - 1; if (d < 0) break; } while (!pfx##_try_cmpxchg(v, &c, d)); \
	return (d); } \
static inline bool pfx##_inc_unless_negative(T *v) { \
	V c = pfx##_read(v); \
	do { if (c < 0) return (false); } while (!pfx##_try_cmpxchg(v, &c, c + 1)); \
	return (true); } \
static inline bool pfx##_dec_unless_positive(T *v) { \
	V c = pfx##_read(v); \
	do { if (c > 0) return (false); } while (!pfx##_try_cmpxchg(v, &c, c - 1)); \
	return (true); }

__LKPI_ATOMIC_FAMILY(atomic, atomic_t, int)
__LKPI_ATOMIC_FAMILY(atomic64, atomic64_t, long long)
__LKPI_ATOMIC_FAMILY(atomic_long, atomic_long_t, long)

#define	ATOMIC_INIT(i)		{ (i) }
#define	ATOMIC64_INIT(i)	{ (i) }
#define	ATOMIC_LONG_INIT(i)	{ (i) }

#define	atomic_add_return_relaxed(i, v)	atomic_add_return(i, v)
#define	atomic_sub_return_relaxed(i, v)	atomic_sub_return(i, v)
#define	atomic_inc_return_relaxed(v)	atomic_inc_return(v)
#define	atomic_dec_return_relaxed(v)	atomic_dec_return(v)
#define	atomic_fetch_inc_relaxed(v)	atomic_fetch_inc(v)
#define	atomic_fetch_add_relaxed(i, v)	atomic_fetch_add(i, v)
#define	atomic_fetch_or_relaxed(i, v)	atomic_fetch_or(i, v)
#define	atomic_xchg_relaxed(v, n)	atomic_xchg(v, n)
#define	atomic_cmpxchg_relaxed(v, o, n)	atomic_cmpxchg(v, o, n)
#define	atomic_try_cmpxchg_relaxed(v, o, n)	atomic_try_cmpxchg(v, o, n)
#define	atomic_try_cmpxchg_acquire(v, o, n)	atomic_try_cmpxchg(v, o, n)
#define	atomic_try_cmpxchg_release(v, o, n)	atomic_try_cmpxchg(v, o, n)
#define	atomic_fetch_add_release(i, v)	atomic_fetch_add(i, v)
#define	atomic_fetch_sub_release(i, v)	atomic_fetch_sub(i, v)
#define	atomic_inc_return_acquire(v)	atomic_inc_return(v)
#define	atomic_dec_return_release(v)	atomic_dec_return(v)
#define	atomic64_add_return_relaxed(i, v)	atomic64_add_return(i, v)
#define	atomic64_fetch_add_relaxed(i, v)	atomic64_fetch_add(i, v)
#define	atomic64_cmpxchg_relaxed(v, o, n)	atomic64_cmpxchg(v, o, n)
#define	atomic64_try_cmpxchg_relaxed(v, o, n)	atomic64_try_cmpxchg(v, o, n)
#define	atomic64_xchg_relaxed(v, n)	atomic64_xchg(v, n)
#define	atomic_long_cmpxchg_relaxed(v, o, n)	atomic_long_cmpxchg(v, o, n)
#define	atomic_long_try_cmpxchg_relaxed(v, o, n)	atomic_long_try_cmpxchg(v, o, n)
#define	atomic_long_xchg_relaxed(v, n)	atomic_long_xchg(v, n)
#define	atomic_long_fetch_add_relaxed(i, v)	atomic_long_fetch_add(i, v)
#define	atomic_cond_read_relaxed(v, c)	smp_cond_load_relaxed(&(v)->counter, (c))
#define	atomic_cond_read_acquire(v, c)	smp_cond_load_acquire(&(v)->counter, (c))
#define	smp_mb__before_atomic_dec()	smp_mb__before_atomic()

#endif
