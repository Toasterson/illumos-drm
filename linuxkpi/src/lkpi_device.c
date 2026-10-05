/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The layer's struct device (see <linux/device.h>): initialisation for a
 * dev_info_t, managed resources (devres) and the device printing helper,
 * plus the small device-related globals (iomem_resource, capable()).
 *
 * Managed resources are kept on a per-device list, newest first; each
 * entry is either an action (function and argument) or an allocation
 * whose memory follows the entry header. They are released, newest
 * first, by lkpi_devres_release_all(), which the PCI glue calls when the
 * driver's probe fails or the device detaches. Actions run without the
 * device's devres lock held, so they may add or remove other resources.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/device/devres.h>
#include <linux/ioport.h>
#include <linux/capability.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/limits.h>
#include <linux/bug.h>
#include "lkpi_internal.h"

/*
 * The root of the I/O memory resource tree. The layer keeps no tree (the
 * PCI nexus owns the address space); request_mem_region() returns this
 * resource so that drivers see success. It spans the whole address space.
 */
struct resource iomem_resource = {
	.name = "PCI mem",
	.start = 0,
	.end = ~(resource_size_t)0,
	.flags = IORESOURCE_MEM
};

/* --- device ------------------------------------------------------------ */

/*
 * The default DMA attributes of a layer device: any address (dma_set_mask()
 * lowers dma_attr_addr_hi), byte alignment (allocations request page
 * alignment themselves), no segment limits worth stating.
 */
static const ddi_dma_attr_t lkpi_default_dma_attr = {
	.dma_attr_version = DMA_ATTR_V0,
	.dma_attr_addr_lo = 0,
	.dma_attr_addr_hi = 0xffffffffffffffffULL,
	.dma_attr_count_max = 0xffffffffULL,
	.dma_attr_align = 1,
	.dma_attr_burstsizes = 1,
	.dma_attr_minxfer = 1,
	.dma_attr_maxxfer = 0xffffffffULL,
	.dma_attr_seg = 0xffffffffULL,
	.dma_attr_sgllen = INT_MAX,
	.dma_attr_granular = 1,
	.dma_attr_flags = 0
};

void
lkpi_device_init(struct device *dev, dev_info_t *dip, const char *name)
{
	dev->lkpi_dip = dip;
	(void) strlcpy(dev->lkpi_name, name != NULL ? name : "",
	    sizeof (dev->lkpi_name));
	dev->init_name = dev->lkpi_name;
	dev->numa_node = NUMA_NO_NODE;
	dev->lkpi_dma_attr = lkpi_default_dma_attr;
	(mutex_init)(&dev->lkpi_devres_lock, NULL, MUTEX_DRIVER, NULL);
	INIT_LIST_HEAD(&dev->lkpi_devres);
}

/* Releases what is left of the device's managed resources. */
void
lkpi_device_fini(struct device *dev)
{
	if (dev->lkpi_devres.next == NULL)
		return;
	lkpi_devres_release_all(dev);
	(mutex_destroy)(&dev->lkpi_devres_lock);
	dev->lkpi_devres.next = dev->lkpi_devres.prev = NULL;
}

dev_info_t *
lkpi_dev_dip(const struct device *dev)
{
	return (dev != NULL ? dev->lkpi_dip : NULL);
}

/* --- printing ---------------------------------------------------------- */

#define	LKPI_DEV_MSGLEN		512
#define	LKPI_DEV_MSGLEN_STACK	160

/*
 * The cmn_err(9F) level of a Linux level string (KERN_SOH and a digit), as
 * in printk: EMERG..WARNING are CE_WARN, NOTICE is CE_NOTE, INFO and DEBUG
 * are CE_NOTE to the system log only (*logonly), KERN_CONT is CE_CONT.
 * A missing level is Linux's default, WARNING.
 */
static int
lkpi_dev_level(const char *level, bool *logonly)
{
	*logonly = false;
	if (level == NULL || level[0] != KERN_SOH_ASCII)
		return (CE_WARN);
	switch (level[1]) {
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
		return (CE_WARN);
	case '5':
		return (CE_NOTE);
	case '6':
	case '7':
		*logonly = true;
		return (CE_NOTE);
	case 'c':
		return (CE_CONT);
	default:
		return (CE_WARN);
	}
}

/*
 * dev_printk(): formats with the layer's vsnprintf() (Linux %p extensions)
 * and prints through dev_err(9F) on the device's node, so that the message
 * carries the driver instance. The buffer comes from kmem without sleeping
 * (this may run in interrupt context), else from the stack (shorter).
 * cmn_err() ends lines itself: one trailing newline is dropped.
 */
void
lkpi_dev_printk(const char *level, const struct device *dev,
    const char *fmt, ...)
{
	char sbuf[LKPI_DEV_MSGLEN_STACK];
	char *buf;
	size_t len;
	va_list ap;
	bool logonly;
	int ce;
	dev_info_t *dip;

	ce = lkpi_dev_level(level, &logonly);
	buf = kmem_alloc(LKPI_DEV_MSGLEN, KM_NOSLEEP);
	len = (buf != NULL) ? LKPI_DEV_MSGLEN : sizeof (sbuf);
	if (buf == NULL)
		buf = sbuf;

	va_start(ap, fmt);
	(void) lkpi_vsnprintf(buf, len, fmt, ap);
	va_end(ap);
	buf[len - 1] = '\0';

	if (ce != CE_CONT) {
		size_t n = strlen(buf);

		if (n > 0 && buf[n - 1] == '\n')
			buf[n - 1] = '\0';
	}

	dip = lkpi_dev_dip(dev);
	if (dip != NULL) {
		(dev_err)(dip, ce, logonly ? "!%s" : "%s", buf);
	} else {
		cmn_err(ce, logonly ? "!%s: %s" : "%s: %s",
		    dev != NULL ? dev->lkpi_name : "(NULL device *)", buf);
	}

	if (buf != sbuf)
		kmem_free(buf, LKPI_DEV_MSGLEN);
}

/* --- capabilities ------------------------------------------------------ */

/*
 * Every Linux capability is the DDI's privilege test: drv_priv(9F) of the
 * calling thread's credential.
 */
bool
lkpi_capable(int cap)
{
	(void) cap;
	return (drv_priv(ddi_get_cred()) == 0);
}

/* --- managed resources ------------------------------------------------- */

/*
 * One managed resource. For an allocation, lr_action is NULL and the
 * memory (lr_size bytes) follows the header, which is padded to 16 bytes
 * so that the memory has kmalloc()'s alignment.
 */
struct lkpi_devres {
	struct list_head lr_link;
	void (*lr_action)(void *);
	void *lr_data;
	size_t lr_size;
} __aligned(16);

static bool
lkpi_devres_ready(const struct device *dev)
{
	if (dev == NULL || dev->lkpi_devres.next == NULL) {
		WARN(1, "devres on a device the layer did not initialise");
		return (false);
	}
	return (true);
}

static void
lkpi_devres_free(struct lkpi_devres *dr)
{
	kmem_free(dr, sizeof (*dr) + dr->lr_size);
}

static void
lkpi_devres_add(struct device *dev, struct lkpi_devres *dr)
{
	mutex_enter(&dev->lkpi_devres_lock);
	list_add(&dr->lr_link, &dev->lkpi_devres);
	mutex_exit(&dev->lkpi_devres_lock);
}

/* Unlinks the newest action entry (action, data); NULL if there is none. */
static struct lkpi_devres *
lkpi_devres_unlink_action(struct device *dev, void (*action)(void *),
    void *data)
{
	struct lkpi_devres *dr, *found = NULL;

	mutex_enter(&dev->lkpi_devres_lock);
	list_for_each_entry(dr, &dev->lkpi_devres, lr_link) {
		if (dr->lr_action == action && dr->lr_data == data) {
			list_del(&dr->lr_link);
			found = dr;
			break;
		}
	}
	mutex_exit(&dev->lkpi_devres_lock);
	return (found);
}

int
devm_add_action(struct device *dev, void (*action)(void *), void *data)
{
	struct lkpi_devres *dr;

	if (action == NULL || !lkpi_devres_ready(dev))
		return (-EINVAL);
	/* Linux allocates with GFP_KERNEL: the caller may sleep. */
	dr = kmem_zalloc(sizeof (*dr), KM_SLEEP);
	dr->lr_action = action;
	dr->lr_data = data;
	lkpi_devres_add(dev, dr);
	return (0);
}

int
devm_add_action_or_reset(struct device *dev, void (*action)(void *),
    void *data)
{
	int ret;

	ret = devm_add_action(dev, action, data);
	if (ret != 0 && action != NULL)
		action(data);
	return (ret);
}

void
devm_remove_action(struct device *dev, void (*action)(void *), void *data)
{
	struct lkpi_devres *dr;

	if (!lkpi_devres_ready(dev))
		return;
	dr = lkpi_devres_unlink_action(dev, action, data);
	if (WARN_ON(dr == NULL))
		return;
	lkpi_devres_free(dr);
}

void
devm_release_action(struct device *dev, void (*action)(void *), void *data)
{
	struct lkpi_devres *dr;

	if (!lkpi_devres_ready(dev))
		return;
	dr = lkpi_devres_unlink_action(dev, action, data);
	if (WARN_ON(dr == NULL))
		return;
	dr->lr_action(dr->lr_data);
	lkpi_devres_free(dr);
}

void *
devm_kmalloc(struct device *dev, size_t size, gfp_t gfp)
{
	struct lkpi_devres *dr;

	if (!lkpi_devres_ready(dev))
		return (NULL);
	if (size == 0)
		return (ZERO_SIZE_PTR);
	if (size > SIZE_MAX - sizeof (*dr))
		return (NULL);
	if ((gfp & __GFP_ZERO) != 0)
		dr = kmem_zalloc(sizeof (*dr) + size, lkpi_kmflag(gfp));
	else
		dr = kmem_alloc(sizeof (*dr) + size, lkpi_kmflag(gfp));
	if (dr == NULL)
		return (NULL);
	INIT_LIST_HEAD(&dr->lr_link);
	dr->lr_action = NULL;
	dr->lr_size = size;
	dr->lr_data = dr + 1;
	lkpi_devres_add(dev, dr);
	return (dr->lr_data);
}

void
devm_kfree(struct device *dev, const void *p)
{
	struct lkpi_devres *dr, *found = NULL;

	if (ZERO_OR_NULL_PTR(p) || !lkpi_devres_ready(dev))
		return;
	mutex_enter(&dev->lkpi_devres_lock);
	list_for_each_entry(dr, &dev->lkpi_devres, lr_link) {
		if (dr->lr_action == NULL && dr->lr_data == p) {
			list_del(&dr->lr_link);
			found = dr;
			break;
		}
	}
	mutex_exit(&dev->lkpi_devres_lock);
	/* Freeing memory the device does not manage would corrupt kmem. */
	if (WARN_ON(found == NULL))
		return;
	lkpi_devres_free(found);
}

char *
devm_kstrdup(struct device *dev, const char *s, gfp_t gfp)
{
	size_t n;
	char *p;

	if (s == NULL)
		return (NULL);
	n = strlen(s) + 1;
	p = devm_kmalloc(dev, n, gfp & ~__GFP_ZERO);
	if (p != NULL)
		bcopy(s, p, n);
	return (p);
}

/*
 * Releases every managed resource of the device, newest first. Each entry
 * is unlinked under the lock and released without it.
 */
void
lkpi_devres_release_all(struct device *dev)
{
	struct lkpi_devres *dr;

	if (dev == NULL || dev->lkpi_devres.next == NULL)
		return;
	for (;;) {
		mutex_enter(&dev->lkpi_devres_lock);
		if (list_empty(&dev->lkpi_devres)) {
			mutex_exit(&dev->lkpi_devres_lock);
			break;
		}
		dr = list_first_entry(&dev->lkpi_devres, struct lkpi_devres,
		    lr_link);
		list_del(&dr->lr_link);
		mutex_exit(&dev->lkpi_devres_lock);

		if (dr->lr_action != NULL)
			dr->lr_action(dr->lr_data);
		lkpi_devres_free(dr);
	}
}
