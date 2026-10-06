/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <acpi/button.h> (GPL-2.0; independent declaration), as on Linux
 * without CONFIG_ACPI_BUTTON: the lid state is unknown and reported open
 * (1), so panels are never skipped for a closed lid.
 */
#ifndef _ACPI_BUTTON_H
#define	_ACPI_BUTTON_H

static inline int
acpi_lid_open(void)
{
	return (1);
}

#endif
