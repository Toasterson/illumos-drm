/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's firmware node properties (include/linux/property.h, GPL-2.0;
 * independent declarations). illumos-drm devices have no firmware nodes
 * (struct device.fwnode is always NULL): the reference functions accept
 * NULL and do nothing; property reads fail with -EINVAL
 * (<linux/device.h>).
 */
#ifndef _LINUX_PROPERTY_H
#define	_LINUX_PROPERTY_H

#include <linux/types.h>
#include <linux/errno.h>

struct device;
struct fwnode_operations;

struct fwnode_handle {
	struct fwnode_handle *secondary;
	const struct fwnode_operations *ops;
};

static inline struct fwnode_handle *
fwnode_handle_get(struct fwnode_handle *fwnode)
{
	return (fwnode);
}

static inline void
fwnode_handle_put(struct fwnode_handle *fwnode)
{
}

/* device_property_read_u32/present: <linux/device.h> (-EINVAL/false). */
#include <linux/device.h>

#endif
