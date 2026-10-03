/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's kernel FPU sections over illumos kernel_fpu_begin/end
 * (<sys/kfpu.h>, src/lkpi_cpu.c): the SSE state is saved, so code inside
 * may use vector instructions (in inline assembly; the layer and drivers
 * are compiled with -mno-sse).
 */
#ifndef _ASM_FPU_API_H
#define	_ASM_FPU_API_H
extern void lkpi_kernel_fpu_begin(void);
extern void lkpi_kernel_fpu_end(void);
#define	kernel_fpu_begin()	lkpi_kernel_fpu_begin()
#define	kernel_fpu_end()	lkpi_kernel_fpu_end()
#define	irq_fpu_usable()	true
#endif
