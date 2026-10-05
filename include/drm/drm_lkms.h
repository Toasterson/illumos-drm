/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Mode setting with Linux's KMS code carried in C (facade 2.0, the R5i
 * step): the hooks through which the core hands a Linux-KMS device's
 * mode-setting ioctls and file events to that code (misc/drm_kms), and the
 * event functions that code delivers vblank and flip events with.
 *
 * The core keeps the ioctl entry point, the argument copies (Linux's size
 * rule), the permission flags, master and authentication, the client
 * capabilities (mirrored into struct drm_file), GET_CAP, the files and
 * their event queues (read(2), chpoll(9E), the 4096-byte budget), GEM,
 * PRIME and sync objects. The Linux KMS code keeps the mode objects and
 * their IDs, properties, blobs, framebuffers, atomic state, vblank
 * counters and the mode-setting ioctl handlers. A driver uses either this
 * interface (illumos_lkms) or the core's own mode setting (illumos_rkms,
 * drm_rkms.h), never both. MIT-licensed.
 */

#ifndef _DRM_LKMS_H
#define	_DRM_LKMS_H

#include <drm/drm_facade_types.h>

#ifdef	__cplusplus
extern "C" {
#endif

struct drm_device;
struct drm_file;
struct drm_event;

/*
 * The hooks of a Linux KMS implementation (drm_driver.illumos_lkms). The
 * core uses them for a device whose drm_device.driver_features has
 * DRIVER_MODESET when drm_dev_register() runs; otherwise the mode-setting
 * ioctls fail with EOPNOTSUPP as for a render-only driver. Every hook runs
 * in thread context without core locks held. Required: ioctl.
 *
 * ioctl	a mode-setting ioctl of the core's mode-setting table
 *		(DRM_IOCTL_MODE_*, WAIT_VBLANK, MODESET_CTL,
 *		CRTC_GET/QUEUE_SEQUENCE), after the core's permission check.
 *		nr is the command's number (the low byte, as Linux's
 *		DRM_IOCTL_NR()); data is the kernel copy of the argument, of
 *		the size Linux's handler expects (shorter user structures are
 *		zero-extended); the core copies it back for commands that
 *		write. Returns 0 or a negative errno (-ENOTTY for a number the
 *		implementation does not know).
 * file_open	after the core set the file up, before the driver's open:
 *		initialise the file's KMS area (framebuffer and blob lists,
 *		pending events). A negative errno fails the open.
 * file_close	at close(9E), before the core releases the file's GEM
 *		handles and calls the driver's postclose: unlink the file's
 *		pending events, remove its framebuffers and user blobs
 *		(Linux's drm_file_free() order).
 * lastclose	after the last open file of the device closed (Linux's
 *		drm_lastclose()). Optional.
 * register	at the end of a successful drm_dev_register(), the minor
 *		nodes exist (Linux's drm_modeset_register_all()). A negative
 *		errno makes drm_dev_register() undo the registration and fail.
 * unregister	in a drm_dev_unregister() that is going to succeed, before
 *		the minor nodes go (Linux's drm_modeset_unregister_all()).
 */
struct drm_illumos_lkms_funcs {
	int (*ioctl)(struct drm_device *dev, unsigned int nr, void *data,
	    struct drm_file *file);
	int (*file_open)(struct drm_device *dev, struct drm_file *file);
	void (*file_close)(struct drm_device *dev, struct drm_file *file);
	void (*lastclose)(struct drm_device *dev);
	int (*register_dev)(struct drm_device *dev);
	void (*unregister_dev)(struct drm_device *dev);
};

/*
 * Events of a Linux-KMS device's files. These may be called in thread
 * context and in a low-level interrupt handler (below LOCK_LEVEL), with
 * the KMS code's own locks held (Linux's dev->event_lock); they never
 * sleep and never call back into the KMS code.
 *
 * drm_illumos_event_reserve
 *	charges `size` bytes to the file's event budget (Linux's
 *	event_space, 4096 bytes). 0, or -ENOMEM when it does not fit or the
 *	file is closing.
 * drm_illumos_event_unreserve
 *	gives back `size` bytes charged with drm_illumos_event_reserve()
 *	for an event that is not delivered.
 * drm_illumos_event_deliver
 *	appends a copy of the event (`event->length` bytes, at most 4096,
 *	whose space was reserved) to the file's read queue and wakes its
 *	readers and pollers. The space stays charged until read(2) has
 *	copied the event out. Events of a file that is closing are dropped
 *	and their space given back.
 */
extern int drm_illumos_event_reserve(struct drm_file *file, size_t size);
extern void drm_illumos_event_unreserve(struct drm_file *file, size_t size);
extern void drm_illumos_event_deliver(struct drm_file *file,
    const struct drm_event *event);

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_LKMS_H */
