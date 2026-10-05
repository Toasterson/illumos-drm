/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/drm_print.h>: the facade's printers and logging macros
 * (<drm/drm_print.h> of the facade, include_next) plus what the facade
 * left to the compatibility layer: the drm_WARN family, seq_file and
 * line printers, drm_print_bits() and drm_print_hex_dump().
 */
#ifndef _LKPI_DRM_PRINT_H
#define	_LKPI_DRM_PRINT_H

#include <linux/device.h>
#include <linux/seq_file.h>
#include <linux/printk.h>
#include <linux/bug.h>
#include <linux/string_choices.h>
/*
 * Messages are formatted by the layer (Linux's %p extensions, %pe, %pa,
 * %ph, %pV, and hashed plain %p), and the core gets the text: the facade's
 * logging entry point and core printers are the layer's under these names
 * (src/lkpi_drm.c).
 */
#define	drm_illumos_vprintk	lkpi_drm_vprintk
#define	__drm_printfn_info	lkpi_drm_printfn_info
#define	__drm_printfn_dbg	lkpi_drm_printfn_dbg
#define	__drm_printfn_err	lkpi_drm_printfn_err
/* The facade's drm_info_printer() takes the node; Linux's the device. */
#define	drm_info_printer	lkpi_facade_drm_info_printer
#include_next <drm/drm_print.h>
#undef	drm_info_printer

/* Linux's drm_info_printer(): KERN_INFO messages on the layer device. */
static inline struct drm_printer
drm_info_printer(struct device *dev)
{
	return (lkpi_facade_drm_info_printer(dev != NULL ?
	    dev->lkpi_dip : NULL));
}

extern void __drm_printfn_seq_file(struct drm_printer *p, struct va_format *vaf);
extern void __drm_puts_seq_file(struct drm_printer *p, const char *str);
extern void __drm_printfn_line(struct drm_printer *p, struct va_format *vaf);
extern void drm_print_bits(struct drm_printer *p, unsigned long value,
    const char * const bits[], unsigned int nbits);
extern void drm_print_hex_dump(struct drm_printer *p, const char *prefix,
    const u8 *buf, size_t len);
extern void drm_puts_lkpi(struct drm_printer *p, const char *str);

static inline struct drm_printer
drm_seq_file_printer(struct seq_file *f)
{
	struct drm_printer p = { 0 };

	p.printfn = __drm_printfn_seq_file;
	p.puts = __drm_puts_seq_file;
	p.arg = f;
	return (p);
}

static inline struct drm_printer
drm_line_printer(struct drm_printer *p, const char *prefix,
    unsigned int series)
{
	struct drm_printer lp = { 0 };

	lp.printfn = __drm_printfn_line;
	lp.arg = p;
	lp.prefix = prefix;
	lp.line.series = series;
	return (lp);
}

static inline struct drm_printer
drm_debug_printer(const char *prefix)
{
	return (drm_dbg_printer(NULL, DRM_UT_DRIVER, prefix));
}

#define	__drm_lkpi_devname(drm) \
	((drm) != NULL && (drm)->dev != NULL ? dev_name((drm)->dev) : "drm")
#define	drm_WARN(drm, condition, format, arg...) \
	WARN(condition, "%s: " format, __drm_lkpi_devname(drm), ##arg)
#define	drm_WARN_ONCE(drm, condition, format, arg...) \
	WARN_ONCE(condition, "%s: " format, __drm_lkpi_devname(drm), ##arg)
#define	drm_WARN_ON(drm, x) \
	drm_WARN((drm), (x), "%s", "drm_WARN_ON(" __stringify(x) ")")
#define	drm_WARN_ON_ONCE(drm, x) \
	drm_WARN_ONCE((drm), (x), "%s", "drm_WARN_ON_ONCE(" __stringify(x) ")")
#define	drm_dbg_kms_once(drm, fmt, ...)	drm_dbg_kms(drm, fmt, ##__VA_ARGS__)
#define	drm_dbg_printer_lkpi(drm, cat, prefix)	drm_dbg_printer(drm, cat, prefix)
#define	DRM_DEBUG_KMS_RATELIMITED(fmt, ...)	DRM_DEBUG_KMS(fmt, ##__VA_ARGS__)
#define	drm_debug_enabled_instrumented(c)	drm_debug_enabled(c)

#endif
