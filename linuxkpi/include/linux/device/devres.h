/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's managed device resources (include/linux/device/devres.h,
 * GPL-2.0; reimplemented in src/lkpi_device.c): actions and allocations
 * released newest first when the device's driver detaches or its probe
 * fails.
 */
#ifndef _LINUX_DEVICE_DEVRES_H
#define	_LINUX_DEVICE_DEVRES_H

#include <linux/types.h>
#include <linux/gfp.h>

struct device;

extern int devm_add_action(struct device *dev, void (*action)(void *),
    void *data);
extern int devm_add_action_or_reset(struct device *dev,
    void (*action)(void *), void *data);
extern void devm_remove_action(struct device *dev, void (*action)(void *),
    void *data);
extern void devm_release_action(struct device *dev, void (*action)(void *),
    void *data);
extern void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp);
extern void devm_kfree(struct device *dev, const void *p);
extern char *devm_kstrdup(struct device *dev, const char *s, gfp_t gfp);
extern void lkpi_devres_release_all(struct device *dev);

#define	devm_kzalloc(dev, size, gfp)	devm_kmalloc((dev), (size), (gfp) | __GFP_ZERO)
#define	devm_kcalloc(dev, n, size, gfp)	devm_kmalloc((dev), size_mul(n, size), (gfp) | __GFP_ZERO)
#define	devm_kmalloc_array(dev, n, s, g)	devm_kmalloc((dev), size_mul(n, s), (g))

#endif
