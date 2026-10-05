/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <asm/processor.h>, reduced to the boot CPU's description
 * (struct cpuinfo_x86 boot_cpu_data, <asm/cpu_device_id.h>), filled by
 * misc/linuxkpi when it loads (src/lkpi_cpu.c).
 */
#ifndef _ASM_PROCESSOR_H
#define	_ASM_PROCESSOR_H
#include <asm/cpu_device_id.h>
#include <asm/barrier.h>
#endif
