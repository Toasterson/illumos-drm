/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's DMI (SMBIOS) system identification (include/linux/dmi.h and the
 * DMI part of include/linux/mod_devicetable.h, GPL-2.0; independent
 * declarations; src/lkpi_dmi.c reads the strings from illumos' SMBIOS
 * snapshot). Drivers match quirk tables against the firmware's system,
 * board and BIOS strings:
 *
 * dmi_get_system_info(field)
 *	the string of `field` (enum dmi_field), or NULL if the firmware has
 *	none. The strings are read once, when misc/linuxkpi loads.
 * dmi_check_system(list)
 *	walks the table (terminated by an entry whose ident and first match
 *	are empty) and calls each matching entry's callback (if any) until
 *	one returns non-zero; returns the number of matches counted.
 * dmi_first_match(list)
 *	the first matching entry, or NULL.
 * dmi_match(field, str)
 *	whether the field contains str.
 *
 * An entry matches when every used slot (slot != DMI_NONE) matches: its
 * substr is a substring of the field's string (exact_match: equal).
 */
#ifndef _LINUX_DMI_H
#define	_LINUX_DMI_H

#include <linux/types.h>
#include <linux/list.h>

enum dmi_field {
	DMI_NONE,
	DMI_BIOS_VENDOR,
	DMI_BIOS_VERSION,
	DMI_BIOS_DATE,
	DMI_BIOS_RELEASE,
	DMI_EC_FIRMWARE_RELEASE,
	DMI_SYS_VENDOR,
	DMI_PRODUCT_NAME,
	DMI_PRODUCT_VERSION,
	DMI_PRODUCT_SERIAL,
	DMI_PRODUCT_UUID,
	DMI_PRODUCT_SKU,
	DMI_PRODUCT_FAMILY,
	DMI_BOARD_VENDOR,
	DMI_BOARD_NAME,
	DMI_BOARD_VERSION,
	DMI_BOARD_SERIAL,
	DMI_BOARD_ASSET_TAG,
	DMI_CHASSIS_VENDOR,
	DMI_CHASSIS_TYPE,
	DMI_CHASSIS_VERSION,
	DMI_CHASSIS_SERIAL,
	DMI_CHASSIS_ASSET_TAG,
	DMI_STRING_MAX,
	DMI_OEM_STRING
};

struct dmi_strmatch {
	unsigned char slot:7;
	unsigned char exact_match:1;
	char substr[79];
};

struct dmi_system_id {
	int (*callback)(const struct dmi_system_id *);
	const char *ident;
	struct dmi_strmatch matches[4];
	void *driver_data;
};

#define	DMI_MATCH(a, b)		{ .slot = (a), .substr = (b) }
#define	DMI_EXACT_MATCH(a, b)	{ .slot = (a), .substr = (b), .exact_match = 1 }

extern const char *dmi_get_system_info(int field);
extern int dmi_check_system(const struct dmi_system_id *list);
extern const struct dmi_system_id *dmi_first_match(
    const struct dmi_system_id *list);
extern bool dmi_match(enum dmi_field f, const char *str);

#endif
