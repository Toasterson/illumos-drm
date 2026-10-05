/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's device tree interface (include/linux/of.h, GPL-2.0;
 * independent declarations), as on a Linux kernel without CONFIG_OF:
 * there are no device tree nodes (struct device.of_node is always NULL),
 * lookups find nothing and property reads fail with -ENOSYS.
 */
#ifndef _LINUX_OF_H
#define	_LINUX_OF_H

#include <linux/types.h>
#include <linux/errno.h>

struct device_node;
struct property;
struct of_phandle_args;

static inline struct device_node *
of_node_get(struct device_node *node)
{
	return (node);
}

static inline void
of_node_put(struct device_node *node)
{
}

static inline bool
of_device_is_available(const struct device_node *node)
{
	return (false);
}

static inline int
of_property_read_u32(const struct device_node *np, const char *name,
    u32 *out)
{
	return (-ENOSYS);
}

static inline struct device_node *
of_get_child_by_name(const struct device_node *node, const char *name)
{
	return (NULL);
}

#define	of_match_ptr(p)		NULL
#define	for_each_child_of_node(parent, child) \
	for ((child) = NULL; (child) != NULL; )

#endif
