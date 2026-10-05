/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's struct bus_type, reduced to a name. */
#ifndef _LINUX_DEVICE_BUS_H
#define	_LINUX_DEVICE_BUS_H
struct device;
struct bus_type {
	const char *name;
};
#define	BUS_NOTIFY_ADD_DEVICE		0x00000001
#define	BUS_NOTIFY_DEL_DEVICE		0x00000002
#define	BUS_NOTIFY_BIND_DRIVER		0x00000004
#define	BUS_NOTIFY_BOUND_DRIVER		0x00000005
#define	BUS_NOTIFY_UNBIND_DRIVER	0x00000006
#define	BUS_NOTIFY_UNBOUND_DRIVER	0x00000007
#define	BUS_NOTIFY_DRIVER_NOT_BOUND	0x00000008
/* Bus notifiers are not supported (no auxiliary/MEI devices): -ENODEV. */
#define	bus_register_notifier(b, nb)	((void)(b), (void)(nb), -ENODEV)
#define	bus_unregister_notifier(b, nb)	((void)(b), (void)(nb), 0)
#define	bus_register(b)		0
#define	bus_unregister(b)	((void)(b))
#endif
