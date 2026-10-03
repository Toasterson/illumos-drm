/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's memory barriers (arch/x86/include/asm/barrier.h, GPL-2.0;
 * reimplemented). x86 is TSO: the smp_ read/write barriers are compiler
 * barriers; full barriers are mfence (illumos membar_enter() is a locked
 * instruction with the same effect).
 */
#ifndef _ASM_BARRIER_H
#define	_ASM_BARRIER_H

#include <linux/compiler.h>

#define	mb()		__asm__ __volatile__("mfence" ::: "memory")
#define	rmb()		__asm__ __volatile__("lfence" ::: "memory")
#define	wmb()		__asm__ __volatile__("sfence" ::: "memory")
#define	dma_rmb()	barrier()
#define	dma_wmb()	barrier()
#define	dma_mb()	mb()
#define	smp_mb()	__asm__ __volatile__("lock; addl $0,-4(%%rsp)" ::: "memory", "cc")
#define	smp_rmb()	barrier()
#define	smp_wmb()	barrier()
#define	__smp_mb()	smp_mb()
#define	smp_store_mb(var, value)	do { WRITE_ONCE(var, value); smp_mb(); } while (0)
#define	smp_store_release(p, v)	do { barrier(); WRITE_ONCE(*(p), (v)); } while (0)
#define	smp_load_acquire(p)	({ typeof (*(p)) __v = READ_ONCE(*(p)); barrier(); __v; })
#define	smp_acquire__after_ctrl_dep()	smp_rmb()
#define	smp_mb__before_atomic()	barrier()
#define	smp_mb__after_atomic()	barrier()
#define	smp_mb__after_spinlock()	smp_mb()
#define	smp_mb__after_unlock_lock()	smp_mb()
#define	virt_mb()	smp_mb()
#define	virt_rmb()	smp_rmb()
#define	virt_wmb()	smp_wmb()
#define	smp_cond_load_relaxed(ptr, cond_expr) ({ \
	typeof (ptr) __PTR = (ptr); \
	typeof (*(ptr)) VAL; \
	for (;;) { \
		VAL = READ_ONCE(*__PTR); \
		if (cond_expr) \
			break; \
		SMT_PAUSE(); \
	} \
	VAL; })
#define	smp_cond_load_acquire(ptr, cond_expr) ({ \
	typeof (*(ptr)) __v = smp_cond_load_relaxed(ptr, cond_expr); \
	barrier(); __v; })
#define	cpu_relax()	SMT_PAUSE()
#ifndef	SMT_PAUSE
#define	SMT_PAUSE()	__asm__ __volatile__("pause" ::: "memory")
#endif

#endif
