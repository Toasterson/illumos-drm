/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's struct device, reduced (include/linux/device.h, GPL-2.0;
 * independent design, src/lkpi_device.c).
 *
 * A layer device wraps an illumos dev_info_t: the PCI glue makes one per
 * attached instance (embedded in struct pci_dev), and its address is what
 * a C driver stores in drm_device.dev (the facade's DRM_FACADE_DEVICE_T is
 * struct device under the layer). Driver data, DMA limits, managed
 * resources (devm_*: released newest first when the driver's probe fails
 * or the device detaches) and the printing helpers live here. dev_name()
 * is the PCI address ("0000:00:02.0") for PCI devices.
 * Messages go to dev_err(9F) on the device's node.
 * A device is reference counted: lkpi_device_init() sets the count to 1,
 * get_device()/put_device() change it, and the last put_device() calls
 * dev->release (else type->release), which frees the structure (the PCI
 * glue's release frees its struct pci_dev; src/lkpi_device.c).
 */
#ifndef _LINUX_DEVICE_H
#define	_LINUX_DEVICE_H

#include <linux/types.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/pm.h>
#include <linux/gfp.h>
#include <linux/printk.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include <linux/uuid.h>
#include <linux/device/bus.h>
#include <linux/device/driver.h>
#include <linux/device/devres.h>

struct device_dma_parameters {
	unsigned int max_segment_size;
	unsigned int min_align_mask;
	unsigned long segment_boundary_mask;
};

struct device_type {
	const char *name;
	const struct attribute_group **groups;
	void (*release)(struct device *dev);
};

struct class;
struct device_node;
struct fwnode_handle;

struct device {
	struct kobject kobj;
	struct device *parent;
	const char *init_name;
	const struct device_type *type;
	const struct bus_type *bus;
	struct device_driver *driver;
	void *platform_data;
	void *driver_data;
	struct dev_pm_info power;
	u64 *dma_mask;
	u64 coherent_dma_mask;
	struct device_dma_parameters *dma_parms;
	struct class *class;
	void (*release)(struct device *dev);
	int numa_node;
	struct device_node *of_node;	/* always NULL: no device tree */
	struct fwnode_handle *fwnode;	/* always NULL */
	/* illumos */
	dev_info_t *lkpi_dip;
	char lkpi_name[32];
	ddi_dma_attr_t lkpi_dma_attr;
	kmutex_t lkpi_devres_lock;
	struct list_head lkpi_devres;
	volatile uint32_t lkpi_refs;	/* get_device() references */
};

static inline const char *
dev_name(const struct device *dev)
{
	return (dev->lkpi_name);
}

static inline void *
dev_get_drvdata(const struct device *dev)
{
	return (dev->driver_data);
}

static inline void
dev_set_drvdata(struct device *dev, void *data)
{
	dev->driver_data = data;
}


#ifndef dev_to_node	/* <linux/gfp.h> defines it too */
#define	dev_to_node(dev)		(-1)
#endif
#define	set_dev_node(dev, n)		((void)(dev))
extern struct device *get_device(struct device *dev);
extern void put_device(struct device *dev);

/*
 * The layer's own life cycle of a struct device it did not get from the
 * PCI glue (src/lkpi_device.c), for illumos glue that makes devices of
 * its own (misc/drm_kms' connector class devices): lkpi_device_init()
 * sets one up on the node `dip` with the name (count 1, devres ready);
 * lkpi_device_fini(), called from its release, runs its managed
 * resources and tears it down. lkpi_dev_dip() (lkpi/prologue.h) is the
 * node of a device.
 */
extern void lkpi_device_init(struct device *dev, dev_info_t *dip,
    const char *name);
extern void lkpi_device_fini(struct device *dev);
#define	device_is_registered(dev)	true
#define	dev_driver_string(dev)		((dev)->driver != NULL ? (dev)->driver->name : "")
#define	dev_is_pci(dev)			((dev)->bus == &pci_bus_type)
#define	device_link_add(c, s, f)	((struct device_link *)NULL)
#define	device_link_del(l)		((void)(l))
#define	device_link_remove(c, s)	((void)(c), (void)(s))
#define	dev_fwnode(dev)			((dev)->fwnode)
#define	device_property_read_u32(dev, n, v)	(-EINVAL)
#define	device_property_present(dev, n)	false
#define	device_iommu_mapped(dev)	false
#define	device_set_pm_not_required(dev)	((void)(dev))

extern void lkpi_dev_printk(const char *level, const struct device *dev,
    const char *fmt, ...) __printf(3, 4);

#define	dev_printk(level, dev, fmt, ...) \
	lkpi_dev_printk(level, dev, fmt, ##__VA_ARGS__)
#define	dev_emerg(dev, fmt, ...)	lkpi_dev_printk(KERN_EMERG, dev, fmt, ##__VA_ARGS__)
#define	dev_alert(dev, fmt, ...)	lkpi_dev_printk(KERN_ALERT, dev, fmt, ##__VA_ARGS__)
#define	dev_crit(dev, fmt, ...)		lkpi_dev_printk(KERN_CRIT, dev, fmt, ##__VA_ARGS__)
#define	dev_err(dev, fmt, ...)		lkpi_dev_printk(KERN_ERR, dev, fmt, ##__VA_ARGS__)
#define	dev_warn(dev, fmt, ...)		lkpi_dev_printk(KERN_WARNING, dev, fmt, ##__VA_ARGS__)
#define	dev_notice(dev, fmt, ...)	lkpi_dev_printk(KERN_NOTICE, dev, fmt, ##__VA_ARGS__)
#define	dev_info(dev, fmt, ...)		lkpi_dev_printk(KERN_INFO, dev, fmt, ##__VA_ARGS__)
#define	dev_dbg(dev, fmt, ...) do { \
	if (lkpi_debug) \
		lkpi_dev_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__); \
	} while (0)
#define	dev_vdbg(dev, fmt, ...)		dev_dbg(dev, fmt, ##__VA_ARGS__)
#define	dev_err_once(dev, fmt, ...)	DO_ONCE_LITE(dev_err, dev, fmt, ##__VA_ARGS__)
#define	dev_warn_once(dev, fmt, ...)	DO_ONCE_LITE(dev_warn, dev, fmt, ##__VA_ARGS__)
#define	dev_info_once(dev, fmt, ...)	DO_ONCE_LITE(dev_info, dev, fmt, ##__VA_ARGS__)
#define	dev_notice_once(dev, fmt, ...)	DO_ONCE_LITE(dev_notice, dev, fmt, ##__VA_ARGS__)
#define	dev_dbg_once(dev, fmt, ...)	dev_dbg(dev, fmt, ##__VA_ARGS__)
#define	dev_err_ratelimited(dev, fmt, ...)	dev_err(dev, fmt, ##__VA_ARGS__)
#define	dev_warn_ratelimited(dev, fmt, ...)	dev_warn(dev, fmt, ##__VA_ARGS__)
#define	dev_info_ratelimited(dev, fmt, ...)	dev_info(dev, fmt, ##__VA_ARGS__)
#define	dev_notice_ratelimited(dev, fmt, ...)	dev_notice(dev, fmt, ##__VA_ARGS__)
#define	dev_dbg_ratelimited(dev, fmt, ...)	dev_dbg(dev, fmt, ##__VA_ARGS__)
#define	dev_WARN(dev, fmt, ...)		WARN(1, "%s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define	dev_WARN_ONCE(dev, cond, fmt, ...) \
	WARN_ONCE(cond, "%s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define	dev_err_probe(dev, err, fmt, ...) ({ \
	int __e = (err); \
	if (__e != -EPROBE_DEFER) \
		dev_err(dev, "error %d: " fmt, __e, ##__VA_ARGS__); \
	__e; })

struct device_attribute {
	struct attribute attr;
	ssize_t (*show)(struct device *dev, struct device_attribute *attr,
	    char *buf);
	ssize_t (*store)(struct device *dev, struct device_attribute *attr,
	    const char *buf, size_t count);
};

#define	DEVICE_ATTR(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)
#define	DEVICE_ATTR_RO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#define	DEVICE_ATTR_RW(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RW(_name)
#define	DEVICE_ATTR_WO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_WO(_name)
#define	DEVICE_ATTR_ADMIN_RW(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RW_MODE(_name, 0600)
#define	device_create_file(dev, attr)		((void)(dev), (void)(attr), 0)
#define	device_remove_file(dev, attr)		((void)(dev), (void)(attr))
#define	device_create_bin_file(dev, attr)	((void)(dev), (void)(attr), 0)
#define	device_remove_bin_file(dev, attr)	((void)(dev), (void)(attr))
#define	kobj_to_dev(k)				container_of((k), struct device, kobj)

extern const struct bus_type pci_bus_type;

#endif
