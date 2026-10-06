/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's backlight class (include/linux/backlight.h, GPL-2.0;
 * independent declarations). illumos has no backlight class devices
 * (CONFIG_BACKLIGHT_CLASS_DEVICE is off in the illumos configurations):
 * the structures exist so that drivers compile; registration fails with
 * -ENODEV (drivers then run without a class device, the panel backlight is
 * still switched by their mode setting), lookups find nothing, and the
 * helpers act on the structure alone (calling ops->update_status as Linux
 * does).
 */
#ifndef _LINUX_BACKLIGHT_H
#define	_LINUX_BACKLIGHT_H

#include <linux/types.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/device.h>

enum backlight_update_reason {
	BACKLIGHT_UPDATE_HOTKEY,
	BACKLIGHT_UPDATE_SYSFS
};

enum backlight_type {
	BACKLIGHT_RAW = 1,
	BACKLIGHT_PLATFORM,
	BACKLIGHT_FIRMWARE,
	BACKLIGHT_TYPE_MAX
};

enum backlight_scale {
	BACKLIGHT_SCALE_UNKNOWN = 0,
	BACKLIGHT_SCALE_LINEAR,
	BACKLIGHT_SCALE_NON_LINEAR
};

struct backlight_device;

#define	BL_CORE_SUSPENDRESUME	(1 << 0)

struct backlight_ops {
	unsigned int options;
	int (*update_status)(struct backlight_device *bd);
	int (*get_brightness)(struct backlight_device *bd);
	bool (*controls_device)(struct backlight_device *bd,
	    struct device *display_dev);
};

#define	BACKLIGHT_POWER_ON	(0)
#define	BACKLIGHT_POWER_OFF	(4)
#define	BACKLIGHT_POWER_REDUCED	(1)

#define	BL_CORE_SUSPENDED	(1 << 0)
#define	BL_CORE_FBBLANK		(1 << 1)

struct backlight_properties {
	int brightness;
	int max_brightness;
	int power;
	enum backlight_type type;
	unsigned int state;
	enum backlight_scale scale;
};

struct backlight_device {
	struct backlight_properties props;
	struct mutex update_lock;
	struct mutex ops_lock;
	const struct backlight_ops *ops;
	struct list_head entry;
	struct device dev;
	bool fb_bl_on[32];
	int use_count;
};

static inline int
backlight_update_status(struct backlight_device *bd)
{
	int ret = -ENOENT;

	mutex_lock(&bd->update_lock);
	if (bd->ops != NULL && bd->ops->update_status != NULL)
		ret = bd->ops->update_status(bd);
	mutex_unlock(&bd->update_lock);
	return (ret);
}

static inline int
backlight_enable(struct backlight_device *bd)
{
	if (bd == NULL)
		return (0);
	bd->props.power = BACKLIGHT_POWER_ON;
	bd->props.state &= ~BL_CORE_FBBLANK;
	return (backlight_update_status(bd));
}

static inline int
backlight_disable(struct backlight_device *bd)
{
	if (bd == NULL)
		return (0);
	bd->props.power = BACKLIGHT_POWER_OFF;
	bd->props.state |= BL_CORE_FBBLANK;
	return (backlight_update_status(bd));
}

static inline bool
backlight_is_blank(const struct backlight_device *bd)
{
	return (bd->props.power != BACKLIGHT_POWER_ON ||
	    (bd->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK)) != 0);
}

static inline int
backlight_get_brightness(const struct backlight_device *bd)
{
	return (backlight_is_blank(bd) ? 0 : bd->props.brightness);
}

static inline void *
bl_get_data(struct backlight_device *bd)
{
	return (dev_get_drvdata(&bd->dev));
}

static inline struct backlight_device *
backlight_device_register(const char *name, struct device *dev,
    void *devdata, const struct backlight_ops *ops,
    const struct backlight_properties *props)
{
	return (ERR_PTR(-ENODEV));
}

static inline struct backlight_device *
devm_backlight_device_register(struct device *dev, const char *name,
    struct device *parent, void *devdata, const struct backlight_ops *ops,
    const struct backlight_properties *props)
{
	return (ERR_PTR(-ENODEV));
}

static inline void
backlight_device_unregister(struct backlight_device *bd)
{
}

static inline struct backlight_device *
backlight_device_get_by_name(const char *name)
{
	return (NULL);
}

static inline struct backlight_device *
backlight_device_get_by_type(enum backlight_type type)
{
	return (NULL);
}

static inline void
backlight_force_update(struct backlight_device *bd,
    enum backlight_update_reason reason)
{
}

static inline void
put_device_backlight(struct backlight_device *bd)
{
}

#endif
