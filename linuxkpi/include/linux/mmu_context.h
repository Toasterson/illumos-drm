/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's use of another task's address space (kthread_use_mm). Not
 * supported: the calls do nothing.
 */
#ifndef _LINUX_MMU_CONTEXT_H
#define	_LINUX_MMU_CONTEXT_H
#include <linux/mm_types.h>
#define	kthread_use_mm(mm)	((void)(mm))
#define	kthread_unuse_mm(mm)	((void)(mm))
#endif
