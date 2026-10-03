/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's struct bus_type, reduced to a name. */
#ifndef _LINUX_DEVICE_BUS_H
#define	_LINUX_DEVICE_BUS_H
struct device;
struct bus_type {
	const char *name;
};
#define	bus_register(b)		0
#define	bus_unregister(b)	((void)(b))
#endif
