/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's PCI driver interface over the illumos DDI (see <linux/pci.h>).
 *
 * A layer PCI device is a struct pci_dev in a private container, made by
 * lkpi_pci_attach() for the node the driver attaches to: identity from
 * the node's properties, bus/device/function from the first "reg" entry,
 * config space through pci_config_setup(9F), BARs from "reg" (the index
 * for ddi_regs_map_setup(9F)) and "assigned-addresses" (the bus address).
 * Attached devices are kept on a list (ioremap() finds BARs by physical
 * address through lkpi_pci_phys_lookup()).
 *
 * Other PCI functions (the host bridge, the ISA bridge) are found by
 * walking the device tree; they become "foreign" wrappers: read-only
 * config space, no BARs, no interrupts. Such a wrapper reaches a node the
 * driver does not own, so writes are refused (-EPERM) except to a host
 * bridge, where i915 enables MCHBAR (i915_gmch.c); those are logged.
 *
 * PCI domains: illumos x86 numbers buses in segment 0; every device is
 * in domain 0.
 *
 * Config accessors return 0 or a negative errno (Linux returns positive
 * PCIBIOS codes; callers test for non-zero). A failed read stores ~0.
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

/*
 * Holds on a node (sys/sunndi.h, not included: the prologue's renames).
 * Non-DDI: a foreign wrapper keeps a config handle on a node its driver
 * does not own; the hold keeps the node from being removed meanwhile.
 */
extern void ndi_hold_devi(dev_info_t *);
extern void ndi_rele_devi(dev_info_t *);

const struct bus_type pci_bus_type = {
	.name = "pci"
};

/* Empty until an Intel display device attaches (resource_size() == 0). */
struct resource intel_graphics_stolen_res = {
	.name = "Graphics Stolen Memory",
	.start = 0,
	.end = ~(resource_size_t)0,
	.flags = IORESOURCE_MEM
};

/* A layer PCI device and its illumos state. */
struct lkpi_pdev {
	struct pci_dev lp_pdev;
	struct list_head lp_link;	/* lkpi_pci_devs (attached only) */
	kmutex_t lp_lock;		/* lp_maps */
	struct list_head lp_maps;	/* struct lkpi_iomap */
	bool lp_held;			/* foreign: ndi_hold_devi() */
};

/* One pci_iomap() mapping. */
struct lkpi_iomap {
	struct list_head lm_link;
	void *lm_addr;
	ddi_acc_handle_t lm_h;
};

#define	LKPI_PCI_MAX_DRIVERS	8
#define	LKPI_PCI_CFG_SIZE	4096	/* PCI Express config space */

static kmutex_t lkpi_pci_lock;		/* the two tables below */
static struct list_head lkpi_pci_devs;
static struct pci_driver *lkpi_pci_drivers[LKPI_PCI_MAX_DRIVERS];
static struct pci_dev *lkpi_stolen_owner;
static bool lkpi_pci_power_logged;

static struct lkpi_pdev *
lkpi_pdev(const struct pci_dev *pdev)
{
	return (container_of(pdev, struct lkpi_pdev, lp_pdev));
}

int
lkpi_pci_init(void)
{
	int ret;

	(mutex_init)(&lkpi_pci_lock, NULL, MUTEX_DRIVER, NULL);
	INIT_LIST_HEAD(&lkpi_pci_devs);
	ret = lkpi_irq_init();
	if (ret != 0)
		(mutex_destroy)(&lkpi_pci_lock);
	return (ret);
}

void
lkpi_pci_fini(void)
{
	/* The module cannot unload while a driver using it is loaded. */
	WARN_ON(!list_empty(&lkpi_pci_devs));
	lkpi_irq_fini();
	(mutex_destroy)(&lkpi_pci_lock);
}

/* --- drivers ------------------------------------------------------------ */

int
pci_register_driver(struct pci_driver *drv)
{
	int i, slot = -1;

	if (drv == NULL || drv->name == NULL)
		return (-EINVAL);
	if (drv->driver.name == NULL)
		drv->driver.name = drv->name;
	mutex_enter(&lkpi_pci_lock);
	for (i = 0; i < LKPI_PCI_MAX_DRIVERS; i++) {
		if (lkpi_pci_drivers[i] == drv) {
			mutex_exit(&lkpi_pci_lock);
			return (-EBUSY);
		}
		if (lkpi_pci_drivers[i] == NULL && slot < 0)
			slot = i;
	}
	if (slot >= 0)
		lkpi_pci_drivers[slot] = drv;
	mutex_exit(&lkpi_pci_lock);
	return (slot >= 0 ? 0 : -ENOSPC);
}

void
pci_unregister_driver(struct pci_driver *drv)
{
	int i;

	mutex_enter(&lkpi_pci_lock);
	for (i = 0; i < LKPI_PCI_MAX_DRIVERS; i++) {
		if (lkpi_pci_drivers[i] == drv)
			lkpi_pci_drivers[i] = NULL;
	}
	mutex_exit(&lkpi_pci_lock);
}

struct pci_driver *
lkpi_pci_driver_lookup(const char *name)
{
	struct pci_driver *drv = NULL;
	int i;

	if (name == NULL)
		return (NULL);
	mutex_enter(&lkpi_pci_lock);
	for (i = 0; i < LKPI_PCI_MAX_DRIVERS; i++) {
		if (lkpi_pci_drivers[i] != NULL &&
		    strcmp(lkpi_pci_drivers[i]->name, name) == 0) {
			drv = lkpi_pci_drivers[i];
			break;
		}
	}
	mutex_exit(&lkpi_pci_lock);
	return (drv);
}

const struct pci_device_id *
pci_match_id(const struct pci_device_id *ids, struct pci_dev *dev)
{
	if (ids == NULL || dev == NULL)
		return (NULL);
	/* The table ends with an all-zero entry. */
	for (; ids->vendor != 0 || ids->subvendor != 0 || ids->class_mask != 0;
	    ids++) {
		if (ids->vendor != (u32)PCI_ANY_ID && ids->vendor != dev->vendor)
			continue;
		if (ids->device != (u32)PCI_ANY_ID && ids->device != dev->device)
			continue;
		if (ids->subvendor != (u32)PCI_ANY_ID &&
		    ids->subvendor != dev->subsystem_vendor)
			continue;
		if (ids->subdevice != (u32)PCI_ANY_ID &&
		    ids->subdevice != dev->subsystem_device)
			continue;
		if (((ids->class ^ dev->class) & ids->class_mask) != 0)
			continue;
		return (ids);
	}
	return (NULL);
}

/* --- config space ------------------------------------------------------ */

static int
lkpi_cfg_check(const struct pci_dev *dev, int where, int size)
{
	if (dev == NULL || dev->lkpi_cfg == NULL)
		return (-ENODEV);
	if (where < 0 || where > LKPI_PCI_CFG_SIZE - size ||
	    (where & (size - 1)) != 0)
		return (-EINVAL);
	return (0);
}

static int
lkpi_cfg_read(const struct pci_dev *dev, int where, int size, u32 *val)
{
	int ret;

	ret = lkpi_cfg_check(dev, where, size);
	if (ret != 0) {
		*val = ~0U;
		return (ret);
	}
	switch (size) {
	case 1:
		*val = pci_config_get8(dev->lkpi_cfg, where);
		break;
	case 2:
		*val = pci_config_get16(dev->lkpi_cfg, where);
		break;
	default:
		*val = pci_config_get32(dev->lkpi_cfg, where);
		break;
	}
	return (0);
}

static int
lkpi_cfg_write(const struct pci_dev *dev, int where, int size, u32 val)
{
	int ret;

	ret = lkpi_cfg_check(dev, where, size);
	if (ret != 0)
		return (ret);
	if (dev->lkpi_foreign) {
		if ((dev->class >> 8) != PCI_CLASS_BRIDGE_HOST) {
			cmn_err(CE_WARN, "linuxkpi: refused config write "
			    "(offset 0x%x, value 0x%x) to %s, a device the "
			    "driver does not own", where, val, pci_name(dev));
			return (-EPERM);
		}
		cmn_err(CE_NOTE, "!linuxkpi: config write (offset 0x%x, "
		    "%d bytes, value 0x%x) to host bridge %s", where, size, val,
		    pci_name(dev));
	}
	switch (size) {
	case 1:
		pci_config_put8(dev->lkpi_cfg, where, (uint8_t)val);
		break;
	case 2:
		pci_config_put16(dev->lkpi_cfg, where, (uint16_t)val);
		break;
	default:
		pci_config_put32(dev->lkpi_cfg, where, val);
		break;
	}
	return (0);
}

int
pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{
	u32 v;
	int ret;

	ret = lkpi_cfg_read(dev, where, 1, &v);
	*val = (u8)v;
	return (ret);
}

int
pci_read_config_word(const struct pci_dev *dev, int where, u16 *val)
{
	u32 v;
	int ret;

	ret = lkpi_cfg_read(dev, where, 2, &v);
	*val = (u16)v;
	return (ret);
}

int
pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val)
{
	return (lkpi_cfg_read(dev, where, 4, val));
}

int
pci_write_config_byte(const struct pci_dev *dev, int where, u8 val)
{
	return (lkpi_cfg_write(dev, where, 1, val));
}

int
pci_write_config_word(const struct pci_dev *dev, int where, u16 val)
{
	return (lkpi_cfg_write(dev, where, 2, val));
}

int
pci_write_config_dword(const struct pci_dev *dev, int where, u32 val)
{
	return (lkpi_cfg_write(dev, where, 4, val));
}

/* --- capabilities ------------------------------------------------------ */

u8
pci_find_capability(struct pci_dev *dev, int cap)
{
	u16 status;
	u8 pos, id;
	int ttl = 48;	/* (256 - 64) / 4: bounds a looping list */

	if (pci_read_config_word(dev, PCI_STATUS, &status) != 0 ||
	    (status & PCI_STATUS_CAP_LIST) == 0)
		return (0);
	if (pci_read_config_byte(dev, PCI_CAPABILITY_LIST, &pos) != 0)
		return (0);
	while (ttl-- > 0) {
		pos &= ~3;
		if (pos < 0x40)
			break;
		if (pci_read_config_byte(dev, pos + PCI_CAP_LIST_ID, &id) != 0 ||
		    id == 0xff)
			break;
		if (id == cap)
			return (pos);
		if (pci_read_config_byte(dev, pos + PCI_CAP_LIST_NEXT,
		    &pos) != 0)
			break;
	}
	return (0);
}

u16
pci_find_ext_capability(struct pci_dev *dev, int cap)
{
	u32 hdr;
	int pos = 0x100;
	int ttl = (4096 - 256) / 8;

	if (!pci_is_pcie(dev))
		return (0);
	if (pci_read_config_dword(dev, pos, &hdr) != 0 || hdr == 0 ||
	    hdr == ~0U)
		return (0);
	while (ttl-- > 0) {
		if ((int)(hdr & 0xffff) == cap)
			return ((u16)pos);
		pos = (hdr >> 20) & 0xffc;
		if (pos < 0x100)
			break;
		if (pci_read_config_dword(dev, pos, &hdr) != 0)
			break;
	}
	return (0);
}

/*
 * PCI Express capability registers. A device without the capability reads
 * zero and ignores writes, as Linux does for unimplemented registers.
 */
int
pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val)
{
	*val = 0;
	if (pos < 0 || (pos & 1) != 0)
		return (-EINVAL);
	if (!pci_is_pcie(dev))
		return (0);
	return (pci_read_config_word(dev, dev->pcie_cap + pos, val));
}

int
pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val)
{
	*val = 0;
	if (pos < 0 || (pos & 3) != 0)
		return (-EINVAL);
	if (!pci_is_pcie(dev))
		return (0);
	return (pci_read_config_dword(dev, dev->pcie_cap + pos, val));
}

int
pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val)
{
	if (pos < 0 || (pos & 1) != 0)
		return (-EINVAL);
	if (!pci_is_pcie(dev))
		return (0);
	return (pci_write_config_word(dev, dev->pcie_cap + pos, val));
}

int
pcie_capability_clear_and_set_word(struct pci_dev *dev, int pos, u16 clear,
    u16 set)
{
	u16 v;
	int ret;

	ret = pcie_capability_read_word(dev, pos, &v);
	if (ret != 0 || !pci_is_pcie(dev))
		return (ret);
	v = (v & ~clear) | set;
	return (pcie_capability_write_word(dev, pos, v));
}

/* --- enabling, bus mastering, power ------------------------------------- */

/*
 * The PCI nexus enables memory decoding when it configures the node, so
 * there is nothing to enable; a disabled decoder is reported.
 */
int
pci_enable_device(struct pci_dev *dev)
{
	u16 cmd;

	if (dev == NULL || dev->lkpi_foreign)
		return (-EINVAL);
	if (pci_read_config_word(dev, PCI_COMMAND, &cmd) != 0)
		return (-EIO);
	if ((cmd & PCI_COMMAND_MEMORY) == 0)
		dev_warn(&dev->dev, "memory decoding is disabled\n");
	return (0);
}

/* Stops bus mastering, as Linux does; decoding stays the nexus' business. */
void
pci_disable_device(struct pci_dev *dev)
{
	if (dev == NULL || dev->lkpi_foreign)
		return;
	pci_clear_master(dev);
}

static void
lkpi_pci_command(struct pci_dev *dev, u16 clear, u16 set)
{
	u16 cmd, ncmd;

	if (dev == NULL || dev->lkpi_foreign)
		return;
	if (pci_read_config_word(dev, PCI_COMMAND, &cmd) != 0)
		return;
	ncmd = (cmd & ~clear) | set;
	if (ncmd != cmd)
		(void) pci_write_config_word(dev, PCI_COMMAND, ncmd);
}

void
pci_set_master(struct pci_dev *dev)
{
	lkpi_pci_command(dev, 0, PCI_COMMAND_MASTER);
}

void
pci_clear_master(struct pci_dev *dev)
{
	lkpi_pci_command(dev, PCI_COMMAND_MASTER, 0);
}

/*
 * Power states are not supported: the device stays in D0. Other states
 * succeed without effect (logged once), so that suspend paths proceed.
 */
int
pci_set_power_state(struct pci_dev *dev, pci_power_t state)
{
	if (state == PCI_D0)
		return (0);
	if (!lkpi_pci_power_logged) {
		lkpi_pci_power_logged = true;
		if (dev != NULL) {
			dev_info(&dev->dev, "PCI power state D%d not "
			    "supported, staying in D0\n", state);
		}
	}
	return (0);
}

/* Config space is not saved or restored (no power transitions). */
int
pci_save_state(struct pci_dev *dev)
{
	(void) dev;
	return (0);
}

void
pci_restore_state(struct pci_dev *dev)
{
	(void) dev;
}

/* The nexus owns the address space: regions are always "available". */
int
pci_request_regions(struct pci_dev *dev, const char *name)
{
	(void) name;
	return (dev == NULL || dev->lkpi_foreign ? -EINVAL : 0);
}

void
pci_release_regions(struct pci_dev *dev)
{
	(void) dev;
}

/* --- resources ----------------------------------------------------------- */

struct resource *
pci_find_resource(struct pci_dev *dev, struct resource *res)
{
	int i;

	if (dev == NULL || res == NULL)
		return (NULL);
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		struct resource *r = &dev->resource[i];

		if (r->flags != 0 && r->end > r->start &&
		    resource_contains(r, res))
			return (r);
	}
	return (NULL);
}

/*
 * Bus address allocation is the nexus' job (it assigns BARs from its
 * "ranges"); the layer cannot allocate. i915 then leaves MCHBAR as the
 * firmware set it up (enabled by the BIOS on the supported platforms).
 */
int
pci_bus_alloc_resource(struct pci_bus *bus, struct resource *res,
    resource_size_t size, resource_size_t align, resource_size_t min,
    unsigned long type_mask,
    resource_size_t (*alignf)(void *, const struct resource *,
    resource_size_t, resource_size_t), void *alignf_data)
{
	(void) bus;
	(void) res;
	(void) size;
	(void) align;
	(void) min;
	(void) type_mask;
	(void) alignf;
	(void) alignf_data;
	return (-ENODEV);
}

void
pci_assign_unassigned_bus_resources(struct pci_bus *bus)
{
	(void) bus;
}

resource_size_t
pcibios_align_resource(void *data, const struct resource *res,
    resource_size_t size, resource_size_t align)
{
	(void) data;
	(void) size;
	(void) align;
	return (res->start);
}

/* Resizable BARs are not supported: the nexus fixed the BAR sizes. */
int
pci_resize_resource(struct pci_dev *dev, int i, int size)
{
	(void) dev;
	(void) i;
	(void) size;
	return (-ENOTSUPP);
}

int
pci_rebar_get_possible_sizes(struct pci_dev *pdev, int bar)
{
	(void) pdev;
	(void) bar;
	return (0);
}

/* --- BAR mappings ------------------------------------------------------ */

static void __iomem *
lkpi_pci_map(struct pci_dev *dev, int bar, unsigned long offset,
    unsigned long maxlen, uint8_t order)
{
	struct lkpi_pdev *lp;
	struct lkpi_iomap *m;
	ddi_device_acc_attr_t attr;
	ddi_acc_handle_t h;
	caddr_t addr;
	off_t size, len;
	int regnum;

	if (dev == NULL || dev->lkpi_foreign || bar < 0 ||
	    bar >= PCI_STD_NUM_BARS)
		return (NULL);
	regnum = dev->lkpi_regnum[bar];
	if (regnum < 0)
		return (NULL);
	/* I/O space has no CPU mapping that readl()/writel() could use. */
	if ((dev->resource[bar].flags & IORESOURCE_IO) != 0) {
		dev_warn(&dev->dev, "BAR %d is I/O space, not mapped\n", bar);
		return (NULL);
	}
	if (ddi_dev_regsize(dev->dev.lkpi_dip, regnum, &size) != DDI_SUCCESS ||
	    size <= 0 || offset >= (unsigned long)size)
		return (NULL);
	len = size - (off_t)offset;
	if (maxlen != 0 && maxlen < (unsigned long)len)
		len = (off_t)maxlen;

	bzero(&attr, sizeof (attr));
	attr.devacc_attr_version = DDI_DEVICE_ATTR_V0;
	attr.devacc_attr_endian_flags = DDI_STRUCTURE_LE_ACC;
	attr.devacc_attr_dataorder = order;
	if (ddi_regs_map_setup(dev->dev.lkpi_dip, regnum, &addr,
	    (offset_t)offset, (offset_t)len, &attr, &h) != DDI_SUCCESS) {
		dev_err(&dev->dev, "cannot map BAR %d (offset 0x%lx, "
		    "length 0x%lx)\n", bar, offset, (unsigned long)len);
		return (NULL);
	}

	lp = lkpi_pdev(dev);
	m = kmem_alloc(sizeof (*m), KM_SLEEP);
	m->lm_addr = addr;
	m->lm_h = h;
	mutex_enter(&lp->lp_lock);
	list_add(&m->lm_link, &lp->lp_maps);
	mutex_exit(&lp->lp_lock);
	return ((void __iomem *)addr);
}

void __iomem *
pci_iomap_range(struct pci_dev *dev, int bar, unsigned long offset,
    unsigned long maxlen)
{
	return (lkpi_pci_map(dev, bar, offset, maxlen, DDI_STRICTORDER_ACC));
}

void __iomem *
pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen)
{
	return (lkpi_pci_map(dev, bar, 0, maxlen, DDI_STRICTORDER_ACC));
}

void __iomem *
pci_iomap_wc(struct pci_dev *dev, int bar, unsigned long maxlen)
{
	return (lkpi_pci_map(dev, bar, 0, maxlen, DDI_MERGING_OK_ACC));
}

/*
 * Unmaps a pci_iomap() mapping; any other address is passed to iounmap(),
 * as Linux's pci_iounmap() does for MMIO.
 */
void
pci_iounmap(struct pci_dev *dev, void __iomem *addr)
{
	struct lkpi_pdev *lp;
	struct lkpi_iomap *m, *found = NULL;

	if (addr == NULL)
		return;
	if (dev != NULL && !dev->lkpi_foreign) {
		lp = lkpi_pdev(dev);
		mutex_enter(&lp->lp_lock);
		list_for_each_entry(m, &lp->lp_maps, lm_link) {
			if (m->lm_addr == (void *)addr) {
				list_del(&m->lm_link);
				found = m;
				break;
			}
		}
		mutex_exit(&lp->lp_lock);
	}
	if (found == NULL) {
		iounmap(addr);
		return;
	}
	ddi_regs_map_free(&found->lm_h);
	kmem_free(found, sizeof (*found));
}

static void
lkpi_pci_unmap_all(struct lkpi_pdev *lp)
{
	struct lkpi_iomap *m;

	for (;;) {
		mutex_enter(&lp->lp_lock);
		if (list_empty(&lp->lp_maps)) {
			mutex_exit(&lp->lp_lock);
			return;
		}
		m = list_first_entry(&lp->lp_maps, struct lkpi_iomap, lm_link);
		list_del(&m->lm_link);
		mutex_exit(&lp->lp_lock);
		ddi_regs_map_free(&m->lm_h);
		kmem_free(m, sizeof (*m));
	}
}

int
lkpi_pci_phys_lookup(resource_size_t phys, size_t size, dev_info_t **dipp,
    uint_t *regnump, off_t *offp)
{
	struct lkpi_pdev *lp;
	resource_size_t last;
	int bar, ret = ENXIO;

	if (size == 0)
		size = 1;
	if (phys + (size - 1) < phys)
		return (ENXIO);
	last = phys + (size - 1);
	mutex_enter(&lkpi_pci_lock);
	list_for_each_entry(lp, &lkpi_pci_devs, lp_link) {
		struct pci_dev *pdev = &lp->lp_pdev;

		for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
			struct resource *r = &pdev->resource[bar];

			if ((r->flags & IORESOURCE_MEM) == 0 ||
			    pdev->lkpi_regnum[bar] < 0 || r->end <= r->start)
				continue;
			if (phys < r->start || last > r->end)
				continue;
			*dipp = pdev->dev.lkpi_dip;
			*regnump = (uint_t)pdev->lkpi_regnum[bar];
			*offp = (off_t)(phys - r->start);
			ret = 0;
			goto out;
		}
	}
out:
	mutex_exit(&lkpi_pci_lock);
	return (ret);
}

ddi_dma_attr_t *
lkpi_dev_dma_attr(struct device *dev)
{
	if (dev == NULL || dev->lkpi_dip == NULL)
		return (NULL);
	if (dev->bus == &pci_bus_type && to_pci_dev(dev)->lkpi_foreign)
		return (NULL);
	return (&dev->lkpi_dma_attr);
}

/* --- making a struct pci_dev for a node -------------------------------- */

/* The bus and devfn of a PCI node, from its first "reg" entry. */
static bool
lkpi_pci_node_bdf(dev_info_t *dip, uint_t *busp, uint_t *devfnp)
{
	int *data;
	uint_t n;
	uint_t hi;

	if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    "reg", &data, &n) != DDI_PROP_SUCCESS)
		return (false);
	if (n < 5 || (n % 5) != 0) {
		ddi_prop_free(data);
		return (false);
	}
	hi = (uint_t)data[0];
	ddi_prop_free(data);
	if ((hi & PCI_ADDR_MASK) != PCI_ADDR_CONFIG || PCI_REG_REG_G(hi) != 0)
		return (false);
	*busp = PCI_REG_BUS_G(hi);
	*devfnp = PCI_DEVFN(PCI_REG_DEV_G(hi), PCI_REG_FUNC_G(hi));
	return (true);
}

static int
lkpi_pci_prop(dev_info_t *dip, const char *name)
{
	return (ddi_prop_get_int(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    (char *)name, -1));
}

/* The BAR index of a config register number, or -1. */
static int
lkpi_pci_reg_bar(uint_t reg)
{
	if (reg >= PCI_CONF_BASE0 && reg <= PCI_CONF_BASE5 &&
	    ((reg - PCI_CONF_BASE0) & 3) == 0)
		return ((int)(reg - PCI_CONF_BASE0) / 4);
	if (reg == PCI_CONF_ROM)
		return (PCI_ROM_RESOURCE);
	return (-1);
}

/*
 * The BARs: "reg" gives each BAR's register-set index (for
 * ddi_regs_map_setup()), "assigned-addresses" its bus address and size.
 */
static void
lkpi_pci_bars(struct pci_dev *pdev, dev_info_t *dip)
{
	pci_regspec_t *rs;
	int *data;
	uint_t n, i;
	int bar;

	if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    "reg", &data, &n) == DDI_PROP_SUCCESS) {
		rs = (pci_regspec_t *)(void *)data;
		for (i = 1; i < n / 5; i++) {
			if ((rs[i].pci_phys_hi & PCI_ADDR_MASK) ==
			    PCI_ADDR_CONFIG)
				continue;
			bar = lkpi_pci_reg_bar(PCI_REG_REG_G(rs[i].pci_phys_hi));
			if (bar >= 0 && pdev->lkpi_regnum[bar] < 0)
				pdev->lkpi_regnum[bar] = (int)i;
		}
		ddi_prop_free(data);
	}

	if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    "assigned-addresses", &data, &n) != DDI_PROP_SUCCESS)
		return;
	rs = (pci_regspec_t *)(void *)data;
	for (i = 0; i < n / 5; i++) {
		uint_t hi = rs[i].pci_phys_hi;
		uint64_t start, size;
		struct resource *r;

		bar = lkpi_pci_reg_bar(PCI_REG_REG_G(hi));
		if (bar < 0)
			continue;
		start = ((uint64_t)rs[i].pci_phys_mid << 32) |
		    rs[i].pci_phys_low;
		size = ((uint64_t)rs[i].pci_size_hi << 32) |
		    rs[i].pci_size_low;
		if (size == 0)
			continue;
		r = &pdev->resource[bar];
		r->start = start;
		r->end = start + size - 1;
		r->name = pdev->dev.lkpi_name;
		switch (hi & PCI_ADDR_MASK) {
		case PCI_ADDR_IO:
			r->flags = IORESOURCE_IO;
			break;
		case PCI_ADDR_MEM64:
			r->flags = IORESOURCE_MEM | IORESOURCE_MEM_64;
			break;
		default:
			r->flags = IORESOURCE_MEM;
			break;
		}
		if ((hi & PCI_PREFETCH_B) != 0)
			r->flags |= IORESOURCE_PREFETCH;
	}
	ddi_prop_free(data);
}

static void
lkpi_pci_free(struct lkpi_pdev *lp)
{
	struct pci_dev *pdev = &lp->lp_pdev;
	dev_info_t *dip = pdev->dev.lkpi_dip;

	lkpi_device_fini(&pdev->dev);
	if (pdev->lkpi_cfg != NULL)
		pci_config_teardown(&pdev->lkpi_cfg);
	(mutex_destroy)(&lp->lp_lock);
	if (lp->lp_held)
		ndi_rele_devi(dip);
	kmem_free(lp, sizeof (*lp));
}

/*
 * A struct pci_dev for a PCI node: identity, config space, capabilities
 * and (for the driver's own node) BARs. NULL if the node is not a PCI
 * function or its config space cannot be set up.
 */
static struct lkpi_pdev *
lkpi_pci_new(dev_info_t *dip, bool foreign)
{
	struct lkpi_pdev *lp;
	struct pci_dev *pdev;
	uint_t bus, devfn;
	char name[32];
	int i, vendor;

	vendor = lkpi_pci_prop(dip, "vendor-id");
	if (vendor < 0 || !lkpi_pci_node_bdf(dip, &bus, &devfn))
		return (NULL);

	lp = kmem_zalloc(sizeof (*lp), KM_SLEEP);
	(mutex_init)(&lp->lp_lock, NULL, MUTEX_DRIVER, NULL);
	INIT_LIST_HEAD(&lp->lp_maps);
	INIT_LIST_HEAD(&lp->lp_link);
	pdev = &lp->lp_pdev;

	pdev->vendor = (unsigned short)vendor;
	pdev->device = (unsigned short)lkpi_pci_prop(dip, "device-id");
	pdev->subsystem_vendor =
	    (unsigned short)lkpi_pci_prop(dip, "subsystem-vendor-id");
	pdev->subsystem_device =
	    (unsigned short)lkpi_pci_prop(dip, "subsystem-id");
	pdev->revision = (u8)lkpi_pci_prop(dip, "revision-id");
	pdev->class = (unsigned int)lkpi_pci_prop(dip, "class-code") &
	    0xffffff;
	pdev->devfn = devfn;
	pdev->lkpi_bus.number = (unsigned char)bus;
	pdev->lkpi_bus.domain_nr = 0;
	pdev->bus = &pdev->lkpi_bus;
	pdev->lkpi_foreign = foreign;
	pdev->dma_mask = DMA_BIT_MASK(64);
	for (i = 0; i <= PCI_NUM_RESOURCES; i++)
		pdev->lkpi_regnum[i] = -1;

	(void) (snprintf)(name, sizeof (name), "%04x:%02x:%02x.%x", 0, bus,
	    PCI_SLOT(devfn), PCI_FUNC(devfn));
	lkpi_device_init(&pdev->dev, dip, name);
	pdev->dev.bus = &pci_bus_type;
	pdev->dev.dma_mask = &pdev->dma_mask;
	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(64);

	if (pci_config_setup(dip, &pdev->lkpi_cfg) != DDI_SUCCESS) {
		pdev->lkpi_cfg = NULL;
		cmn_err(CE_WARN, "linuxkpi: %s: pci_config_setup failed", name);
		lkpi_pci_free(lp);
		return (NULL);
	}
	pdev->hdr_type = pci_config_get8(pdev->lkpi_cfg, PCI_CONF_HEADER) &
	    0x7f;
	pdev->pcie_cap = pci_find_capability(pdev, PCI_CAP_ID_EXP);
	pdev->msi_cap = pci_find_capability(pdev, PCI_CAP_ID_MSI);
	pdev->msix_cap = pci_find_capability(pdev, PCI_CAP_ID_MSIX);

	if (!foreign)
		lkpi_pci_bars(pdev, dip);
	return (lp);
}

/* --- Intel graphics stolen memory -------------------------------------- */

/*
 * Device IDs per graphics generation, from the i915 PCI ID lists of
 * include/drm/intel/pciids.h (MIT): gen6 = Sandy Bridge; gen7 = Ivy
 * Bridge, Haswell, Valleyview; gen8 = Broadwell (not Cherryview, whose
 * GMS encoding differs); gen9 = Skylake, Kaby Lake, Amber Lake, Coffee
 * Lake, Whiskey Lake, Comet Lake.
 */
static const uint16_t lkpi_intel_gen6[] = {
	0x0102, 0x010a, 0x0112, 0x0122, 0x0106, 0x0116, 0x0126
};

static const uint16_t lkpi_intel_gen7[] = {
	/* Ivy Bridge */
	0x0156, 0x0166, 0x0152, 0x015a, 0x0162, 0x016a,
	/* Haswell */
	0x0a02, 0x0a06, 0x0a0a, 0x0a0b, 0x0a0e, 0x0402, 0x0406, 0x040a,
	0x040b, 0x040e, 0x0c02, 0x0c06, 0x0c0a, 0x0c0b, 0x0c0e, 0x0d02,
	0x0d06, 0x0d0a, 0x0d0b, 0x0d0e, 0x0a12, 0x0a16, 0x0a1a, 0x0a1b,
	0x0a1e, 0x0412, 0x0416, 0x041a, 0x041b, 0x041e, 0x0c12, 0x0c16,
	0x0c1a, 0x0c1b, 0x0c1e, 0x0d12, 0x0d16, 0x0d1a, 0x0d1b, 0x0d1e,
	0x0a22, 0x0a26, 0x0a2a, 0x0a2b, 0x0a2e, 0x0422, 0x0426, 0x042a,
	0x042b, 0x042e, 0x0c22, 0x0c26, 0x0c2a, 0x0c2b, 0x0c2e, 0x0d22,
	0x0d26, 0x0d2a, 0x0d2b, 0x0d2e,
	/* Valleyview */
	0x0f30, 0x0f31, 0x0f32, 0x0f33
};

static const uint16_t lkpi_intel_gen8[] = {
	0x1606, 0x160b, 0x160e, 0x1602, 0x160a, 0x160d, 0x1616, 0x161b,
	0x161e, 0x1612, 0x161a, 0x161d, 0x1626, 0x162b, 0x162e, 0x1622,
	0x162a, 0x162d, 0x1636, 0x163b, 0x163e, 0x1632, 0x163a, 0x163d
};

static const uint16_t lkpi_intel_gen9[] = {
	/* Skylake */
	0x1906, 0x1913, 0x190e, 0x1915, 0x1902, 0x190a, 0x190b, 0x1917,
	0x1916, 0x1921, 0x191e, 0x1912, 0x191a, 0x191b, 0x191d, 0x1923,
	0x1926, 0x1927, 0x192a, 0x192b, 0x192d, 0x1932, 0x193a, 0x193b,
	0x193d,
	/* Kaby Lake, Amber Lake */
	0x5906, 0x5913, 0x590e, 0x5915, 0x5902, 0x5908, 0x590a, 0x590b,
	0x5916, 0x5921, 0x591e, 0x5912, 0x5917, 0x591a, 0x591b, 0x591d,
	0x5926, 0x5923, 0x5927, 0x593b, 0x591c, 0x87c0,
	/* Coffee Lake, Amber Lake */
	0x3e90, 0x3e93, 0x3e99, 0x3e91, 0x3e92, 0x3e96, 0x3e98, 0x3e9a,
	0x3e9c, 0x3e94, 0x3e9b, 0x3ea9, 0x3ea5, 0x3ea6, 0x3ea7, 0x3ea8,
	0x87ca,
	/* Comet Lake */
	0x9ba2, 0x9ba4, 0x9ba5, 0x9ba8, 0x9bc2, 0x9bc4, 0x9bc5, 0x9bc6,
	0x9bc8, 0x9be6, 0x9bf6, 0x9b21, 0x9baa, 0x9bac, 0x9b41, 0x9bca,
	0x9bcc,
	/* Whiskey Lake */
	0x3ea1, 0x3ea4, 0x3ea0, 0x3ea3, 0x3ea2
};

static bool
lkpi_id_in(uint16_t id, const uint16_t *ids, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (ids[i] == id)
			return (true);
	}
	return (false);
}

static int
lkpi_intel_gen(uint16_t id)
{
	if (lkpi_id_in(id, lkpi_intel_gen6, ARRAY_SIZE(lkpi_intel_gen6)))
		return (6);
	if (lkpi_id_in(id, lkpi_intel_gen7, ARRAY_SIZE(lkpi_intel_gen7)))
		return (7);
	if (lkpi_id_in(id, lkpi_intel_gen8, ARRAY_SIZE(lkpi_intel_gen8)))
		return (8);
	if (lkpi_id_in(id, lkpi_intel_gen9, ARRAY_SIZE(lkpi_intel_gen9)))
		return (9);
	return (0);
}

/* The graphics device's config registers (Intel datasheets). */
#define	LKPI_INTEL_GGC		0x50	/* GMCH Graphics Control, 16 bit */
#define	LKPI_INTEL_BDSM		0x5c	/* Base of Data Stolen Memory */
#define	LKPI_INTEL_BDSM_MASK	0xfff00000U	/* bits 31:20, 1 MiB */

/*
 * Records the stolen memory of an Intel integrated GPU in
 * intel_graphics_stolen_res, as Linux's x86 early quirk does at boot:
 * the base from BDSM, the size from the Graphics Mode Select field of
 * GGC, whose position and unit depend on the generation:
 *	gen6, gen7:	GMS = GGC[7:3], GMS * 32 MiB
 *	gen8:		GMS = GGC[15:8], GMS * 32 MiB
 *	gen9 (9.5):	GMS = GGC[15:8], GMS * 32 MiB below 0xf0, else
 *			(GMS - 0xf0 + 1) * 4 MiB
 * Other devices leave the resource empty (i915 then uses no stolen
 * memory).
 */
static void
lkpi_intel_stolen(struct pci_dev *pdev)
{
	uint64_t base, size = 0;
	u16 ggc;
	u32 bdsm;
	uint_t gms;
	int gen;

	if (pdev->vendor != PCI_VENDOR_ID_INTEL ||
	    (pdev->class >> 16) != PCI_BASE_CLASS_DISPLAY)
		return;
	gen = lkpi_intel_gen(pdev->device);
	if (gen == 0) {
		dev_info(&pdev->dev, "stolen memory of device 0x%04x unknown, "
		    "none used\n", pdev->device);
		return;
	}
	if (pci_read_config_word(pdev, LKPI_INTEL_GGC, &ggc) != 0 ||
	    pci_read_config_dword(pdev, LKPI_INTEL_BDSM, &bdsm) != 0)
		return;

	switch (gen) {
	case 6:
	case 7:
		gms = (ggc >> 3) & 0x1f;
		size = (uint64_t)gms * 32 * 1024 * 1024;
		break;
	case 8:
		gms = (ggc >> 8) & 0xff;
		size = (uint64_t)gms * 32 * 1024 * 1024;
		break;
	default:
		gms = (ggc >> 8) & 0xff;
		if (gms < 0xf0)
			size = (uint64_t)gms * 32 * 1024 * 1024;
		else
			size = (uint64_t)(gms - 0xf0 + 1) * 4 * 1024 * 1024;
		break;
	}
	base = bdsm & LKPI_INTEL_BDSM_MASK;
	if (base == 0 || size == 0) {
		dev_info(&pdev->dev, "no graphics stolen memory (GGC 0x%04x, "
		    "BDSM 0x%08x)\n", ggc, bdsm);
		return;
	}

	mutex_enter(&lkpi_pci_lock);
	if (lkpi_stolen_owner == NULL) {
		intel_graphics_stolen_res.start = base;
		intel_graphics_stolen_res.end = base + size - 1;
		lkpi_stolen_owner = pdev;
	}
	mutex_exit(&lkpi_pci_lock);
	dev_info(&pdev->dev, "graphics stolen memory 0x%llx-0x%llx "
	    "(%llu MiB, gen%d)\n", (unsigned long long)base,
	    (unsigned long long)(base + size - 1),
	    (unsigned long long)(size >> 20), gen);
}

/* --- attach and detach ------------------------------------------------- */

struct pci_dev *
lkpi_pci_dev_of(dev_info_t *dip)
{
	struct lkpi_pdev *lp;
	struct pci_dev *pdev = NULL;

	if (dip == NULL)
		return (NULL);
	mutex_enter(&lkpi_pci_lock);
	list_for_each_entry(lp, &lkpi_pci_devs, lp_link) {
		if (lp->lp_pdev.dev.lkpi_dip == dip) {
			pdev = &lp->lp_pdev;
			break;
		}
	}
	mutex_exit(&lkpi_pci_lock);
	return (pdev);
}

/*
 * Undoes an attach: the driver's managed resources, its interrupts and
 * BAR mappings, then the device itself.
 */
static void
lkpi_pci_release(struct lkpi_pdev *lp)
{
	struct pci_dev *pdev = &lp->lp_pdev;

	lkpi_devres_release_all(&pdev->dev);
	pci_free_irq_vectors(pdev);
	lkpi_pci_unmap_all(lp);
	ddi_set_driver_private(pdev->dev.lkpi_dip, NULL);

	mutex_enter(&lkpi_pci_lock);
	list_del(&lp->lp_link);
	if (lkpi_stolen_owner == pdev) {
		lkpi_stolen_owner = NULL;
		intel_graphics_stolen_res.start = 0;
		intel_graphics_stolen_res.end = ~(resource_size_t)0;
	}
	mutex_exit(&lkpi_pci_lock);

	pdev->driver = NULL;
	lkpi_pci_free(lp);
}

int
lkpi_pci_attach(dev_info_t *dip, ddi_attach_cmd_t cmd, struct pci_driver *drv)
{
	struct lkpi_pdev *lp;
	struct pci_dev *pdev;
	const struct pci_device_id *id;
	int ret;

	if (cmd != DDI_ATTACH || dip == NULL || drv == NULL ||
	    drv->probe == NULL)
		return (DDI_FAILURE);
	if (lkpi_pci_dev_of(dip) != NULL) {
		(dev_err)(dip, CE_WARN, "linuxkpi: node already attached");
		return (DDI_FAILURE);
	}
	lp = lkpi_pci_new(dip, false);
	if (lp == NULL) {
		(dev_err)(dip, CE_WARN, "linuxkpi: not a usable PCI function");
		return (DDI_FAILURE);
	}
	pdev = &lp->lp_pdev;
	if (drv->driver.name == NULL)
		drv->driver.name = drv->name;
	pdev->driver = drv;
	pdev->dev.driver = &drv->driver;

	id = pci_match_id(drv->id_table, pdev);
	if (id == NULL) {
		dev_err(&pdev->dev, "device %04x:%04x not in the ID table of "
		    "%s\n", pdev->vendor, pdev->device,
		    drv->name != NULL ? drv->name : "the driver");
		lkpi_pci_free(lp);
		return (DDI_FAILURE);
	}

	lkpi_intel_stolen(pdev);

	/* Listed before probe: ioremap() during probe finds the BARs. */
	mutex_enter(&lkpi_pci_lock);
	list_add_tail(&lp->lp_link, &lkpi_pci_devs);
	mutex_exit(&lkpi_pci_lock);
	ddi_set_driver_private(dip, pdev);

	ret = drv->probe(pdev, id);
	if (ret < 0) {
		dev_err(&pdev->dev, "probe failed: %d\n", ret);
		lkpi_pci_release(lp);
		return (DDI_FAILURE);
	}
	if (ret > 0)
		dev_warn(&pdev->dev, "probe returned %d, taken as success\n",
		    ret);
	return (DDI_SUCCESS);
}

int
lkpi_pci_detach(dev_info_t *dip, ddi_detach_cmd_t cmd, struct pci_driver *drv)
{
	struct pci_dev *pdev;

	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);
	pdev = lkpi_pci_dev_of(dip);
	if (pdev == NULL)
		return (DDI_FAILURE);
	if (drv != NULL && pdev->driver != drv)
		dev_warn(&pdev->dev, "detach with another driver than "
		    "attach\n");
	if (pdev->driver != NULL && pdev->driver->remove != NULL)
		pdev->driver->remove(pdev);
	lkpi_pci_release(lkpi_pdev(pdev));
	return (DDI_SUCCESS);
}

/* --- other functions: device tree lookups ------------------------------ */

enum lkpi_pci_find_how {
	LKPI_FIND_BDF,
	LKPI_FIND_ID,
	LKPI_FIND_CLASS
};

struct lkpi_pci_find {
	enum lkpi_pci_find_how lf_how;
	uint_t lf_bus;
	uint_t lf_devfn;
	uint_t lf_vendor;
	uint_t lf_device;
	uint_t lf_class;
	dev_info_t *lf_from;	/* continue after this node */
	bool lf_seen;		/* lf_from passed (or none) */
	dev_info_t *lf_found;	/* held */
};

static int
lkpi_pci_find_cb(dev_info_t *dip, void *arg)
{
	struct lkpi_pci_find *f = arg;
	uint_t bus, devfn;
	int v;

	v = lkpi_pci_prop(dip, "vendor-id");
	if (v < 0)
		return (DDI_WALK_CONTINUE);
	switch (f->lf_how) {
	case LKPI_FIND_BDF:
		if (!lkpi_pci_node_bdf(dip, &bus, &devfn) ||
		    bus != f->lf_bus || devfn != f->lf_devfn)
			return (DDI_WALK_CONTINUE);
		break;
	case LKPI_FIND_ID:
		if (f->lf_vendor != (uint_t)PCI_ANY_ID &&
		    (uint_t)v != f->lf_vendor)
			return (DDI_WALK_CONTINUE);
		if (f->lf_device != (uint_t)PCI_ANY_ID &&
		    (uint_t)lkpi_pci_prop(dip, "device-id") != f->lf_device)
			return (DDI_WALK_CONTINUE);
		if (!lkpi_pci_node_bdf(dip, &bus, &devfn))
			return (DDI_WALK_CONTINUE);
		break;
	case LKPI_FIND_CLASS:
		if (((uint_t)lkpi_pci_prop(dip, "class-code") & 0xffffff) !=
		    f->lf_class || !lkpi_pci_node_bdf(dip, &bus, &devfn))
			return (DDI_WALK_CONTINUE);
		break;
	}
	if (!f->lf_seen) {
		if (dip == f->lf_from)
			f->lf_seen = true;
		return (DDI_WALK_CONTINUE);
	}
	ndi_hold_devi(dip);
	f->lf_found = dip;
	return (DDI_WALK_TERMINATE);
}

/*
 * Walks the device tree for a PCI node. The attached layer device of the
 * node is returned as is; any other node becomes a foreign wrapper,
 * released by lkpi_pci_dev_put().
 *
 * ddi_walk_devs(9F) is documented as not callable from autoconfiguration
 * entry points, but i915 looks up the host bridge from probe, i.e. from
 * attach(9E). The walk enters nodes top-down from the root, the same
 * order the framework uses, and the attaching thread already owns the
 * nodes above its own; the callback takes no other locks.
 */
static struct pci_dev *
lkpi_pci_find(struct lkpi_pci_find *f)
{
	struct lkpi_pdev *lp;
	struct pci_dev *pdev;

	f->lf_found = NULL;
	ddi_walk_devs(ddi_root_node(), lkpi_pci_find_cb, f);
	if (f->lf_found == NULL)
		return (NULL);
	pdev = lkpi_pci_dev_of(f->lf_found);
	if (pdev != NULL) {
		ndi_rele_devi(f->lf_found);
		return (pdev);
	}
	lp = lkpi_pci_new(f->lf_found, true);
	if (lp == NULL) {
		ndi_rele_devi(f->lf_found);
		return (NULL);
	}
	lp->lp_held = true;
	return (&lp->lp_pdev);
}

struct pci_dev *
pci_get_domain_bus_and_slot(int domain, unsigned int bus, unsigned int devfn)
{
	struct lkpi_pci_find f;
	struct lkpi_pdev *lp;
	struct pci_dev *pdev = NULL;

	if (domain != 0 || bus > 0xff || devfn > 0xff)
		return (NULL);
	mutex_enter(&lkpi_pci_lock);
	list_for_each_entry(lp, &lkpi_pci_devs, lp_link) {
		if (lp->lp_pdev.bus->number == bus &&
		    lp->lp_pdev.devfn == devfn) {
			pdev = &lp->lp_pdev;
			break;
		}
	}
	mutex_exit(&lkpi_pci_lock);
	if (pdev != NULL)
		return (pdev);

	bzero(&f, sizeof (f));
	f.lf_how = LKPI_FIND_BDF;
	f.lf_bus = bus;
	f.lf_devfn = devfn;
	f.lf_seen = true;
	return (lkpi_pci_find(&f));
}

/* As on Linux, the reference to "from" is dropped. */
struct pci_dev *
pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from)
{
	struct lkpi_pci_find f;
	struct pci_dev *pdev;

	bzero(&f, sizeof (f));
	f.lf_how = LKPI_FIND_ID;
	f.lf_vendor = vendor;
	f.lf_device = device;
	f.lf_from = from != NULL ? from->dev.lkpi_dip : NULL;
	f.lf_seen = (from == NULL);
	pdev = lkpi_pci_find(&f);
	lkpi_pci_dev_put(from);
	return (pdev);
}

struct pci_dev *
pci_get_class(unsigned int cls, struct pci_dev *from)
{
	struct lkpi_pci_find f;
	struct pci_dev *pdev;

	bzero(&f, sizeof (f));
	f.lf_how = LKPI_FIND_CLASS;
	f.lf_class = cls & 0xffffff;
	f.lf_from = from != NULL ? from->dev.lkpi_dip : NULL;
	f.lf_seen = (from == NULL);
	pdev = lkpi_pci_find(&f);
	lkpi_pci_dev_put(from);
	return (pdev);
}

/* Frees a foreign wrapper; attached devices are not reference counted. */
void
lkpi_pci_dev_put(struct pci_dev *dev)
{
	if (dev == NULL || !dev->lkpi_foreign)
		return;
	lkpi_pci_free(lkpi_pdev(dev));
}

/* --- config access by bus and devfn ------------------------------------ */

static struct pci_dev *
lkpi_pci_bus_dev(struct pci_bus *bus, unsigned int devfn)
{
	if (bus == NULL)
		return (NULL);
	return (pci_get_domain_bus_and_slot(bus->domain_nr, bus->number,
	    devfn));
}

static int
lkpi_pci_bus_read(struct pci_bus *bus, unsigned int devfn, int where,
    int size, u32 *val)
{
	struct pci_dev *pdev;
	int ret;

	pdev = lkpi_pci_bus_dev(bus, devfn);
	if (pdev == NULL) {
		*val = ~0U;
		return (-ENODEV);
	}
	ret = lkpi_cfg_read(pdev, where, size, val);
	lkpi_pci_dev_put(pdev);
	return (ret);
}

static int
lkpi_pci_bus_write(struct pci_bus *bus, unsigned int devfn, int where,
    int size, u32 val)
{
	struct pci_dev *pdev;
	int ret;

	pdev = lkpi_pci_bus_dev(bus, devfn);
	if (pdev == NULL)
		return (-ENODEV);
	ret = lkpi_cfg_write(pdev, where, size, val);
	lkpi_pci_dev_put(pdev);
	return (ret);
}

int
pci_bus_read_config_byte(struct pci_bus *bus, unsigned int devfn, int where,
    u8 *val)
{
	u32 v;
	int ret;

	ret = lkpi_pci_bus_read(bus, devfn, where, 1, &v);
	*val = (u8)v;
	return (ret);
}

int
pci_bus_read_config_word(struct pci_bus *bus, unsigned int devfn, int where,
    u16 *val)
{
	u32 v;
	int ret;

	ret = lkpi_pci_bus_read(bus, devfn, where, 2, &v);
	*val = (u16)v;
	return (ret);
}

int
pci_bus_read_config_dword(struct pci_bus *bus, unsigned int devfn, int where,
    u32 *val)
{
	return (lkpi_pci_bus_read(bus, devfn, where, 4, val));
}

int
pci_bus_write_config_byte(struct pci_bus *bus, unsigned int devfn, int where,
    u8 val)
{
	return (lkpi_pci_bus_write(bus, devfn, where, 1, val));
}

int
pci_bus_write_config_word(struct pci_bus *bus, unsigned int devfn, int where,
    u16 val)
{
	return (lkpi_pci_bus_write(bus, devfn, where, 2, val));
}

int
pci_bus_write_config_dword(struct pci_bus *bus, unsigned int devfn, int where,
    u32 val)
{
	return (lkpi_pci_bus_write(bus, devfn, where, 4, val));
}
