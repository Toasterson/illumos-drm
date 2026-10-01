/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The driver description and the character-device entry points of the
 * illumos-drm facade.
 *
 * Origin: struct drm_driver and enum drm_driver_feature follow Linux
 * v7.2's include/drm/drm_drv.h (MIT): the callbacks and fields facade
 * version 0 supports keep Linux's names, types and relative order; the
 * illumos_* fields at the end are illumos additions. The entry points
 * drm_open() ... drm_segmap() replace Linux's struct file_operations
 * (DEFINE_DRM_GEM_FOPS). This file is a reimplementation for illumos,
 * MIT-licensed.
 */

#ifndef _DRM_DRV_H
#define	_DRM_DRV_H

#include <drm/drm_facade_types.h>
#include <drm/drm_facade_version.h>
#include <sys/conf.h>
#include <sys/cred.h>
#include <sys/uio.h>
#include <sys/poll.h>
#include <sys/ddidevmap.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* Linux's driver feature flags (the ones the core implements). */
enum drm_driver_feature {
	DRIVER_GEM = (1 << 0),
	DRIVER_MODESET = (1 << 1),
	DRIVER_RENDER = (1 << 3),
	DRIVER_ATOMIC = (1 << 4),
	DRIVER_SYNCOBJ = (1 << 5),
	DRIVER_SYNCOBJ_TIMELINE = (1 << 6)
};

/*
 * A driver. One static table per driver module, passed to
 * drm_dev_alloc(); the core reads it, never writes it.
 *
 * open		called for each open(2) of a card or render node, after
 *		the core has set the file up; a negative errno fails the
 *		open. Optional.
 * postclose	called at the file's close(9E), after the core has released
 *		its handles; frees driver_priv. Optional.
 * release	called when the last reference to the device is dropped,
 *		before the core frees the structure. Optional.
 * gem_prime_import
 *		imports a dma-buf of another device. NULL means
 *		drm_gem_prime_import(), which makes a core object that shares
 *		the exporter's memory. Optional.
 * dumb_create	MODE_CREATE_DUMB. Required for dumb buffers: allocate an
 *		object (drm_gem_object_init()), create a handle
 *		(drm_gem_handle_create()), fill in handle, pitch and size.
 *		The core has checked the caller's permission.
 * dumb_map_offset
 *		MODE_MAP_DUMB. NULL means drm_gem_dumb_map_offset().
 * major, minor, patchlevel, name, desc
 *		what the VERSION ioctl reports.
 * driver_features
 *		DRIVER_* flags; DRIVER_SYNCOBJ_TIMELINE needs DRIVER_SYNCOBJ.
 *		DRIVER_MODESET with illumos_rkms (and DRIVER_GEM) has a
 *		display; without illumos_rkms the device reports the
 *		mode-setting capabilities but has no display objects, and the
 *		mode-setting ioctls fail with EOPNOTSUPP.
 * ioctls, num_ioctls
 *		the driver's private ioctls (drm_ioctl.h).
 *
 * illumos_facade_major, illumos_facade_minor
 *		the facade version the driver was compiled against; set them
 *		with DRM_FACADE_DRIVER_VERSION. drm_dev_alloc() refuses a
 *		table of another major version or a newer minor version.
 * illumos_date	the date VERSION reports (Linux dropped it); NULL is "0".
 * illumos_max_object_size, illumos_max_memory
 *		the largest GEM object and the most object memory of one
 *		device, in bytes; 0 means 256 MiB and 1 GiB.
 * illumos_before_detach
 *		called first by a drm_dev_unregister() that may still be
 *		refused: stop starting new work and signal the fences the
 *		driver owns (with an error), because every fence must
 *		signal. Optional.
 * illumos_before_teardown
 *		called once drm_dev_unregister() is sure to succeed, before
 *		the mode-setting and GEM state go: stop vblank and fence
 *		sources. Optional.
 * illumos_rkms	mode setting with the core's R3 implementation (drm_rkms.h);
 *		for the Rust drivers until the C mode-setting core (R5i).
 */
struct drm_driver {
	int (*open)(struct drm_device *dev, struct drm_file *file);
	void (*postclose)(struct drm_device *dev, struct drm_file *file);
	void (*release)(struct drm_device *dev);
	struct drm_gem_object *(*gem_prime_import)(struct drm_device *dev,
	    struct dma_buf *dma_buf);
	int (*dumb_create)(struct drm_file *file_priv,
	    struct drm_device *dev, struct drm_mode_create_dumb *args);
	int (*dumb_map_offset)(struct drm_file *file_priv,
	    struct drm_device *dev, uint32_t handle, uint64_t *offset);
	int major;
	int minor;
	int patchlevel;
	const char *name;
	const char *desc;
	uint32_t driver_features;
	const struct drm_ioctl_desc *ioctls;
	int num_ioctls;
	/* illumos additions */
	uint32_t illumos_facade_major;
	uint32_t illumos_facade_minor;
	const char *illumos_date;
	uint64_t illumos_max_object_size;
	uint64_t illumos_max_memory;
	void (*illumos_before_detach)(struct drm_device *dev);
	void (*illumos_before_teardown)(struct drm_device *dev);
	const struct drm_rkms_funcs *illumos_rkms;
};

#define	DRM_FACADE_DRIVER_VERSION \
	.illumos_facade_major = DRM_FACADE_MAJOR, \
	.illumos_facade_minor = DRM_FACADE_MINOR

/*
 * Per-device settings that Linux keeps in struct device or
 * drm_mode_config. Call them between drm_dev_alloc() and
 * drm_dev_register(); afterwards they fail with -EBUSY.
 *
 * drm_dev_illumos_set_dma_attr
 *	GEM memory (drm_gem_object_init()) is bound for DMA against the
 *	device's node with these attributes; the core keeps a copy. Without
 *	it objects have no DMA addresses (a pseudo device).
 * drm_dev_illumos_set_fence_priority
 *	the interrupt priority (ddi_intr_get_pri(9F)) of the highest
 *	interrupt handler that signals the device's fences; the core
 *	creates the locks such a signal takes at that priority. It must be a
 *	low-level priority (below LOCK_LEVEL): a high-level handler hands
 *	the signal to a soft interrupt. Without it fences are signalled in
 *	thread context or from soft interrupts only.
 * drm_dev_illumos_set_mode_config
 *	the mode-setting limits GET_CAP reports.
 */
struct drm_illumos_mode_config {
	uint64_t cursor_width;
	uint64_t cursor_height;
	uint64_t preferred_depth;
	bool prefer_shadow;
	bool fb_modifiers;
};

extern int drm_dev_illumos_set_dma_attr(struct drm_device *dev,
    const ddi_dma_attr_t *attr);
extern int drm_dev_illumos_set_fence_priority(struct drm_device *dev,
    uint_t pri);
extern int drm_dev_illumos_set_mode_config(struct drm_device *dev,
    const struct drm_illumos_mode_config *cfg);

/*
 * The character-device entry points. Every DRM driver points its cb_ops
 * at them (DRM_CB_OPS below): the core serves the card, render, dma-buf
 * and sync-file minor numbers of every registered device, whatever driver
 * attached it.
 */
extern int drm_open(dev_t *devp, int flag, int otyp, cred_t *credp);
extern int drm_close(dev_t dev, int flag, int otyp, cred_t *credp);
extern int drm_read(dev_t dev, struct uio *uiop, cred_t *credp);
extern int drm_ioctl(dev_t dev, int cmd, intptr_t arg, int mode,
    cred_t *credp, int *rvalp);
extern int drm_chpoll(dev_t dev, short events, int anyyet, short *reventsp,
    struct pollhead **phpp);
extern int drm_devmap(dev_t dev, devmap_cookie_t dhp, offset_t off,
    size_t len, size_t *maplen, uint_t model);
extern int drm_segmap(dev_t dev, off_t off, struct as *asp, caddr_t *addrp,
    off_t len, uint_t prot, uint_t maxprot, uint_t flags, cred_t *credp);

/*
 * getinfo(9E) for a DRM driver: maps the device numbers the core hands
 * out to the driver's instance and node.
 */
extern int drm_getinfo(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg,
    void **resultp);

/*
 * The instance of the driver that owns a DRM minor number (a node, clone,
 * dma-buf or sync-file minor), or -1.
 */
extern int drm_minor_instance(minor_t minor);

/* A cb_ops initialiser for a DRM driver. */
#define	DRM_CB_OPS { \
	.cb_open = drm_open, \
	.cb_close = drm_close, \
	.cb_strategy = nodev, \
	.cb_print = nodev, \
	.cb_dump = nodev, \
	.cb_read = drm_read, \
	.cb_write = nodev, \
	.cb_ioctl = drm_ioctl, \
	.cb_devmap = drm_devmap, \
	.cb_mmap = nodev, \
	.cb_segmap = drm_segmap, \
	.cb_chpoll = drm_chpoll, \
	.cb_prop_op = ddi_prop_op, \
	.cb_str = NULL, \
	.cb_flag = D_NEW | D_MP | D_64BIT | D_DEVMAP, \
	.cb_rev = CB_REV, \
	.cb_aread = nodev, \
	.cb_awrite = nodev \
}

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_DRV_H */
