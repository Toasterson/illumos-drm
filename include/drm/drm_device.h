/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The DRM device of the illumos-drm facade.
 *
 * Origin: struct drm_device and struct drm_minor follow Linux v7.2's
 * include/drm/drm_device.h and include/drm/drm_file.h (both MIT): the
 * public fields keep Linux's names and relative order, with illumos types
 * (dev_info_t for struct device, kmutex_t-based locks). Fields that facade
 * version 0 does not carry are listed below; the core's own state lives
 * behind the opaque tail __core. This file is a reimplementation for
 * illumos, MIT-licensed.
 *
 * Not in facade version 0 (Linux fields left out): managed, accel, master,
 * huge_mnt, master_mutex, open_count, filelist_internal, clientlist_mutex,
 * clientlist, client_sysrq_list, vblank, vbl_lock, max_vblank_count,
 * vblank_event_list, mode_config (with the C mode-setting core, R5i),
 * object_name_lock, object_name_idr, vram_mm, fb_helper, gem_lru_mutex.
 */

#ifndef _DRM_DEVICE_H
#define	_DRM_DEVICE_H

#include <drm/drm_facade_types.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* Linux's enum drm_minor_type. */
enum drm_minor_type {
	DRM_MINOR_PRIMARY = 0,
	DRM_MINOR_CONTROL = 1,
	DRM_MINOR_RENDER = 2,
	DRM_MINOR_ACCEL = 32
};

/*
 * A minor node of a device (card<N> or renderD<128+N>). Allocated by the
 * core at drm_dev_register(); read-only for drivers.
 */
struct drm_minor {
	int index;		/* the node's minor: N, or 128 + N */
	int type;		/* enum drm_minor_type */
	dev_info_t *kdev;	/* the node the minor node was created on */
	struct drm_device *dev;
};

/* Linux's enum switch_power_state (vga_switcheroo). */
enum switch_power_state {
	DRM_SWITCH_POWER_ON = 0,
	DRM_SWITCH_POWER_OFF = 1,
	DRM_SWITCH_POWER_CHANGING = 2,
	DRM_SWITCH_POWER_DYNAMIC_OFF = 3
};

struct drm_vma_offset_manager;

/*
 * A DRM device. A driver allocates it with drm_dev_alloc() or embeds it in
 * its own structure with drm_dev_alloc_container(), registers it with
 * drm_dev_register() in attach(9E), and unregisters it with
 * drm_dev_unregister() in detach(9E).
 *
 * Which fields are maintained in facade version 0:
 *	if_version, ref, dev, dma_dev, driver, dev_private, primary, render,
 *	registered, driver_features, unplugged, unique, filelist_mutex,
 *	filelist (every open file of the device, linked through
 *	drm_file.lhead; walk it holding filelist_mutex), num_crtcs,
 *	vma_offset_manager (non-NULL for a GEM driver, opaque),
 *	switch_power_state (always DRM_SWITCH_POWER_ON).
 *	anon_inode and debugfs_root are always NULL (illumos has neither);
 *	vblank_disable_immediate, vblank_time_lock and event_lock are
 *	initialised but not used by the core.
 */
struct drm_device {
	int if_version;
	struct kref ref;
	dev_info_t *dev;
	dev_info_t *dma_dev;
	const struct drm_driver *driver;
	void *dev_private;
	struct drm_minor *primary;
	struct drm_minor *render;
	bool registered;
	uint32_t driver_features;
	bool unplugged;
	void *anon_inode;
	char *unique;
	struct lx_mutex filelist_mutex;
	struct list_head filelist;
	bool vblank_disable_immediate;
	spinlock_t vblank_time_lock;
	spinlock_t event_lock;
	unsigned int num_crtcs;
	struct drm_vma_offset_manager *vma_offset_manager;
	enum switch_power_state switch_power_state;
	void *debugfs_root;
	/* private: the core's state */
	uint64_t __core[16];
};

/*
 * Allocates a device for driver `driver` on the driver's own node
 * `parent` (the dev_info_t of the current attach(9E)). Returns the device
 * with one reference, or ERR_PTR(-EINVAL) for a driver table built for
 * another facade version or a NULL argument, ERR_PTR(-ENOMEM).
 */
extern struct drm_device *drm_dev_alloc(const struct drm_driver *driver,
    dev_info_t *parent);

/*
 * Allocates a zeroed driver structure of `size` bytes with an embedded
 * struct drm_device at `offset` and initialises the device as
 * drm_dev_alloc() does. Returns the start of the structure or
 * ERR_PTR(-errno). The structure is freed by the core when the last
 * reference to the device is dropped (drm_dev_put()), after the driver's
 * release callback. The illumos counterpart of Linux's
 * __devm_drm_dev_alloc(); illumos has no devres, so the driver drops its
 * reference itself after drm_dev_unregister().
 */
extern void *__drm_dev_alloc(dev_info_t *parent,
    const struct drm_driver *driver, size_t size, size_t offset);

#define	drm_dev_alloc_container(parent, driver, type, member) \
	((type *)__drm_dev_alloc(parent, driver, sizeof (type), \
	    offsetof(type, member)))

/*
 * Sets the device's unique name, the bus ID that GET_UNIQUE reports
 * ("pci:0000:00:02.0"). Call before drm_dev_register(); without it the
 * core uses "pseudo:<driver>@<instance>". Returns 0 or -EINVAL (too long,
 * at most 63 bytes) or -EBUSY (already registered).
 */
extern int drm_dev_set_unique(struct drm_device *dev, const char *name);

/*
 * Registers the device: takes the lowest free DRM device number N of the
 * system, creates the minor nodes card<N> and (with DRIVER_RENDER)
 * renderD<128+N> on the driver's node, and sets up the GEM, sync and
 * mode-setting state. `flags` must be 0. Returns 0 or a negative errno;
 * on failure nothing is left registered.
 */
extern int drm_dev_register(struct drm_device *dev, unsigned long flags);

/*
 * Unregisters the device. Unlike Linux, where unregistering cannot fail,
 * illumos detach(9E) may refuse: this returns -EBUSY and leaves the device
 * registered and working while files are open, framebuffers or GEM
 * objects remain, or fences and sync objects are alive. Returns 0 once the
 * device is gone; the device structure stays until drm_dev_put().
 */
extern int drm_dev_unregister(struct drm_device *dev);

extern void drm_dev_get(struct drm_device *dev);
extern void drm_dev_put(struct drm_device *dev);

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_DEVICE_H */
