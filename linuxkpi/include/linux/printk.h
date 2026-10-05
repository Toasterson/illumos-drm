/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's printk() and pr_*() (include/linux/printk.h, GPL-2.0;
 * reimplemented). Messages go to cmn_err(9F) after formatting with the
 * layer's vsnprintf(), which understands Linux's %p extensions (%pa,
 * %pad, %pe, %ph, %pV, %pS, %pR, %pU, %p4cc). Levels: EMERG..WARNING are
 * CE_WARN, NOTICE is CE_NOTE, INFO and DEBUG go to the system log only
 * ("!" prefix); pr_cont() is CE_CONT. pr_debug() prints only when the
 * tunable lkpi_debug is non-zero (set linuxkpi:lkpi_debug = 1 in
 * /etc/system, or with mdb -kw).
 */
#ifndef _LINUX_PRINTK_H
#define	_LINUX_PRINTK_H

#include <linux/types.h>
#include <linux/stdarg.h>
#include <linux/kern_levels.h>
#include <linux/ratelimit_types.h>
#include <linux/once_lite.h>

extern volatile int lkpi_debug;

extern int lkpi_printk(const char *fmt, ...) __printf(1, 2);
extern int lkpi_vprintk(const char *fmt, va_list ap) __printf(1, 0);
#define	printk(fmt, ...)	lkpi_printk(fmt, ##__VA_ARGS__)
#define	vprintk(fmt, ap)	lkpi_vprintk(fmt, ap)
#define	printk_deferred(fmt, ...)	lkpi_printk(fmt, ##__VA_ARGS__)
#define	_printk(fmt, ...)	lkpi_printk(fmt, ##__VA_ARGS__)

#ifndef	pr_fmt
#define	pr_fmt(fmt)	fmt
#endif

#define	no_printk(fmt, ...) ({ \
	if (0) \
		lkpi_printk(fmt, ##__VA_ARGS__); \
	0; })

#define	pr_emerg(fmt, ...)	printk(KERN_EMERG pr_fmt(fmt), ##__VA_ARGS__)
#define	pr_alert(fmt, ...)	printk(KERN_ALERT pr_fmt(fmt), ##__VA_ARGS__)
#define	pr_crit(fmt, ...)	printk(KERN_CRIT pr_fmt(fmt), ##__VA_ARGS__)
#define	pr_err(fmt, ...)	printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define	pr_warn(fmt, ...)	printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define	pr_warning(fmt, ...)	pr_warn(fmt, ##__VA_ARGS__)
#define	pr_notice(fmt, ...)	printk(KERN_NOTICE pr_fmt(fmt), ##__VA_ARGS__)
#define	pr_info(fmt, ...)	printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define	pr_cont(fmt, ...)	printk(KERN_CONT fmt, ##__VA_ARGS__)
#define	pr_devel(fmt, ...)	no_printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#define	pr_debug(fmt, ...) do { \
	if (lkpi_debug) \
		printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__); \
	} while (0)

#define	printk_once(fmt, ...)	DO_ONCE_LITE(printk, fmt, ##__VA_ARGS__)
#define	pr_emerg_once(fmt, ...)	DO_ONCE_LITE(pr_emerg, fmt, ##__VA_ARGS__)
#define	pr_err_once(fmt, ...)	DO_ONCE_LITE(pr_err, fmt, ##__VA_ARGS__)
#define	pr_warn_once(fmt, ...)	DO_ONCE_LITE(pr_warn, fmt, ##__VA_ARGS__)
#define	pr_notice_once(fmt, ...)	DO_ONCE_LITE(pr_notice, fmt, ##__VA_ARGS__)
#define	pr_info_once(fmt, ...)	DO_ONCE_LITE(pr_info, fmt, ##__VA_ARGS__)
#define	pr_debug_once(fmt, ...)	DO_ONCE_LITE(pr_debug, fmt, ##__VA_ARGS__)

/* Rate limiting is not implemented: every message is printed. */
#define	printk_ratelimit()		1
#define	printk_ratelimited(fmt, ...)	printk(fmt, ##__VA_ARGS__)
#define	pr_err_ratelimited(fmt, ...)	pr_err(fmt, ##__VA_ARGS__)
#define	pr_warn_ratelimited(fmt, ...)	pr_warn(fmt, ##__VA_ARGS__)
#define	pr_notice_ratelimited(fmt, ...)	pr_notice(fmt, ##__VA_ARGS__)
#define	pr_info_ratelimited(fmt, ...)	pr_info(fmt, ##__VA_ARGS__)
#define	pr_debug_ratelimited(fmt, ...)	pr_debug(fmt, ##__VA_ARGS__)

enum {
	DUMP_PREFIX_NONE,
	DUMP_PREFIX_ADDRESS,
	DUMP_PREFIX_OFFSET
};

extern void print_hex_dump(const char *level, const char *prefix_str,
    int prefix_type, int rowsize, int groupsize, const void *buf, size_t len,
    bool ascii);
extern int hex_dump_to_buffer(const void *buf, size_t len, int rowsize,
    int groupsize, char *linebuf, size_t linebuflen, bool ascii);
#define	print_hex_dump_debug(prefix, type, rs, gs, buf, len, ascii) \
	do { if (lkpi_debug) print_hex_dump(KERN_DEBUG, prefix, type, rs, gs, \
	    buf, len, ascii); } while (0)
#define	print_hex_dump_bytes(prefix, type, buf, len) \
	print_hex_dump_debug(prefix, type, 16, 1, buf, len, true)

/* No console lock or log buffer to flush on illumos. */
#define	console_lock()		((void)0)
#define	console_unlock()	((void)0)
#define	console_trylock()	1
#define	oops_in_progress	0
#define	dump_stack()		((void)0)

#endif
