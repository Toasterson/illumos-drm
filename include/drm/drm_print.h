/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Logging for drivers on the illumos-drm facade.
 *
 * Origin: the macro names follow Linux v7.2's include/drm/drm_print.h
 * (MIT): drm_err(), drm_warn(), drm_info(), drm_dbg(). They print through
 * dev_err(9F) on the device's node, so messages carry the driver name and
 * instance. drm_dbg() prints only while the debug category is enabled in
 * the core's drm_debug variable (set it with mdb -kw or
 * "set drm:drm_debug = 0x..." in /etc/system). MIT-licensed.
 */

#ifndef _DRM_PRINT_H
#define	_DRM_PRINT_H

#include <drm/drm_facade_types.h>
#include <drm/drm_device.h>
#include <sys/cmn_err.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* Linux's debug categories (enum drm_debug_category). */
enum drm_debug_category {
	DRM_UT_CORE = 0x01,
	DRM_UT_DRIVER = 0x02,
	DRM_UT_KMS = 0x04,
	DRM_UT_PRIME = 0x08,
	DRM_UT_ATOMIC = 0x10,
	DRM_UT_VBL = 0x20,
	DRM_UT_STATE = 0x40,
	DRM_UT_LEASE = 0x80,
	DRM_UT_DP = 0x100,
	DRM_UT_DRMRES = 0x200
};

/* The enabled debug categories; 0 by default. */
extern volatile uint32_t drm_debug;

static inline bool
drm_debug_enabled(enum drm_debug_category category)
{
	return ((drm_debug & (uint32_t)category) != 0);
}

/*
 * "!" sends a message to the system log only, "^" to the console only;
 * errors and warnings go to both, information and debug messages to the
 * log.
 */
#define	drm_err(drm, fmt, ...) \
	dev_err((drm)->dev, CE_WARN, "[drm] *ERROR* " fmt, ##__VA_ARGS__)
#define	drm_warn(drm, fmt, ...) \
	dev_err((drm)->dev, CE_WARN, "[drm] " fmt, ##__VA_ARGS__)
#define	drm_info(drm, fmt, ...) \
	dev_err((drm)->dev, CE_NOTE, "![drm] " fmt, ##__VA_ARGS__)
#define	drm_dbg_driver(drm, fmt, ...) do { \
	if (drm_debug_enabled(DRM_UT_DRIVER)) \
		dev_err((drm)->dev, CE_NOTE, "![drm:dbg] " fmt, \
		    ##__VA_ARGS__); \
	} while (0)
#define	drm_dbg_core(drm, fmt, ...) do { \
	if (drm_debug_enabled(DRM_UT_CORE)) \
		dev_err((drm)->dev, CE_NOTE, "![drm:dbg] " fmt, \
		    ##__VA_ARGS__); \
	} while (0)
#define	drm_dbg(drm, fmt, ...)	drm_dbg_driver(drm, fmt, ##__VA_ARGS__)

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_PRINT_H */
