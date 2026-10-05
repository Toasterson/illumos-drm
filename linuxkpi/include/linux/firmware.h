/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's firmware loading over firmload(9F) (include/linux/firmware.h,
 * GPL-2.0; reimplemented in src/lkpi_firmware.c). A name "dir/file" is
 * opened with firmware_open(9F) as firmware "file" of module "dir", which
 * searches the firmware directories of the module path
 * (/kernel/firmware/dir/, /usr/kernel/firmware/dir/; decision D8 puts
 * GPU firmware in /usr/kernel/firmware/<vendor>/); a name without a
 * slash uses the calling driver's name. The whole file is read into a
 * kmem buffer. request_firmware_nowait() runs the callback from a work
 * item.
 */
#ifndef _LINUX_FIRMWARE_H
#define	_LINUX_FIRMWARE_H

#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/module.h>

struct device;
struct lx_module;

struct firmware {
	size_t size;
	const u8 *data;
	void *priv;
};

extern int request_firmware(const struct firmware **fw, const char *name,
    struct device *device);
extern int firmware_request_nowarn(const struct firmware **fw,
    const char *name, struct device *device);
extern int request_firmware_direct(const struct firmware **fw,
    const char *name, struct device *device);
extern int request_firmware_into_buf(const struct firmware **fw,
    const char *name, struct device *device, void *buf, size_t size);
extern int request_firmware_nowait(struct lx_module *module, bool uevent,
    const char *name, struct device *device, gfp_t gfp, void *context,
    void (*cont)(const struct firmware *fw, void *context));
extern void release_firmware(const struct firmware *fw);
#define	FW_ACTION_UEVENT	1
#define	FW_ACTION_NOUEVENT	0

#endif
