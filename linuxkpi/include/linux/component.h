/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's component framework (binding the graphics driver with the
 * audio, MEI and HDCP drivers). illumos has no such companion drivers, so
 * no aggregate driver ever binds a component: component_add() succeeds
 * and the component stays unbound, as on Linux before (or without) the
 * aggregate driver (i915's display audio then has no audio component,
 * and every use of it checks for that); removal does nothing.
 * component_master_add_with_match() fails with -ENODEV (an aggregate
 * driver needs its components, which never come).
 */
#ifndef _LINUX_COMPONENT_H
#define	_LINUX_COMPONENT_H
#include <linux/device.h>
struct component_ops {
	int (*bind)(struct device *comp, struct device *master, void *master_data);
	void (*unbind)(struct device *comp, struct device *master, void *master_data);
};
struct component_master_ops {
	int (*bind)(struct device *master);
	void (*unbind)(struct device *master);
};
struct component_match;
#define	component_add(dev, ops)			((void)(dev), 0)
#define	component_add_typed(dev, ops, t)	((void)(dev), 0)
#define	component_del(dev, ops)			((void)(dev))
#define	component_bind_all(m, d)		(-ENODEV)
#define	component_unbind_all(m, d)		((void)(m))
#define	component_master_add_with_match(d, o, m)	(-ENODEV)
#define	component_master_del(d, o)		((void)(d))
#endif
