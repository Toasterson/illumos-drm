/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The Video BIOS Table (VBT) from the ACPI OpRegion, read-only, for the
 * illumos build of i915, which is built without CONFIG_ACPI and so
 * without intel_opregion.c (its ASLE/SWSCI mailboxes and ACPI
 * notifications need ACPI). The system firmware sets the OpRegion up for
 * the graphics device; PCI config register ASLS holds its physical
 * address. The VBT is in mailbox 4, or (OpRegion 2.0 and later) at the
 * address in the ASLE mailbox's RVDA/RVDS. Without it i915 runs on its
 * "VBT missing" defaults; with it, it gets the panel type, LVDS spread
 * spectrum and dithering and the port data Linux would use.
 * intel_bios_get_vbt() calls i915_illumos_opregion_vbt() after
 * intel_opregion_get_vbt() (patch 0013).
 *
 * This only reads: it never writes the OpRegion (no driver-ready flag,
 * no ASLE or backlight requests), so the firmware keeps behaving as for
 * a driver without OpRegion support. The OpRegion is mapped for the copy
 * and unmapped again (memremap(MEMREMAP_WB) of the layer: the OpRegion
 * is ACPI NVS, not kernel RAM). Layout and the choice between RVDA and
 * mailbox 4 as Intel's OpRegion specification describes them and Linux
 * v7.2's intel_opregion_setup() applies them; written for illumos.
 *
 * As Linux:
 * - the boards of Linux's intel_no_opregion_vbt DMI table (a ThinkCentre
 *   A57, whose OpRegion VBT is wrong) get no OpRegion VBT, so i915 looks
 *   in its other places (SPI flash, the PCI ROM) as there;
 * - from OpRegion 2.1 on, RVDA is relative to the OpRegion and must lie
 *   beyond its 8 KiB; one that does not is reported, and used as Linux
 *   uses it (its validation as a VBT still decides).
 *
 * Cache types: the mapping is write-back, as Linux's (intel_opregion_
 * setup() uses memremap(MEMREMAP_WB) too). If the firmware's AML declares
 * an OperationRegion over the OpRegion, illumos' ACPICA maps those frames
 * as well (AcpiOsMapMemory(): psm_map_new(), uncached for frames outside
 * RAM), so the frames can have a write-back and an uncached mapping at
 * the same time, an alias of memory types that Intel's SDM advises
 * against. It is short-lived and read-only here (copy, unmap): no line
 * this mapping caches is ever dirty, so nothing can be written back over
 * what the firmware writes through its mapping (see memremap() in
 * linuxkpi/src/lkpi_page.c).
 */

#include <linux/types.h>
#include <linux/dmi.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/string.h>

#include <drm/drm_print.h>

#include "display/intel_bios.h"
#include "display/intel_display_core.h"
#include "i915_illumos_vbt.h"

#define	ILL_ASLS		0xfc	/* PCI config: OpRegion address */
#define	ILL_OPREGION_SIZE	(8 * 1024)
#define	ILL_SIG			"IntelGraphicsMem"
#define	ILL_SIG_LEN		16
#define	ILL_OVER_MINOR		22	/* header: version, minor */
#define	ILL_OVER_MAJOR		23	/* header: version, major */
#define	ILL_MBOXES		0x58	/* header: mailboxes present */
#define	ILL_MBOX_ASLE		(1U << 2)	/* mailbox 3 */
#define	ILL_MBOX_ASLE_EXT	(1U << 4)	/* mailbox 5 */
#define	ILL_ASLE_OFFSET		0x300
#define	ILL_ASLE_RVDA		(ILL_ASLE_OFFSET + 186)	/* u64 */
#define	ILL_ASLE_RVDS		(ILL_ASLE_OFFSET + 194)	/* u32 */
#define	ILL_VBT_OFFSET		0x400	/* mailbox 4 */
#define	ILL_ASLE_EXT_OFFSET	0x1c00
/* An RVDS beyond this is not a VBT (they are a few KiB). */
#define	ILL_RVDS_MAX		(1024 * 1024)

/* Linux v7.2's intel_no_opregion_vbt: boards whose OpRegion VBT is bad. */
static const struct dmi_system_id ill_no_opregion_vbt[] = {
	{
		.ident = "ThinkCentre A57",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "97027RG"),
		},
	},
	{ }
};

static uint32_t
ill_get32(const uint8_t *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

static uint64_t
ill_get64(const uint8_t *p)
{
	return ((uint64_t)ill_get32(p) | (uint64_t)ill_get32(p + 4) << 32);
}

/*
 * A kmalloc() copy of `size` bytes at physical address `pa`, or NULL (no
 * memory, or memremap() refused the range).
 */
static void *
ill_copy_phys(uint64_t pa, size_t size)
{
	void *map, *buf;

	map = memremap(pa, size, MEMREMAP_WB);
	if (map == NULL)
		return (NULL);
	buf = kmalloc(size, GFP_KERNEL);
	if (buf != NULL)
		memcpy(buf, map, size);
	memunmap(map);
	return (buf);
}

/*
 * The VBT of the OpRegion as a kmalloc() copy the caller frees (as
 * intel_opregion_get_vbt() returns it), with its size in *sizep; NULL if
 * there is no OpRegion or no valid VBT in it.
 */
const void *
i915_illumos_opregion_vbt(struct intel_display *display, size_t *sizep)
{
	struct pci_dev *pdev = to_pci_dev(display->drm->dev);
	uint32_t asls = 0, mboxes, rvds;
	uint64_t rvda;
	uint8_t *op;
	void *vbt = NULL;
	size_t size;

	if (pci_read_config_dword(pdev, ILL_ASLS, &asls) != 0 || asls == 0) {
		drm_dbg_kms(display->drm, "no ACPI OpRegion (ASLS is 0)\n");
		return (NULL);
	}
	op = ill_copy_phys(asls, ILL_OPREGION_SIZE);
	if (op == NULL) {
		drm_dbg_kms(display->drm, "OpRegion at 0x%x not mapped\n",
		    asls);
		return (NULL);
	}
	if (memcmp(op, ILL_SIG, ILL_SIG_LEN) != 0) {
		drm_dbg_kms(display->drm, "OpRegion at 0x%x: no signature\n",
		    asls);
		goto out;
	}
	mboxes = ill_get32(op + ILL_MBOXES);
	drm_dbg_kms(display->drm, "OpRegion %u.%u at 0x%x, mailboxes 0x%x\n",
	    op[ILL_OVER_MAJOR], op[ILL_OVER_MINOR], asls, mboxes);
	if (dmi_check_system(ill_no_opregion_vbt) != 0) {
		drm_dbg_kms(display->drm, "OpRegion VBT skipped (DMI quirk)\n");
		goto out;
	}

	/*
	 * OpRegion 2.0 and later: a VBT outside the region (RVDA/RVDS of
	 * the ASLE mailbox), physical in 2.0, relative to the OpRegion
	 * from 2.1 on. Linux takes it if it is valid and falls back to
	 * mailbox 4 otherwise.
	 */
	if (op[ILL_OVER_MAJOR] >= 2 && (mboxes & ILL_MBOX_ASLE) != 0) {
		rvda = ill_get64(op + ILL_ASLE_RVDA);
		rvds = ill_get32(op + ILL_ASLE_RVDS);
		if (rvda != 0 && rvds != 0 && rvds <= ILL_RVDS_MAX) {
			if (op[ILL_OVER_MAJOR] > 2 || op[ILL_OVER_MINOR] >= 1) {
				if (rvda < ILL_OPREGION_SIZE)
					drm_warn(display->drm, "OpRegion "
					    "RVDA 0x%llx points into the "
					    "OpRegion\n",
					    (unsigned long long)rvda);
				rvda += asls;
			}
			vbt = ill_copy_phys(rvda, rvds);
			if (vbt != NULL &&
			    intel_bios_is_valid_vbt(display, vbt, rvds)) {
				drm_info(display->drm,
				    "VBT from the ACPI OpRegion (RVDA)\n");
				if (sizep != NULL)
					*sizep = rvds;
				goto out;
			}
			drm_dbg_kms(display->drm,
			    "no valid VBT at RVDA 0x%llx (%u bytes)\n",
			    (unsigned long long)rvda, rvds);
			kfree(vbt);
			vbt = NULL;
		}
	}

	/*
	 * Mailbox 4. It may extend into the ASLE extension mailbox's area
	 * when that mailbox is absent (Linux allows it for boards whose VBT
	 * does).
	 */
	size = ((mboxes & ILL_MBOX_ASLE_EXT) != 0 ?
	    ILL_ASLE_EXT_OFFSET : ILL_OPREGION_SIZE) - ILL_VBT_OFFSET;
	if (intel_bios_is_valid_vbt(display, op + ILL_VBT_OFFSET, size)) {
		vbt = kmemdup(op + ILL_VBT_OFFSET, size, GFP_KERNEL);
		if (vbt != NULL) {
			drm_info(display->drm,
			    "VBT from the ACPI OpRegion (mailbox 4)\n");
			if (sizep != NULL)
				*sizep = size;
		}
	} else {
		drm_dbg_kms(display->drm, "no valid VBT in mailbox 4\n");
	}
out:
	kfree(op);
	return (vbt);
}
