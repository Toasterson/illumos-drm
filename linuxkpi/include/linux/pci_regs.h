/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * PCI configuration-space register offsets and bits with Linux's names
 * (Linux's include/uapi/linux/pci_regs.h is GPL-2.0 WITH
 * Linux-syscall-note; the values are the PCI specification's). Names
 * illumos' <sys/pci.h> also defines are only defined when missing.
 */
#ifndef _LINUX_PCI_REGS_H
#define	_LINUX_PCI_REGS_H

#ifndef	PCI_VENDOR_ID
#define	PCI_VENDOR_ID		0x00
#endif
#ifndef	PCI_DEVICE_ID
#define	PCI_DEVICE_ID		0x02
#endif
#define	PCI_COMMAND		0x04
#define	PCI_COMMAND_IO		0x1
#define	PCI_COMMAND_MEMORY	0x2
#define	PCI_COMMAND_MASTER	0x4
#define	PCI_COMMAND_INTX_DISABLE	0x400
#define	PCI_STATUS		0x06
#define	PCI_STATUS_CAP_LIST	0x10
#define	PCI_CLASS_REVISION	0x08
#define	PCI_REVISION_ID		0x08
#define	PCI_CLASS_PROG		0x09
#define	PCI_CLASS_DEVICE	0x0a
#define	PCI_HEADER_TYPE		0x0e
#define	PCI_BASE_ADDRESS_0	0x10
#define	PCI_BASE_ADDRESS_1	0x14
#define	PCI_BASE_ADDRESS_2	0x18
#define	PCI_BASE_ADDRESS_3	0x1c
#define	PCI_BASE_ADDRESS_4	0x20
#define	PCI_BASE_ADDRESS_5	0x24
#define	PCI_BASE_ADDRESS_SPACE	0x01
#define	PCI_BASE_ADDRESS_MEM_TYPE_64	0x04
#define	PCI_BASE_ADDRESS_MEM_PREFETCH	0x08
#define	PCI_BASE_ADDRESS_MEM_MASK	(~0x0fUL)
#define	PCI_SUBSYSTEM_VENDOR_ID	0x2c
#define	PCI_SUBSYSTEM_ID	0x2e
#define	PCI_ROM_ADDRESS		0x30
#define	PCI_CAPABILITY_LIST	0x34
#define	PCI_INTERRUPT_LINE	0x3c
#define	PCI_INTERRUPT_PIN	0x3d
#define	PCI_CAP_LIST_ID		0
#define	PCI_CAP_LIST_NEXT	1
#ifndef	PCI_CAP_ID_PM
#define	PCI_CAP_ID_PM		0x01
#endif
#ifndef	PCI_CAP_ID_AGP
#define	PCI_CAP_ID_AGP		0x02
#endif
#ifndef	PCI_CAP_ID_VPD
#define	PCI_CAP_ID_VPD		0x03
#endif
#ifndef	PCI_CAP_ID_MSI
#define	PCI_CAP_ID_MSI		0x05
#endif
#ifndef	PCI_CAP_ID_VNDR
#define	PCI_CAP_ID_VNDR		0x09
#endif
#define	PCI_CAP_ID_EXP		0x10
#ifndef	PCI_CAP_ID_MSIX
#define	PCI_CAP_ID_MSIX		0x11
#endif
#define	PCI_EXT_CAP_ID_REBAR	0x15
#define	PCI_EXT_CAP_ID_VNDR	0x0b
#define	PCI_PM_CTRL		4
#define	PCI_PM_CTRL_STATE_MASK	0x0003
#define	PCI_EXP_FLAGS		0x02
#define	PCI_EXP_DEVCAP		0x04
#define	PCI_EXP_DEVCTL		0x08
#define	PCI_EXP_DEVSTA		0x0a
#define	PCI_EXP_LNKCAP		0x0c
#define	PCI_EXP_LNKCAP_SLS	0x0000000f
#define	PCI_EXP_LNKCAP_MLW	0x000003f0
#define	PCI_EXP_LNKCTL		0x10
#define	PCI_EXP_LNKSTA		0x12
#define	PCI_EXP_LNKSTA_CLS	0x000f
#define	PCI_EXP_LNKSTA_NLW	0x03f0
#define	PCI_EXP_DEVCTL2		0x28
#define	PCI_EXP_LNKCAP2		0x2c
#define	PCI_EXP_LNKCTL2		0x30
#define	PCI_MSI_FLAGS		0x02
#define	PCI_MSI_FLAGS_ENABLE	0x0001
#define	PCI_MSI_ADDRESS_LO	0x04
#define	PCI_GMCH_CTRL_LKPI	0x52
#define	PCI_REBAR_CAP		4
#define	PCI_REBAR_CTRL		8
#define	PCI_REBAR_CTRL_BAR_SIZE	0x00001f00
#define	PCI_REBAR_CTRL_BAR_SHIFT	8

/* PCI-to-PCI bridge control register (type 1 header) */
#define	PCI_BRIDGE_CONTROL		0x3e
#define	PCI_BRIDGE_CTL_PARITY		0x01
#define	PCI_BRIDGE_CTL_SERR		0x02
#define	PCI_BRIDGE_CTL_ISA		0x04
#define	PCI_BRIDGE_CTL_VGA		0x08
#define	PCI_BRIDGE_CTL_MASTER_ABORT	0x20
#define	PCI_BRIDGE_CTL_BUS_RESET	0x40

#endif
