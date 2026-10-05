/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's PCI interface over the illumos DDI (include/linux/pci.h,
 * GPL-2.0; independent implementation in src/lkpi_pci.c).
 *
 * A struct pci_dev is made by the layer's attach glue for the driver's
 * own node: identity from the "vendor-id"/"device-id"/... properties,
 * config space through pci_config_setup(9F), BARs from "reg" and
 * "assigned-addresses" (pci_iomap() maps a BAR with
 * ddi_regs_map_setup(9F)), interrupts through ddi_intr_*(9F)
 * (<linux/interrupt.h>). Other functions' config space
 * (pci_get_domain_bus_and_slot(), e.g. the host bridge) is reached by
 * finding their node in the device tree; such devices are read-only
 * wrappers without BARs or interrupts. Power states and save/restore of
 * config space are not supported (always D0; 0 returned).
 */
#ifndef _LINUX_PCI_H
#define	_LINUX_PCI_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/pci_ids.h>
#include <linux/pci_regs.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>

#define	PCI_NUM_RESOURCES	6
#define	PCI_STD_NUM_BARS	6
#define	PCI_ROM_RESOURCE	6
#define	PCI_DEVFN(slot, func)	((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define	PCI_SLOT(devfn)		(((devfn) >> 3) & 0x1f)
#define	PCI_FUNC(devfn)		((devfn) & 0x07)
#define	PCI_ANY_ID		(~0)
#define	PCI_D0			0
#define	PCI_D1			1
#define	PCI_D2			2
#define	PCI_D3hot		3
#define	PCI_D3cold		4
#define	PCI_UNKNOWN		5
#define	PCI_POWER_ERROR		(-1)
#define	PCI_IRQ_INTX		(1 << 0)
#define	PCI_IRQ_LEGACY		PCI_IRQ_INTX
#define	PCI_IRQ_MSI		(1 << 1)
#define	PCI_IRQ_MSIX		(1 << 2)
#define	PCI_IRQ_AFFINITY	(1 << 3)
#define	PCI_IRQ_ALL_TYPES	(PCI_IRQ_INTX | PCI_IRQ_MSI | PCI_IRQ_MSIX)

typedef unsigned int pci_channel_state_t;
typedef unsigned short pci_dev_flags_t;

struct pci_bus {
	struct pci_bus *parent;
	struct pci_dev *self;
	unsigned char number;
	unsigned char primary;
	int domain_nr;
};

struct lkpi_pci_irq;

struct pci_dev {
	struct device dev;
	struct pci_bus *bus;
	struct pci_bus lkpi_bus;
	unsigned int devfn;
	unsigned short vendor;
	unsigned short device;
	unsigned short subsystem_vendor;
	unsigned short subsystem_device;
	unsigned int class;
	u8 revision;
	u8 hdr_type;
	u8 pcie_cap;
	u8 msi_cap;
	u8 msix_cap;
	int irq;
	unsigned int msi_enabled:1;
	unsigned int msix_enabled:1;
	unsigned int is_virtfn:1;
	unsigned int no_64bit_msi:1;
	struct resource resource[PCI_NUM_RESOURCES + 1];
	struct pci_driver *driver;
	u64 dma_mask;
	pci_dev_flags_t dev_flags;
	/* illumos */
	ddi_acc_handle_t lkpi_cfg;
	int lkpi_regnum[PCI_NUM_RESOURCES + 1];
	bool lkpi_foreign;
	struct lkpi_pci_irq *lkpi_irq;
};

struct pci_driver {
	const char *name;
	const struct pci_device_id *id_table;
	int (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
	void (*remove)(struct pci_dev *dev);
	void (*shutdown)(struct pci_dev *dev);
	int (*suspend)(struct pci_dev *dev, pm_message_t state);
	int (*resume)(struct pci_dev *dev);
	const struct pci_error_handlers *err_handler;
	struct device_driver driver;
};

#define	to_pci_dev(d)		container_of((d), struct pci_dev, dev)
#define	pci_get_drvdata(p)	dev_get_drvdata(&(p)->dev)
#define	pci_set_drvdata(p, v)	dev_set_drvdata(&(p)->dev, (v))
#define	pci_name(p)		dev_name(&(p)->dev)
#define	pci_domain_nr(bus)	((bus)->domain_nr)
#define	pci_dev_id(p)		((u16)(((p)->bus->number << 8) | (p)->devfn))
#define	pci_resource_start(p, bar)	((p)->resource[(bar)].start)
#define	pci_resource_end(p, bar)	((p)->resource[(bar)].end)
#define	pci_resource_flags(p, bar)	((p)->resource[(bar)].flags)
#define	pci_resource_len(p, bar) \
	((p)->resource[(bar)].end == 0 ? 0 : resource_size(&(p)->resource[(bar)]))
#define	pci_resource_n(p, bar)	(&(p)->resource[(bar)])
#define	pci_is_root_bus(b)	((b)->parent == NULL)
#define	pci_is_pcie(p)		((p)->pcie_cap != 0)
#define	pci_pcie_cap(p)		((p)->pcie_cap)
#define	pci_dev_put(p)		lkpi_pci_dev_put(p)
#define	pci_dev_get(p)		(p)
#define	pci_dev_present(ids)	0
#define	pci_is_enabled(p)	true
#define	pci_dev_run_wake(p)	false
#define	pci_ignore_hotplug(p)	((void)(p))
#define	pci_d3cold_disable(p)	((void)(p))
#define	pci_d3cold_enable(p)	((void)(p))
#define	pci_dev_is_disconnected(p)	false

extern int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val);
extern int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val);
extern int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val);
extern int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val);
extern int pci_write_config_word(const struct pci_dev *dev, int where, u16 val);
extern int pci_write_config_dword(const struct pci_dev *dev, int where, u32 val);
extern int pci_bus_read_config_byte(struct pci_bus *bus, unsigned int devfn,
    int where, u8 *val);
extern int pci_bus_read_config_word(struct pci_bus *bus, unsigned int devfn,
    int where, u16 *val);
extern int pci_bus_read_config_dword(struct pci_bus *bus, unsigned int devfn,
    int where, u32 *val);
extern int pci_bus_write_config_byte(struct pci_bus *bus, unsigned int devfn,
    int where, u8 val);
extern int pci_bus_write_config_word(struct pci_bus *bus, unsigned int devfn,
    int where, u16 val);
extern int pci_bus_write_config_dword(struct pci_bus *bus, unsigned int devfn,
    int where, u32 val);
extern u8 pci_find_capability(struct pci_dev *dev, int cap);
extern u16 pci_find_ext_capability(struct pci_dev *dev, int cap);
extern int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val);
extern int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val);
extern int pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val);
extern int pcie_capability_clear_and_set_word(struct pci_dev *dev, int pos,
    u16 clear, u16 set);
extern int pci_enable_device(struct pci_dev *dev);
extern void pci_disable_device(struct pci_dev *dev);
extern void pci_set_master(struct pci_dev *dev);
extern void pci_clear_master(struct pci_dev *dev);
extern int pci_set_power_state(struct pci_dev *dev, pci_power_t state);
extern int pci_save_state(struct pci_dev *dev);
extern void pci_restore_state(struct pci_dev *dev);
extern int pci_request_regions(struct pci_dev *dev, const char *name);
extern void pci_release_regions(struct pci_dev *dev);
extern void __iomem *pci_iomap(struct pci_dev *dev, int bar,
    unsigned long maxlen);
extern void __iomem *pci_iomap_range(struct pci_dev *dev, int bar,
    unsigned long offset, unsigned long maxlen);
extern void __iomem *pci_iomap_wc(struct pci_dev *dev, int bar,
    unsigned long maxlen);
extern void pci_iounmap(struct pci_dev *dev, void __iomem *addr);
extern struct pci_dev *pci_get_domain_bus_and_slot(int domain,
    unsigned int bus, unsigned int devfn);
extern struct pci_dev *pci_get_device(unsigned int vendor,
    unsigned int device, struct pci_dev *from);
extern struct pci_dev *pci_get_class(unsigned int cls, struct pci_dev *from);
extern void lkpi_pci_dev_put(struct pci_dev *dev);
extern int pci_register_driver(struct pci_driver *drv);
extern void pci_unregister_driver(struct pci_driver *drv);
extern const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
    struct pci_dev *dev);
extern int pci_enable_msi(struct pci_dev *dev);
extern void pci_disable_msi(struct pci_dev *dev);
extern int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs,
    unsigned int max_vecs, unsigned int flags);
extern void pci_free_irq_vectors(struct pci_dev *dev);
extern int pci_irq_vector(struct pci_dev *dev, unsigned int nr);
extern int pci_msix_vec_count(struct pci_dev *dev);
extern int pci_msi_vec_count(struct pci_dev *dev);
extern struct resource *pci_find_resource(struct pci_dev *dev,
    struct resource *res);
extern int pci_bus_alloc_resource(struct pci_bus *bus, struct resource *res,
    resource_size_t size, resource_size_t align, resource_size_t min,
    unsigned long type_mask,
    resource_size_t (*alignf)(void *, const struct resource *,
    resource_size_t, resource_size_t), void *alignf_data);
extern void pci_assign_unassigned_bus_resources(struct pci_bus *bus);
extern resource_size_t pcibios_align_resource(void *data,
    const struct resource *res, resource_size_t size, resource_size_t align);
#define	PCIBIOS_MIN_MEM		0x10000000UL
#define	PCIBIOS_MIN_IO		0x1000UL
extern int pci_resize_resource(struct pci_dev *dev, int i, int size);
extern int pci_rebar_get_possible_sizes(struct pci_dev *pdev, int bar);

#define	pcim_enable_device(p)		pci_enable_device(p)
#define	pcim_iomap(p, bar, len)		pci_iomap(p, bar, len)
#define	pcim_iounmap(p, a)		pci_iounmap(p, a)
#define	pci_rebar_bytes_to_size(b)	0
#define	pci_rebar_size_to_bytes(s)	((u64)1 << ((s) + 20))
#define	pci_platform_rom(p, s)		NULL
#define	pci_map_rom(p, s)		((void __iomem *)NULL)
#define	pci_unmap_rom(p, r)		((void)(r))
#define	pci_upstream_bridge(p)		((struct pci_dev *)NULL)
#define	pcie_find_root_port(p)		((struct pci_dev *)NULL)
#define	pci_physfn(p)			(p)
#define	pci_num_vf(p)			0
#define	pci_sriov_get_totalvfs(p)	0
#define	pci_vfs_assigned(p)		0
#define	pcie_get_speed_cap(p)		0
#define	pcie_get_width_cap(p)		0

#define	module_pci_driver(drv) \
	int lkpi_module_init_pci(void) { return (pci_register_driver(&(drv))); } \
	void lkpi_module_exit_pci(void) { pci_unregister_driver(&(drv)); }

#endif
