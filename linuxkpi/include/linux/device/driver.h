/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's struct device_driver, reduced. */
#ifndef _LINUX_DEVICE_DRIVER_H
#define	_LINUX_DEVICE_DRIVER_H
#include <linux/pm.h>
struct lx_module;
struct device_driver {
	const char *name;
	const struct bus_type *bus;
	struct lx_module *owner;
	const struct dev_pm_ops *pm;
	const struct attribute_group **groups;
	const struct attribute_group **dev_groups;
	bool suppress_bind_attrs;
	int probe_type;
};
#define	PROBE_DEFAULT_STRATEGY	0
#define	PROBE_PREFER_ASYNCHRONOUS	1
#endif
