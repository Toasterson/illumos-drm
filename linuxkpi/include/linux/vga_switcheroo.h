/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's GPU switching between integrated and discrete GPUs. Not
 * supported: registration fails with -ENODEV, the queries report no
 * switcheroo, the DDC lock is a no-op (unlocking reports -ENODEV, as on
 * Linux without CONFIG_VGA_SWITCHEROO).
 */
#ifndef _LINUX_VGA_SWITCHEROO_H
#define	_LINUX_VGA_SWITCHEROO_H
#include <linux/types.h>
#include <linux/fb.h>
#include <linux/errno.h>
struct pci_dev;
enum vga_switcheroo_handler_flags_t {
	VGA_SWITCHEROO_CAN_SWITCH_DDC = (1 << 0),
	VGA_SWITCHEROO_NEEDS_EDP_CONFIG = (1 << 1)
};
enum vga_switcheroo_state {
	VGA_SWITCHEROO_OFF,
	VGA_SWITCHEROO_ON,
	VGA_SWITCHEROO_NOT_FOUND
};
struct vga_switcheroo_client_ops {
	void (*set_gpu_state)(struct pci_dev *dev, enum vga_switcheroo_state state);
	void (*reprobe)(struct pci_dev *dev);
	bool (*can_switch)(struct pci_dev *dev);
	void (*gpu_bound)(struct pci_dev *dev, int id);
};
#define	vga_switcheroo_register_client(p, o, d)	(-ENODEV)
#define	vga_switcheroo_unregister_client(p)	((void)(p))
#define	vga_switcheroo_process_delayed_switch()	0
#define	vga_switcheroo_handler_flags()		0
#define	vga_switcheroo_client_probe_defer(p)	false
#define	vga_switcheroo_init_domain_pm_ops(d, p)	(-EINVAL)
#define	vga_switcheroo_fini_domain_pm_ops(d)	((void)(d))
static inline void
vga_switcheroo_lock_ddc(struct pci_dev *pdev)
{
}
static inline int
vga_switcheroo_unlock_ddc(struct pci_dev *pdev)
{
	return (-ENODEV);
}
#endif
