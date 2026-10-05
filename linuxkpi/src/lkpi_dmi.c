/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's DMI system identification (<linux/dmi.h>) from illumos' SMBIOS
 * snapshot (ksmbios, read by the kernel at boot): the strings of the
 * system, base board, BIOS and chassis structures are copied once at
 * module load (lkpi_dmi_init()) into the field table Linux's quirk tables
 * are matched against (dmi_check_system(), dmi_first_match()). A field
 * the firmware does not provide (or a system without SMBIOS) is NULL and
 * matches nothing, as on Linux. Not provided: DMI_BIOS_RELEASE,
 * DMI_EC_FIRMWARE_RELEASE, DMI_PRODUCT_UUID and OEM strings (no user).
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/dmi.h>
#include <linux/export.h>
#include <sys/smbios.h>
#include "lkpi_internal.h"

static char *lkpi_dmi_ident[DMI_STRING_MAX];

static void
lkpi_dmi_set(int field, const char *s)
{
	if (s == NULL || *s == '\0' || lkpi_dmi_ident[field] != NULL)
		return;
	lkpi_dmi_ident[field] = kstrdup(s, GFP_KERNEL);
}

/* The common strings (vendor, name, version, serial, asset tag) of id. */
static void
lkpi_dmi_common(smbios_hdl_t *shp, id_t id, int vendor, int name,
    int version, int serial, int asset)
{
	smbios_info_t info;

	if (id == -1 || smbios_info_common(shp, id, &info) != 0)
		return;
	lkpi_dmi_set(vendor, info.smbi_manufacturer);
	if (name != DMI_NONE)
		lkpi_dmi_set(name, info.smbi_product);
	lkpi_dmi_set(version, info.smbi_version);
	lkpi_dmi_set(serial, info.smbi_serial);
	if (asset != DMI_NONE)
		lkpi_dmi_set(asset, info.smbi_asset);
}

int
lkpi_dmi_init(void)
{
	smbios_hdl_t *shp = ksmbios;
	smbios_system_t sys;
	smbios_bios_t bios;
	smbios_struct_t st;
	smbios_chassis_t ch;
	char type[8];
	id_t id;

	if (shp == NULL)
		return (0);
	if (smbios_info_bios(shp, &bios) != -1) {
		lkpi_dmi_set(DMI_BIOS_VENDOR, bios.smbb_vendor);
		lkpi_dmi_set(DMI_BIOS_VERSION, bios.smbb_version);
		lkpi_dmi_set(DMI_BIOS_DATE, bios.smbb_reldate);
	}
	if ((id = smbios_info_system(shp, &sys)) != -1) {
		lkpi_dmi_common(shp, id, DMI_SYS_VENDOR, DMI_PRODUCT_NAME,
		    DMI_PRODUCT_VERSION, DMI_PRODUCT_SERIAL, DMI_NONE);
		lkpi_dmi_set(DMI_PRODUCT_SKU, sys.smbs_sku);
		lkpi_dmi_set(DMI_PRODUCT_FAMILY, sys.smbs_family);
	}
	if (smbios_lookup_type(shp, SMB_TYPE_BASEBOARD, &st) == 0) {
		lkpi_dmi_common(shp, st.smbstr_id, DMI_BOARD_VENDOR,
		    DMI_BOARD_NAME, DMI_BOARD_VERSION, DMI_BOARD_SERIAL,
		    DMI_BOARD_ASSET_TAG);
	}
	if (smbios_lookup_type(shp, SMB_TYPE_CHASSIS, &st) == 0) {
		lkpi_dmi_common(shp, st.smbstr_id, DMI_CHASSIS_VENDOR,
		    DMI_NONE, DMI_CHASSIS_VERSION, DMI_CHASSIS_SERIAL,
		    DMI_CHASSIS_ASSET_TAG);
		if (smbios_info_chassis(shp, st.smbstr_id, &ch) == 0) {
			(void) snprintf(type, sizeof (type), "%u",
			    (unsigned int)ch.smbc_type);
			lkpi_dmi_set(DMI_CHASSIS_TYPE, type);
		}
	}
	return (0);
}

void
lkpi_dmi_fini(void)
{
	int i;

	for (i = 0; i < DMI_STRING_MAX; i++) {
		kfree(lkpi_dmi_ident[i]);
		lkpi_dmi_ident[i] = NULL;
	}
}

const char *
dmi_get_system_info(int field)
{
	if (field <= DMI_NONE || field >= DMI_STRING_MAX)
		return (NULL);
	return (lkpi_dmi_ident[field]);
}
EXPORT_SYMBOL(dmi_get_system_info);

/*
 * Whether every slot of the entry matches, up to the first unused one:
 * the field contains the substring (exact_match: equals it); a field the
 * firmware lacks matches nothing.
 */
static bool
lkpi_dmi_matches(const struct dmi_system_id *d)
{
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(d->matches); i++) {
		const struct dmi_strmatch *m = &d->matches[i];
		const char *s;

		if (m->slot == DMI_NONE)
			break;
		s = dmi_get_system_info(m->slot);
		if (s == NULL)
			return (false);
		if (m->exact_match ? strcmp(s, m->substr) != 0 :
		    strstr(s, m->substr) == NULL)
			return (false);
	}
	return (true);
}

/* A table ends with an entry whose first slot is unused. */
#define	LKPI_DMI_END(d)	((d)->matches[0].slot == DMI_NONE)

int
dmi_check_system(const struct dmi_system_id *list)
{
	const struct dmi_system_id *d;
	int count = 0;

	for (d = list; d != NULL && !LKPI_DMI_END(d); d++) {
		if (!lkpi_dmi_matches(d))
			continue;
		count++;
		if (d->callback != NULL && d->callback(d) != 0)
			break;
	}
	return (count);
}
EXPORT_SYMBOL(dmi_check_system);

const struct dmi_system_id *
dmi_first_match(const struct dmi_system_id *list)
{
	const struct dmi_system_id *d;

	for (d = list; d != NULL && !LKPI_DMI_END(d); d++) {
		if (lkpi_dmi_matches(d))
			return (d);
	}
	return (NULL);
}
EXPORT_SYMBOL(dmi_first_match);

bool
dmi_match(enum dmi_field f, const char *str)
{
	const char *info = dmi_get_system_info(f);

	if (info == NULL || str == NULL)
		return (info == str);
	return (strcmp(info, str) == 0);
}
EXPORT_SYMBOL(dmi_match);
