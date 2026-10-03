/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's string formatting (include/linux/sprintf.h, GPL-2.0;
 * reimplemented in src/lkpi_printf.c with Linux's %p extensions).
 * snprintf(), vsnprintf() and sprintf() are the layer's, not illumos'.
 */
#ifndef _LINUX_SPRINTF_H
#define	_LINUX_SPRINTF_H

#include <linux/types.h>
#include <linux/stdarg.h>

extern int lkpi_vsnprintf(char *buf, size_t size, const char *fmt,
    va_list ap) __printf(3, 0);
extern int lkpi_snprintf(char *buf, size_t size, const char *fmt, ...)
    __printf(3, 4);
extern int lkpi_vscnprintf(char *buf, size_t size, const char *fmt,
    va_list ap) __printf(3, 0);
extern int lkpi_scnprintf(char *buf, size_t size, const char *fmt, ...)
    __printf(3, 4);
extern int lkpi_sprintf(char *buf, const char *fmt, ...) __printf(2, 3);
extern char *kasprintf(gfp_t gfp, const char *fmt, ...) __printf(2, 3);
extern char *kvasprintf(gfp_t gfp, const char *fmt, va_list ap)
    __printf(2, 0);
extern const char *kvasprintf_const(gfp_t gfp, const char *fmt,
    va_list ap) __printf(2, 0);

#undef	snprintf
#undef	vsnprintf
#undef	sprintf
#define	vsnprintf(b, s, f, a)	lkpi_vsnprintf(b, s, f, a)
#define	snprintf(b, s, f, ...)	lkpi_snprintf(b, s, f, ##__VA_ARGS__)
#define	vscnprintf(b, s, f, a)	lkpi_vscnprintf(b, s, f, a)
#define	scnprintf(b, s, f, ...)	lkpi_scnprintf(b, s, f, ##__VA_ARGS__)
#define	sprintf(b, f, ...)	lkpi_sprintf(b, f, ##__VA_ARGS__)

#endif
