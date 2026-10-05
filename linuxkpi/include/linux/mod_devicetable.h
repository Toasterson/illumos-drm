/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's PCI device-ID table entry (the same layout). */
#ifndef _LINUX_MOD_DEVICETABLE_H
#define	_LINUX_MOD_DEVICETABLE_H
#include <linux/types.h>
typedef unsigned long kernel_ulong_t;
struct pci_device_id {
	__u32 vendor, device;
	__u32 subvendor, subdevice;
	__u32 class, class_mask;
	kernel_ulong_t driver_data;
	__u32 override_only;
};
#define	PCI_DEVICE(vend, dev) \
	.vendor = (vend), .device = (dev), \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define	PCI_VDEVICE(vend, dev) \
	.vendor = PCI_VENDOR_ID_##vend, .device = (dev), \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, 0, 0
#define	PCI_DEVICE_CLASS(dev_class, dev_class_mask) \
	.class = (dev_class), .class_mask = (dev_class_mask), \
	.vendor = PCI_ANY_ID, .device = PCI_ANY_ID, \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
struct x86_cpu_id {
	__u16 vendor;
	__u16 family;
	__u16 model;
	__u16 steppings;
	__u16 feature;
	__u16 flags;
	__u32 type;
	kernel_ulong_t driver_data;
};
#define	MODULE_DEVICE_TABLE(type, name)
#endif
