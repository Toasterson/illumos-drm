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
struct drm_illumos_mode_config;

/*
 * The hooks of a Linux KMS implementation (drm_driver.illumos_lkms). The
 * core uses them once drm_dev_illumos_lkms_register() (below) has started
 * mode setting on the device; before that, and for a device it does not
 * start, the mode-setting ioctls fail with EOPNOTSUPP and GET_CAP and
 * SET_CLIENT_CAP answer as for a device without DRIVER_MODESET. Every
 * hook runs in thread context without core locks held. Required: ioctl,
 * file_open, file_close, register_dev, unregister_dev, mode_config.
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
 *		pending events). A negative errno fails the open. Only for
 *		files opened after mode setting started; a file opened before
 *		gets none of the hooks and EOPNOTSUPP for the mode-setting
 *		ioctls.
 * file_close	at close(9E) of a file file_open accepted, before the core
 *		releases the file's GEM
 *		handles and calls the driver's postclose: unlink the file's
 *		pending events, remove its framebuffers and user blobs
 *		(Linux's drm_file_free() order).
 * lastclose	after the last open file of the device closed (Linux's
 *		drm_lastclose()). Optional.
 * register_dev	in drm_dev_illumos_lkms_register(), after mode_config; the
 *		device is registered and its minor nodes exist (Linux's
 *		drm_modeset_register_all()). A negative errno fails
 *		drm_dev_illumos_lkms_register(): mode setting stays off, the
 *		device stays registered.
 * unregister_dev
 *		once, if register_dev succeeded: in a drm_dev_unregister()
 *		that is going to succeed, before the minor nodes go, or in a
 *		successful drm_dev_illumos_unplug() (Linux's
 *		drm_modeset_unregister_all() in drm_dev_unregister() and
 *		drm_dev_unplug()).
 * mode_config	fills in the mode-setting limits GET_CAP reports (cursor
 *		size, preferred depth, shadow preference, modifiers,
 *		asynchronous flips) from the KMS code's configuration
 *		(Linux's drm_mode_config); called once by
 *		drm_dev_illumos_lkms_register() before register_dev, instead
 *		of the driver calling drm_dev_illumos_set_mode_config().
 *		Required.
 */
struct drm_illumos_lkms_funcs {
	int (*ioctl)(struct drm_device *dev, unsigned int nr, void *data,
	    struct drm_file *file);
	int (*file_open)(struct drm_device *dev, struct drm_file *file);
	void (*file_close)(struct drm_device *dev, struct drm_file *file);
	void (*lastclose)(struct drm_device *dev);
	int (*register_dev)(struct drm_device *dev);
	void (*unregister_dev)(struct drm_device *dev);
	void (*mode_config)(struct drm_device *dev,
	    struct drm_illumos_mode_config *out);
};

/*
 * Starts Linux mode setting on a registered device (Linux's
 * drm_dev_register() as far as mode setting goes: the LinuxKPI layer
 * calls it where the driver calls drm_dev_register(), once the driver's
 * mode configuration exists and it has settled drm_device.driver_features).
 * drm_dev_register() never starts it. If the device's driver_features has
 * DRIVER_MODESET and its table has illumos_lkms: mode_config (the GET_CAP
 * limits), register_dev, and from then on the mode-setting ioctls of
 * files opened afterwards go to the ioctl hook; GET_CAP and
 * SET_CLIENT_CAP follow driver_features (DRIVER_MODESET, DRIVER_ATOMIC)
 * as read here. Returns 0 (also when the device has no DRIVER_MODESET or
 * no illumos_lkms: then nothing is done), -EINVAL for a device that is
 * not registered (or unplugged), -EBUSY for a second call or while a file
 * of the device is open (the call is not counted then), or register_dev's
 * negative errno (mode setting stays off; the device stays registered).
 * Thread context, no core locks held.
 */
extern int drm_dev_illumos_lkms_register(struct drm_device *dev);

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
/*
 * drm_dev_illumos_lkms_register
 *	starts Linux KMS on a registered device: called once, when the
 *	driver's mode configuration exists (the LinuxKPI layer calls it
 *	where a Linux driver calls drm_dev_register()). If
 *	dev->driver_features has DRIVER_MODESET at that moment and the
 *	driver has illumos_lkms, the core calls the mode_config hook, then
 *	register_dev, and forwards the mode-setting ioctls from then on.
 *	0 (also when there is nothing to start), -EINVAL for a device that
 *	is not registered, -EBUSY for a second call, or register_dev's
 *	error.
 */
extern int drm_dev_illumos_lkms_register(struct drm_device *dev);

extern int drm_illumos_event_reserve(struct drm_file *file, size_t size);
extern void drm_illumos_event_unreserve(struct drm_file *file, size_t size);
extern void drm_illumos_event_deliver(struct drm_file *file,
    const struct drm_event *event);

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_LKMS_H */
