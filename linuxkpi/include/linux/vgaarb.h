/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's VGA arbiter (include/linux/vgaarb.h, GPL-2.0; independent
 * declarations), as on Linux without CONFIG_VGA_ARB: illumos has no VGA
 * arbiter. The legacy VGA ranges (I/O 0x3b0-0x3df, memory 0xa0000-0xbffff)
 * are reached by whichever device decodes them; the new-stack boot
 * environments exclude drv/vgatext, so the display driver is their only
 * user. vga_get() succeeds at once, vga_put() and the decoding changes do
 * nothing, client registration succeeds without a client.
 */
#ifndef _LINUX_VGAARB_H
#define	_LINUX_VGAARB_H

#include <linux/types.h>

struct pci_dev;

#define	VGA_RSRC_NONE		0x00
#define	VGA_RSRC_LEGACY_IO	0x01
#define	VGA_RSRC_LEGACY_MEM	0x02
#define	VGA_RSRC_LEGACY_MASK	(VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM)
#define	VGA_RSRC_NORMAL_IO	0x04
#define	VGA_RSRC_NORMAL_MEM	0x08

static inline void
vga_set_legacy_decoding(struct pci_dev *pdev, unsigned int decodes)
{
}

static inline int
vga_get(struct pci_dev *pdev, unsigned int rsrc, int interruptible)
{
	return (0);
}

static inline int
vga_get_interruptible(struct pci_dev *pdev, unsigned int rsrc)
{
	return (vga_get(pdev, rsrc, 1));
}

static inline int
vga_get_uninterruptible(struct pci_dev *pdev, unsigned int rsrc)
{
	return (vga_get(pdev, rsrc, 0));
}

static inline void
vga_put(struct pci_dev *pdev, unsigned int rsrc)
{
}

static inline int
vga_client_register(struct pci_dev *pdev,
    unsigned int (*set_decode)(struct pci_dev *pdev, bool state))
{
	return (0);
}

static inline void
vga_client_unregister(struct pci_dev *pdev)
{
}

static inline struct pci_dev *
vga_default_device(void)
{
	return (NULL);
}

#endif
