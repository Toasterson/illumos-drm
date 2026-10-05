/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <linux/kernel.h> (GPL-2.0; reimplemented): the umbrella of
 * the basic helpers, plus might_sleep() and the hex helpers.
 * might_sleep() asserts thread context in DEBUG kernels only.
 */
#ifndef _LINUX_KERNEL_H
#define	_LINUX_KERNEL_H

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/compiler.h>
#include <linux/limits.h>
#include <linux/err.h>
#include <linux/bitops.h>
#include <linux/log2.h>
#include <linux/minmax.h>
#include <linux/math.h>
#include <linux/array_size.h>
#include <linux/align.h>
#include <linux/wordpart.h>
#include <linux/overflow.h>
#include <linux/util_macros.h>
#include <linux/printk.h>
#include <linux/bug.h>
#include <linux/kstrtox.h>
#include <linux/sprintf.h>
#include <linux/typecheck.h>
#include <linux/instruction_pointer.h>
#include <linux/panic.h>
#include <linux/static_call_types.h>
#include <linux/hex.h>
#include <linux/string.h>
#include <linux/jump_label.h>
#include <linux/error-injection.h>
#include <linux/dynamic_debug.h>
#include <linux/ratelimit.h>
#include <linux/sysctl.h>

/* drm_util.h has the same definition; i915 headers use it without it. */
#ifndef for_each_if
#define	for_each_if(condition)	if (!(condition)) {} else
#endif

#define	might_sleep()		ASSERT(!servicing_interrupt())
#define	might_sleep_if(c)	do { if (c) might_sleep(); } while (0)
#define	might_fault()		might_sleep()
#define	might_resched()		((void)0)
#define	cant_sleep()		((void)0)
#define	cant_migrate()		((void)0)
#define	non_block_start()	((void)0)
#define	non_block_end()		((void)0)
#define	sched_annotate_sleep()	((void)0)
#define	__might_sleep(f, l)	((void)0)
#define	trace_printk(fmt, ...)	pr_debug(fmt, ##__VA_ARGS__)
#define	STACK_MAGIC		0xdeadbeef
#define	_RET_IP_		((unsigned long)__builtin_return_address(0))
#define	_THIS_IP_		({ __label__ __here; __here: (unsigned long)&&__here; })

enum system_states {
	SYSTEM_BOOTING,
	SYSTEM_SCHEDULING,
	SYSTEM_FREEING_INITMEM,
	SYSTEM_RUNNING,
	SYSTEM_HALT,
	SYSTEM_POWER_OFF,
	SYSTEM_RESTART,
	SYSTEM_SUSPEND
};
#define	system_state	SYSTEM_RUNNING

extern int sscanf(const char *buf, const char *fmt, ...) __scanf(2, 3);
extern int vsscanf(const char *buf, const char *fmt, va_list ap)
    __scanf(2, 0);

#endif
