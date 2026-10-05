/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Logging for drivers on the illumos-drm facade.
 *
 * Origin: the names, structures and semantics follow Linux v7.2's
 * include/drm/drm_print.h (MIT): struct drm_printer and its printers,
 * drm_printf(), the drm_err()/drm_warn()/drm_info()/drm_dbg_*() family on
 * a struct drm_device, the DRM_DEV_* and DRM_* macros, and the debug
 * categories. Everything ends in cmn_err(9F): through dev_err() on the
 * device's node when there is one, so messages carry the driver name and
 * instance. MIT-licensed.
 *
 * Levels: errors and warnings go to the console and the system log
 * (CE_WARN), notices to both (CE_NOTE), information and debug messages to
 * the system log only (CE_NOTE with "!"). A message longer than 511 bytes
 * is cut there and ends in "...".
 *
 * Debug messages: a debug message of category C is printed only while
 * bit C (1 << C) of drm_debug, a variable of misc/drm_core, is set; 0 by
 * default. Set it at boot in /etc/system:
 *	set drm_core:drm_debug = 0x6
 * (DRM_UT_DRIVER and DRM_UT_KMS), or on a running system:
 *	echo 'drm_debug/W 0x6' | mdb -kw
 * The bits are Linux's (drm.debug=0x6 on Linux selects the same).
 *
 * Not provided: drm_WARN*() (they need the compatibility layer's WARN),
 * drm_line_printer(), drm_coredump_printer(), drm_seq_file_printer() and
 * drm_print_bits(); the *_ratelimited variants print every message.
 */

#ifndef _DRM_PRINT_H
#define	_DRM_PRINT_H

#include <drm/drm_facade_types.h>
#include <drm/drm_device.h>
#include <sys/cmn_err.h>
#include <sys/varargs.h>
#include <sys/atomic.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* Linux's debug categories; the bit of category C is (1 << C). */
enum drm_debug_category {
	DRM_UT_CORE = 0,
	DRM_UT_DRIVER = 1,
	DRM_UT_KMS = 2,
	DRM_UT_PRIME = 3,
	DRM_UT_ATOMIC = 4,
	DRM_UT_VBL = 5,
	DRM_UT_STATE = 6,
	DRM_UT_LEASE = 7,
	DRM_UT_DP = 8,
	DRM_UT_DRMRES = 9
};

/* The enabled debug categories (one bit each); 0 by default. */
extern volatile uint32_t drm_debug;

static inline bool
drm_debug_enabled_raw(enum drm_debug_category category)
{
	return ((drm_debug & (1u << (unsigned int)category)) != 0);
}
#define	drm_debug_enabled(category)	drm_debug_enabled_raw(category)

/* Linux's message levels: KERN_SOH and the level's digit. */
#ifndef	KERN_SOH
#define	KERN_SOH	"\001"
#define	KERN_EMERG	KERN_SOH "0"
#define	KERN_ALERT	KERN_SOH "1"
#define	KERN_CRIT	KERN_SOH "2"
#define	KERN_ERR	KERN_SOH "3"
#define	KERN_WARNING	KERN_SOH "4"
#define	KERN_NOTICE	KERN_SOH "5"
#define	KERN_INFO	KERN_SOH "6"
#define	KERN_DEBUG	KERN_SOH "7"
#endif

/* A format and its arguments (Linux's struct va_format). */
struct va_format {
	const char *fmt;
	va_list *va;
};

/*
 * Where printed text goes: drivers declare printers by value, make them
 * with the drm_*_printer() functions below and print with drm_printf().
 * printfn and puts are the printer's; the core's printers use arg, prefix
 * and category as described at each.
 */
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
	enum drm_debug_category category;
};

/*
 * The core's printing function, which every macro and printer here ends
 * in: formats `vaf` (with vsnprintf(9F)'s conversions) behind `prefix`
 * (NULL for none) and logs it at `level` (a KERN_* string; anything else
 * is KERN_INFO) through dev_err() on `dev`, or cmn_err() for a NULL
 * `dev`. A KERN_DEBUG message with a category (0 to 31; -1 for none) is
 * dropped unless its bit is set in drm_debug. Any context.
 */
extern void drm_illumos_vprintk(dev_info_t *dev, const char *level,
    int category, const char *prefix, struct va_format *vaf);

/*
 * The core's printers: drm_info_printer() logs at KERN_INFO on `dev` (arg
 * is the dev_info_t), drm_dbg_printer() at KERN_DEBUG in `category` on the
 * device's node (arg is the struct drm_device) and drm_err_printer() at
 * KERN_ERR with "*ERROR*"; each puts `prefix` before every message.
 */
extern void __drm_printfn_info(struct drm_printer *p, struct va_format *vaf);
extern void __drm_printfn_dbg(struct drm_printer *p, struct va_format *vaf);
extern void __drm_printfn_err(struct drm_printer *p, struct va_format *vaf);

static inline struct drm_printer
drm_info_printer(dev_info_t *dev)
{
	struct drm_printer p = { 0 };

	p.printfn = __drm_printfn_info;
	p.arg = dev;
	return (p);
}

static inline struct drm_printer
drm_dbg_printer(struct drm_device *drm, enum drm_debug_category category,
    const char *prefix)
{
	struct drm_printer p = { 0 };

	p.printfn = __drm_printfn_dbg;
	p.arg = drm;
	p.prefix = prefix;
	p.category = category;
	return (p);
}

static inline struct drm_printer
drm_err_printer(struct drm_device *drm, const char *prefix)
{
	struct drm_printer p = { 0 };

	p.printfn = __drm_printfn_err;
	p.arg = drm;
	p.prefix = prefix;
	return (p);
}

static inline void
drm_vprintf(struct drm_printer *p, const char *fmt, va_list *va)
{
	struct va_format vaf;

	vaf.fmt = fmt;
	vaf.va = va;
	p->printfn(p, &vaf);
}

/* Prints to `p`; each call is one message of the core's printers. */
static inline void
drm_printf(struct drm_printer *p, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	drm_vprintf(p, fmt, &ap);
	va_end(ap);
}

#define	drm_printf_indent(printer, indent, fmt, ...) \
	drm_printf((printer), "%.*s" fmt, (indent), "\t\t\t\t\tX", \
	    ##__VA_ARGS__)

static inline void
drm_puts(struct drm_printer *p, const char *str)
{
	if (p->puts != NULL)
		p->puts(p, str);
	else
		drm_printf(p, "%s", str);
}

/* --- logging on a device node ---------------------------------------- */

static inline void
__drm_illumos_printk(dev_info_t *dev, const char *level, int category,
    const char *fmt, ...)
{
	va_list ap;
	struct va_format vaf;

	va_start(ap, fmt);
	vaf.fmt = fmt;
	vaf.va = &ap;
	drm_illumos_vprintk(dev, level, category, NULL, &vaf);
	va_end(ap);
}

#define	drm_dev_printk(dev, level, fmt, ...) \
	__drm_illumos_printk((dev), (level), -1, fmt, ##__VA_ARGS__)
#define	__drm_dev_dbg(desc, dev, cat, fmt, ...) \
	__drm_illumos_printk((dev), KERN_DEBUG, (int)(cat), \
	    "[drm:dbg] " fmt, ##__VA_ARGS__)
#define	drm_dev_dbg(dev, cat, fmt, ...) do { \
	if (drm_debug_enabled(cat)) \
		__drm_dev_dbg(NULL, (dev), (cat), fmt, ##__VA_ARGS__); \
	} while (0)

static inline dev_info_t *
__drm_to_dev(const struct drm_device *drm)
{
	return (drm != NULL ? drm->dev : NULL);
}

#define	__drm_printk(drm, level, fmt, ...) \
	drm_dev_printk(__drm_to_dev(drm), (level), "[drm] " fmt, \
	    ##__VA_ARGS__)
#define	__drm_once(stmt) do { \
	static volatile uint32_t __drm_once_done; \
	if (atomic_cas_32(&__drm_once_done, 0, 1) == 0) \
		stmt; \
	} while (0)

#define	drm_info(drm, fmt, ...) \
	__drm_printk((drm), KERN_INFO, fmt, ##__VA_ARGS__)
#define	drm_notice(drm, fmt, ...) \
	__drm_printk((drm), KERN_NOTICE, fmt, ##__VA_ARGS__)
#define	drm_warn(drm, fmt, ...) \
	__drm_printk((drm), KERN_WARNING, fmt, ##__VA_ARGS__)
#define	drm_err(drm, fmt, ...) \
	__drm_printk((drm), KERN_ERR, "*ERROR* " fmt, ##__VA_ARGS__)
#define	drm_info_once(drm, fmt, ...) \
	__drm_once(drm_info((drm), fmt, ##__VA_ARGS__))
#define	drm_notice_once(drm, fmt, ...) \
	__drm_once(drm_notice((drm), fmt, ##__VA_ARGS__))
#define	drm_warn_once(drm, fmt, ...) \
	__drm_once(drm_warn((drm), fmt, ##__VA_ARGS__))
#define	drm_err_once(drm, fmt, ...) \
	__drm_once(drm_err((drm), fmt, ##__VA_ARGS__))
#define	drm_err_ratelimited(drm, fmt, ...) \
	drm_err((drm), fmt, ##__VA_ARGS__)

#define	drm_dbg_core(drm, fmt, ...) \
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_CORE, fmt, ##__VA_ARGS__)
#define	drm_dbg_driver(drm, fmt, ...) \
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_DRIVER, fmt, ##__VA_ARGS__)
#define	drm_dbg_kms(drm, fmt, ...) \
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_KMS, fmt, ##__VA_ARGS__)
#define	drm_dbg_prime(drm, fmt, ...) \
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_PRIME, fmt, ##__VA_ARGS__)
#define	drm_dbg_atomic(drm, fmt, ...) \
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_ATOMIC, fmt, ##__VA_ARGS__)
#define	drm_dbg_vbl(drm, fmt, ...) \
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_VBL, fmt, ##__VA_ARGS__)
#define	drm_dbg_state(drm, fmt, ...) \
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_STATE, fmt, ##__VA_ARGS__)
#define	drm_dbg_lease(drm, fmt, ...) \
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_LEASE, fmt, ##__VA_ARGS__)
#define	drm_dbg_dp(drm, fmt, ...) \
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_DP, fmt, ##__VA_ARGS__)
#define	drm_dbg_drmres(drm, fmt, ...) \
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_DRMRES, fmt, ##__VA_ARGS__)
#define	drm_dbg(drm, fmt, ...) \
	drm_dbg_driver(drm, fmt, ##__VA_ARGS__)
#define	drm_dbg_ratelimited(drm, fmt, ...) \
	drm_dbg((drm), fmt, ##__VA_ARGS__)
#define	drm_dbg_kms_ratelimited(drm, fmt, ...) \
	drm_dbg_kms((drm), fmt, ##__VA_ARGS__)

/* The DRM_DEV_* macros take the device's dev_info_t. */
#define	DRM_DEV_ERROR(dev, fmt, ...) \
	drm_dev_printk((dev), KERN_ERR, "*ERROR* " fmt, ##__VA_ARGS__)
#define	DRM_DEV_ERROR_RATELIMITED(dev, fmt, ...) \
	DRM_DEV_ERROR((dev), fmt, ##__VA_ARGS__)
#define	DRM_DEV_INFO(dev, fmt, ...) \
	drm_dev_printk((dev), KERN_INFO, fmt, ##__VA_ARGS__)
#define	DRM_DEV_INFO_ONCE(dev, fmt, ...) \
	__drm_once(DRM_DEV_INFO((dev), fmt, ##__VA_ARGS__))
#define	DRM_DEV_DEBUG(dev, fmt, ...) \
	drm_dev_dbg((dev), DRM_UT_CORE, fmt, ##__VA_ARGS__)
#define	DRM_DEV_DEBUG_DRIVER(dev, fmt, ...) \
	drm_dev_dbg((dev), DRM_UT_DRIVER, fmt, ##__VA_ARGS__)
#define	DRM_DEV_DEBUG_KMS(dev, fmt, ...) \
	drm_dev_dbg((dev), DRM_UT_KMS, fmt, ##__VA_ARGS__)

/* --- logging without a device -------------------------------------- */

#define	__drm_err(fmt, ...) \
	__drm_illumos_printk(NULL, KERN_ERR, -1, "[drm] *ERROR* " fmt, \
	    ##__VA_ARGS__)
#define	__drm_dbg(cat, fmt, ...) do { \
	if (drm_debug_enabled(cat)) \
		__drm_illumos_printk(NULL, KERN_DEBUG, (int)(cat), \
		    "[drm:dbg] " fmt, ##__VA_ARGS__); \
	} while (0)

#define	DRM_INFO(fmt, ...) \
	__drm_illumos_printk(NULL, KERN_INFO, -1, "[drm] " fmt, ##__VA_ARGS__)
#define	DRM_NOTE(fmt, ...) \
	__drm_illumos_printk(NULL, KERN_NOTICE, -1, "[drm] " fmt, \
	    ##__VA_ARGS__)
#define	DRM_WARN(fmt, ...) \
	__drm_illumos_printk(NULL, KERN_WARNING, -1, "[drm] " fmt, \
	    ##__VA_ARGS__)
#define	DRM_INFO_ONCE(fmt, ...)	__drm_once(DRM_INFO(fmt, ##__VA_ARGS__))
#define	DRM_NOTE_ONCE(fmt, ...)	__drm_once(DRM_NOTE(fmt, ##__VA_ARGS__))
#define	DRM_WARN_ONCE(fmt, ...)	__drm_once(DRM_WARN(fmt, ##__VA_ARGS__))
#define	DRM_ERROR(fmt, ...)	__drm_err(fmt, ##__VA_ARGS__)
#define	DRM_ERROR_RATELIMITED(fmt, ...)	DRM_ERROR(fmt, ##__VA_ARGS__)
#define	DRM_DEBUG(fmt, ...)	__drm_dbg(DRM_UT_CORE, fmt, ##__VA_ARGS__)
#define	DRM_DEBUG_DRIVER(fmt, ...) \
	__drm_dbg(DRM_UT_DRIVER, fmt, ##__VA_ARGS__)
#define	DRM_DEBUG_KMS(fmt, ...)	__drm_dbg(DRM_UT_KMS, fmt, ##__VA_ARGS__)
#define	DRM_DEBUG_PRIME(fmt, ...) \
	__drm_dbg(DRM_UT_PRIME, fmt, ##__VA_ARGS__)
#define	DRM_DEBUG_ATOMIC(fmt, ...) \
	__drm_dbg(DRM_UT_ATOMIC, fmt, ##__VA_ARGS__)
#define	DRM_DEBUG_VBL(fmt, ...)	__drm_dbg(DRM_UT_VBL, fmt, ##__VA_ARGS__)
#define	DRM_DEBUG_LEASE(fmt, ...) \
	__drm_dbg(DRM_UT_LEASE, fmt, ##__VA_ARGS__)
#define	DRM_DEBUG_DP(fmt, ...)	__drm_dbg(DRM_UT_DP, fmt, ##__VA_ARGS__)

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_PRINT_H */
