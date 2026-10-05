/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The LinuxKPI layer's interface version. The layer's structures are
 * compiled into the drivers (struct pci_dev, struct device, ...), so a
 * driver must run with the layer it was built against: its _init(9E)
 * calls linuxkpi_check(name, LINUXKPI_ABI) first and fails with the
 * returned errno (EINVAL on a mismatch, which is also logged). Raise
 * LINUXKPI_ABI with every change to a layout or an interface drivers use.
 */
#ifndef _LKPI_VERSION_H
#define	_LKPI_VERSION_H

#define	LINUXKPI_ABI	1

extern int linuxkpi_check(const char *driver, unsigned int abi);

#endif
