/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's compiler annotations used by the carried helpers (carried/):
 * branch hints, inlining and unused markers, READ_ONCE()/WRITE_ONCE().
 *
 * To be reconciled with R5c: a minimal stand-in written for R5e before
 * R5c's compatibility layer existed; R5c's header of the same name
 * replaces it.
 *
 * Origin: the names and meaning follow Linux v7.2's
 * include/linux/compiler.h and compiler_types.h (GPL-2.0). This file is a
 * reimplementation for GCC on illumos, not a copy (decision D7).
 */

#ifndef _LINUXKPI_LINUX_COMPILER_H
#define	_LINUXKPI_LINUX_COMPILER_H

/*
 * The system's annotations first (illumos' sys/ccompile.h, through the
 * facade's base types, defines __maybe_unused and others); each one below
 * is defined only where the system has none.
 */
#include <linux/types.h>

/*
 * Linux code writes typeof; the illumos-gate's kernel flags include
 * -fno-asm, which in GNU C mode removes the typeof (and asm) keywords and
 * leaves only __typeof__.
 */
#ifndef	typeof
#define	typeof	__typeof__
#endif

#ifndef	likely
#define	likely(x)	__builtin_expect(!!(x), 1)
#endif
#ifndef	unlikely
#define	unlikely(x)	__builtin_expect(!!(x), 0)
#endif

#ifndef	__always_inline
#define	__always_inline	inline __attribute__((__always_inline__))
#endif
#ifndef	noinline
#define	noinline	__attribute__((__noinline__))
#endif
#ifndef	__maybe_unused
#define	__maybe_unused	__attribute__((__unused__))
#endif
#ifndef	__must_check
#define	__must_check	__attribute__((__warn_unused_result__))
#endif

/* A compiler barrier: no memory access is moved across it. */
#ifndef	barrier
#define	barrier()	__asm__ __volatile__("" : : : "memory")
#endif

/*
 * One access of exactly the object's size, neither merged, split nor
 * repeated by the compiler (scalars of 1, 2, 4 or 8 bytes).
 */
#define	READ_ONCE(x)	(*(const volatile __typeof__(x) *)&(x))
#define	WRITE_ONCE(x, v) \
	do { *(volatile __typeof__(x) *)&(x) = (v); } while (0)

#endif	/* _LINUXKPI_LINUX_COMPILER_H */
