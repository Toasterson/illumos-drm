/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's platform devices (include/linux/platform_device.h, GPL-2.0;
 * independent declarations). The illumos layer has no platform bus:
 * registering a platform device fails with -ENODEV and unregistering
 * accepts NULL and error pointers (DRM's HDMI audio codec device, which
 * illumos-drm never creates).
 */
#ifndef _LINUX_PLATFORM_DEVICE_H
#define	_LINUX_PLATFORM_DEVICE_H

#include <linux/device.h>
#include <linux/err.h>

#define	PLATFORM_DEVID_NONE	(-1)
#define	PLATFORM_DEVID_AUTO	(-2)

struct resource;

struct platform_device {
	const char *name;
	int id;
	struct device dev;
	u32 num_resources;
	struct resource *resource;
};

#define	to_platform_device(x)	container_of((x), struct platform_device, dev)

struct platform_device_info {
	struct device *parent;
	const char *name;
	int id;
	const void *data;
	size_t size_data;
};

static inline struct platform_device *
platform_device_register_full(const struct platform_device_info *info)
{
	return (ERR_PTR(-ENODEV));
}

static inline struct platform_device *
platform_device_register_data(struct device *parent, const char *name,
    int id, const void *data, size_t size)
{
	return (ERR_PTR(-ENODEV));
}

static inline int
platform_device_register(struct platform_device *pdev)
{
	return (-ENODEV);
}

static inline void
platform_device_unregister(struct platform_device *pdev)
{
}

static inline void *
platform_get_drvdata(const struct platform_device *pdev)
{
	return (dev_get_drvdata(&pdev->dev));
}

static inline void
platform_set_drvdata(struct platform_device *pdev, void *data)
{
	dev_set_drvdata(&pdev->dev, data);
}

#endif
