/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's MIPI DSI bus (include/drm/drm_mipi_dsi.h, GPL-2.0-only;
 * independent declarations), reduced to the types i915's display headers
 * embed (intel_dsi.h) and the two DSC commands intel_vdsc.c sends to a DSI
 * panel. illumos-drm has no MIPI DSI bus: i915's DSI encoders are stubbed
 * (DSI is not on Ivy Bridge) and the bus functions are not provided by any
 * module; a DSI path is never reached on the supported hardware.
 */
#ifndef __DRM_MIPI_DSI_H__
#define	__DRM_MIPI_DSI_H__

#include <linux/types.h>
#include <linux/bits.h>
#include <linux/list.h>
#include <linux/device.h>

struct mipi_dsi_host_ops;
struct drm_dsc_picture_parameter_set;
struct drm_dsc_config;

enum mipi_dsi_pixel_format {
	MIPI_DSI_FMT_RGB888,
	MIPI_DSI_FMT_RGB666,
	MIPI_DSI_FMT_RGB666_PACKED,
	MIPI_DSI_FMT_RGB565,
	MIPI_DSI_FMT_RGB101010
};

#define	MIPI_DSI_MODE_VIDEO		BIT(0)
#define	MIPI_DSI_MODE_VIDEO_BURST	BIT(1)
#define	MIPI_DSI_MODE_VIDEO_SYNC_PULSE	BIT(2)
#define	MIPI_DSI_MODE_VIDEO_AUTO_VERT	BIT(3)
#define	MIPI_DSI_MODE_VIDEO_HSE		BIT(4)
#define	MIPI_DSI_MODE_VIDEO_NO_HFP	BIT(5)
#define	MIPI_DSI_MODE_VIDEO_NO_HBP	BIT(6)
#define	MIPI_DSI_MODE_VIDEO_NO_HSA	BIT(7)
#define	MIPI_DSI_MODE_NO_EOT_PACKET	BIT(9)
#define	MIPI_DSI_CLOCK_NON_CONTINUOUS	BIT(10)
#define	MIPI_DSI_MODE_LPM		BIT(11)

#define	DSI_DEV_NAME_SIZE		20

struct mipi_dsi_host {
	struct device *dev;
	const struct mipi_dsi_host_ops *ops;
	struct list_head list;
};

struct mipi_dsi_device {
	struct mipi_dsi_host *host;
	struct device dev;
	bool attached;
	char name[DSI_DEV_NAME_SIZE];
	unsigned int channel;
	unsigned int lanes;
	enum mipi_dsi_pixel_format format;
	unsigned long mode_flags;
	unsigned long hs_rate;
	unsigned long lp_rate;
	struct drm_dsc_config *dsc;
};

/* Bits per pixel of a format (-EINVAL for an unknown one). */
static inline int
mipi_dsi_pixel_format_to_bpp(enum mipi_dsi_pixel_format fmt)
{
	switch (fmt) {
	case MIPI_DSI_FMT_RGB101010:
		return (30);
	case MIPI_DSI_FMT_RGB888:
	case MIPI_DSI_FMT_RGB666:
		return (24);
	case MIPI_DSI_FMT_RGB666_PACKED:
		return (18);
	case MIPI_DSI_FMT_RGB565:
		return (16);
	}
	return (-EINVAL);
}

int mipi_dsi_compression_mode(struct mipi_dsi_device *dsi, bool enable);
int mipi_dsi_picture_parameter_set(struct mipi_dsi_device *dsi,
    const struct drm_dsc_picture_parameter_set *pps);

#endif
