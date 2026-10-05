/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Host-test override of the facade's drm/drm_print.h: struct drm_printer
 * with the facade's fields and drm_printf()/drm_puts() over vprintf, for
 * the carried helpers' printing functions (drm_mm_print()) in a userland
 * test program. Test scaffolding only; never in a module.
 */

#ifndef _DRM_PRINT_H
#define	_DRM_PRINT_H

#include <stdarg.h>
#include <stdio.h>
#include <linux/types.h>

struct va_format {
	const char *fmt;
	va_list *va;
};

struct drm_printer {
	void (*printfn)(struct drm_printer *p, struct va_format *vaf);
	void (*puts)(struct drm_printer *p, const char *str);
	void *arg;
	const void *origin;
	const char *prefix;
	struct {
		unsigned int series;
		unsigned int counter;
	} line;
	int category;
};

static inline void
drm_vprintf(struct drm_printer *p, const char *fmt, va_list *va)
{
	struct va_format vaf;

	vaf.fmt = fmt;
	vaf.va = va;
	p->printfn(p, &vaf);
}

static inline void
drm_printf(struct drm_printer *p, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	drm_vprintf(p, fmt, &ap);
	va_end(ap);
}

static inline void
drm_puts(struct drm_printer *p, const char *str)
{
	if (p->puts != NULL)
		p->puts(p, str);
	else
		drm_printf(p, "%s", str);
}

#define	DRM_ERROR(fmt, ...)	fprintf(stderr, "[drm] *ERROR* " fmt, ##__VA_ARGS__)

#endif	/* _DRM_PRINT_H */
