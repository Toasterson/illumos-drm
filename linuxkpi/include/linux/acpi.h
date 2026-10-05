/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's ACPI interface. Not supported (CONFIG_ACPI is off): no ACPI
 * handles; acpi_disabled is true.
 */
#ifndef _LINUX_ACPI_H
#define	_LINUX_ACPI_H
#include <linux/types.h>
#define	acpi_disabled		1
#define	ACPI_HANDLE(dev)	((void *)NULL)
typedef void *acpi_handle;
#endif
