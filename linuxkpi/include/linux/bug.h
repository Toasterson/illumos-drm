/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's BUG() and WARN() (include/linux/bug.h, asm-generic/bug.h;
 * GPL-2.0; reimplemented). BUG() and a true BUG_ON() panic, as on Linux.
 * WARN*() log the condition, file and line with cmn_err(CE_WARN) and
 * return the condition; the *_ONCE forms log once per call site.
 */
#ifndef _LINUX_BUG_H
#define	_LINUX_BUG_H

#include <linux/compiler.h>
#include <linux/stringify.h>
#include <linux/build_bug.h>
#include <linux/once_lite.h>
#include <linux/instrumentation.h>

/* WARN()s fired since misc/linuxkpi loaded (lkpi_printf.c). */
extern volatile uint64_t lkpi_warnings;
extern void lkpi_warn(const char *file, int line, const char *fmt, ...)
    __printf(3, 4) __cold;
extern void lkpi_bug(const char *file, int line) __noreturn __cold;

#define	BUG()		lkpi_bug(__FILE__, __LINE__)
#define	BUG_ON(cond)	do { if (unlikely(cond)) BUG(); } while (0)
#define	__WARN()	lkpi_warn(__FILE__, __LINE__, NULL)
#define	__WARN_printf(taint, fmt, ...) \
	lkpi_warn(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define	WARN_ON(cond) ({ \
	bool __w = !!(cond); \
	if (unlikely(__w)) \
		lkpi_warn(__FILE__, __LINE__, "WARN_ON(%s)", #cond); \
	unlikely(__w); })
#define	WARN(cond, fmt, ...) ({ \
	bool __w = !!(cond); \
	if (unlikely(__w)) \
		lkpi_warn(__FILE__, __LINE__, fmt, ##__VA_ARGS__); \
	unlikely(__w); })
#define	WARN_ON_ONCE(cond) ({ \
	static volatile uint32_t __warned; \
	bool __w = !!(cond); \
	if (unlikely(__w) && __atomic_exchange_n(&__warned, 1, __ATOMIC_SEQ_CST) == 0) \
		lkpi_warn(__FILE__, __LINE__, "WARN_ON_ONCE(%s)", #cond); \
	unlikely(__w); })
#define	WARN_ONCE(cond, fmt, ...) ({ \
	static volatile uint32_t __warned; \
	bool __w = !!(cond); \
	if (unlikely(__w) && __atomic_exchange_n(&__warned, 1, __ATOMIC_SEQ_CST) == 0) \
		lkpi_warn(__FILE__, __LINE__, fmt, ##__VA_ARGS__); \
	unlikely(__w); })
#define	WARN_TAINT(cond, taint, fmt, ...)	WARN(cond, fmt, ##__VA_ARGS__)
#define	WARN_TAINT_ONCE(cond, taint, fmt, ...)	WARN_ONCE(cond, fmt, ##__VA_ARGS__)
#define	WARN_ON_SMP(cond)	WARN_ON(cond)
#define	BUILD_BUG_ON_ZERO_OR_NULL(e)	BUILD_BUG_ON_ZERO(e)
#define	MAYBE_BUILD_BUG_ON(cond)	((void)sizeof (char[1 - 2 * !!(cond)]))

enum bug_trap_type {
	BUG_TRAP_TYPE_NONE = 0,
	BUG_TRAP_TYPE_WARN = 1,
	BUG_TRAP_TYPE_BUG = 2
};

#define	TAINT_WARN		9
#define	TAINT_USER		6
#define	TAINT_MACHINE_CHECK	4
#define	LOCKDEP_STILL_OK	0
#define	LOCKDEP_NOW_UNRELIABLE	1
#define	add_taint(flag, lockdep)	((void)(flag), (void)(lockdep))
#define	test_taint(flag)		0

#endif
