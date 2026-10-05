/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's stack depot. Not supported (debugging): no stacks are saved;
 * handles are 0.
 */
#ifndef _LINUX_STACKDEPOT_H
#define	_LINUX_STACKDEPOT_H
#include <linux/types.h>
typedef u32 depot_stack_handle_t;
#define	stack_depot_save(e, n, g)		((depot_stack_handle_t)0)
#define	stack_depot_fetch(h, e)			0U
#define	stack_depot_snprint(h, b, s, sp)	0
#define	stack_depot_print(h)			((void)(h))
#define	stack_depot_init()			((void)0)
#define	stack_trace_save(e, n, s)		0U
#define	stack_trace_snprint(b, s, e, n, sp)	0
#endif
