/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The illumos-drm facade: the C interface of the DRM core module
 * misc/drm_core. A driver includes this header, links with
 *
 *	ld -ztype=kmod -N misc/drm_core -z defs -M ddi.mapfile \
 *	    -M include/drm/drm_facade.mapfile ...
 *
 * and calls drm_facade_check() in _init(9E) before mod_install(9F).
 * MIT-licensed; see the individual headers for the Linux interfaces they
 * mirror.
 */

#ifndef _DRM_FACADE_H
#define	_DRM_FACADE_H

#include <drm/drm_facade_types.h>
#include <drm/drm_facade_version.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/dma_resv.h>
#include <drm/drm_gem.h>
#include <drm/dma_fence.h>
#include <drm/dma_buf.h>
#include <drm/drm_syncobj.h>
#include <drm/drm_print.h>
#include <drm/drm_managed.h>
#include <drm/drm_illumos.h>
#include <drm/drm_rkms.h>
#include <drm/drm_lkms.h>
#include <sys/cmn_err.h>
#include <sys/errno.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Compares the facade this driver was compiled against with the loaded
 * core: the same major version, a core minor version at least the
 * driver's, and the same size of every structure. Returns 0, or EINVAL
 * (positive, for _init(9E)) after logging the first difference.
 */
static inline int
drm_facade_check(const char *driver)
{
	struct drm_facade_version v;
	static const uint64_t mine[] = {
#define	DRM_FACADE_SIZEOF_(s)	sizeof (struct s),
		DRM_FACADE_STRUCTS(DRM_FACADE_SIZEOF_)
#undef	DRM_FACADE_SIZEOF_
#define	DRM_FACADE_TAILOF_(s)	sizeof (((struct s *)0)->__core),
		DRM_FACADE_TAILS(DRM_FACADE_TAILOF_)
#undef	DRM_FACADE_TAILOF_
	};
	static const char *const names[] = {
#define	DRM_FACADE_NAME_(s)	"struct " #s,
		DRM_FACADE_STRUCTS(DRM_FACADE_NAME_)
#undef	DRM_FACADE_NAME_
#define	DRM_FACADE_TAILNAME_(s)	"the private tail of struct " #s,
		DRM_FACADE_TAILS(DRM_FACADE_TAILNAME_)
#undef	DRM_FACADE_TAILNAME_
	};
	uint32_t i;

	if (drm_facade_version(&v) != 0) {
		cmn_err(CE_WARN, "%s: no DRM facade version", driver);
		return (EINVAL);
	}
	/* A variable, not a constant: minor 0 would make "< 0" a warning. */
	const uint32_t want_minor = DRM_FACADE_MINOR;
	if (v.major != DRM_FACADE_MAJOR || v.minor < want_minor) {
		cmn_err(CE_WARN, "%s: built for DRM facade %u.%u, misc/drm_core "
		    "provides %u.%u", driver, DRM_FACADE_MAJOR,
		    DRM_FACADE_MINOR, v.major, v.minor);
		return (EINVAL);
	}
	if (v.nsizes < DRM_FACADE_NSIZES) {
		cmn_err(CE_WARN, "%s: misc/drm_core knows %u facade structures, "
		    "the driver %u", driver, v.nsizes, DRM_FACADE_NSIZES);
		return (EINVAL);
	}
	for (i = 0; i < DRM_FACADE_NSIZES; i++) {
		if (v.sizes[i] != mine[i]) {
			cmn_err(CE_WARN, "%s: %s is %llu bytes in "
			    "misc/drm_core, %llu in the driver", driver,
			    names[i], (unsigned long long)v.sizes[i],
			    (unsigned long long)mine[i]);
			return (EINVAL);
		}
	}
	return (0);
}

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_FACADE_H */
