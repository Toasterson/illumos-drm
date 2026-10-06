/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <acpi/video.h> (GPL-2.0; independent declarations), as on Linux
 * without CONFIG_ACPI_VIDEO: no ACPI video device; registration does
 * nothing and the native (driver) backlight is the one to use.
 */
#ifndef _ACPI_VIDEO_H
#define	_ACPI_VIDEO_H

#include <linux/types.h>
#include <linux/errno.h>

struct acpi_device;

enum acpi_backlight_type {
	acpi_backlight_undef = -1,
	acpi_backlight_none = 0,
	acpi_backlight_video,
	acpi_backlight_vendor,
	acpi_backlight_native,
	acpi_backlight_nvidia_wmi_ec,
	acpi_backlight_apple_gmux,
	acpi_backlight_dell_uart
};

static inline int
acpi_video_register(void)
{
	return (-ENODEV);
}

static inline void
acpi_video_unregister(void)
{
}

static inline void
acpi_video_register_backlight(void)
{
}

static inline bool
acpi_video_backlight_use_native(void)
{
	return (true);
}

static inline enum acpi_backlight_type
acpi_video_get_backlight_type(void)
{
	return (acpi_backlight_native);
}

#endif
