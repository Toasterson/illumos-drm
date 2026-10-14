/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Notifications carry IDs, never connector pointers. The registry lock makes
 * lookup/queue atomic with unregister; unregister removes the entry, then
 * drains its worker before the device and its dip can disappear.
 */
#include <sys/nvpair.h>
#include <sys/sysevent.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include "drm_kms_illumos.h"

struct drm_hotplug {
	struct list_head link;
	struct drm_device *dev;
	struct delayed_work work;
	unsigned long next;
	uint64_t sequence;
	uint32_t connector, reasons;
	unsigned retries;
	bool dirty, stopped;
};
static LIST_HEAD(hotplug_devices);
static DEFINE_SPINLOCK(hotplug_lock);
/* mdb-visible counters; updated under hotplug_lock. */
uint64_t drm_hotplug_sent, drm_hotplug_failed, drm_hotplug_coalesced;

static void
hotplug_queue(struct drm_hotplug *hp)
{
	unsigned long now = jiffies;
	unsigned long delay = msecs_to_jiffies(250);

	if (time_before(now, hp->next))
		delay = max(delay, hp->next - now);
	/* Do not move an already scheduled deadline during a storm. */
	queue_delayed_work(system_wq, &hp->work, delay);
}

static int
hotplug_emit(struct drm_hotplug *hp, uint32_t connector, uint32_t reasons)
{
	nvlist_t *attrs;
	dev_info_t *dip = lkpi_dev_dip(hp->dev->dev);
	char path[MAXPATHLEN];
	int rc;

	if (nvlist_alloc(&attrs, NV_UNIQUE_NAME, KM_NOSLEEP) != 0)
		return (-1);
	(void) ddi_pathname(dip, path);
	rc = nvlist_add_uint32(attrs, "version", 1) |
	    nvlist_add_uint32(attrs, "driver-major", ddi_driver_major(dip)) |
	    nvlist_add_uint32(attrs, "primary-minor", hp->dev->primary->index) |
	    nvlist_add_string(attrs, "device-path", path) |
	    nvlist_add_uint32(attrs, "connector-id", connector) |
	    nvlist_add_uint32(attrs, "reasons", reasons) |
	    nvlist_add_uint64(attrs, "sequence", hp->sequence);
	if (rc == 0)
		rc = ddi_log_sysevent(dip, "illumos-drm", "EC_drm",
		    "ESC_drm_hotplug", attrs, NULL, DDI_NOSLEEP);
	nvlist_free(attrs);
	return (rc);
}

static void
hotplug_work(struct work_struct *work)
{
	struct drm_hotplug *hp = container_of(to_delayed_work(work),
	    struct drm_hotplug, work);
	unsigned long flags;
	uint32_t connector, reasons;
	int rc;

	spin_lock_irqsave(&hotplug_lock, flags);
	if (hp->stopped || !hp->dirty) {
		spin_unlock_irqrestore(&hotplug_lock, flags);
		return;
	}
	connector = hp->connector;
	reasons = hp->reasons;
	hp->dirty = false;
	hp->reasons = 0;
	hp->sequence++;
	hp->next = jiffies + msecs_to_jiffies(1000);
	spin_unlock_irqrestore(&hotplug_lock, flags);
	rc = hotplug_emit(hp, connector, reasons);
	spin_lock_irqsave(&hotplug_lock, flags);
	if (rc == 0) {
		drm_hotplug_sent++;
		hp->retries = 0;
	} else {
		drm_hotplug_failed++;
		/* Retry delivery at most three times without a new HPD. */
		if (++hp->retries <= 3 && !hp->stopped) {
			hp->dirty = true;
			hp->connector = 0;
			hp->reasons |= reasons;
		}
	}
	if (hp->dirty && !hp->stopped)
		hotplug_queue(hp);
	spin_unlock_irqrestore(&hotplug_lock, flags);
}

int
drm_kms_hotplug_register(struct drm_device *dev)
{
	struct drm_hotplug *hp;
	unsigned long flags;

	if (dev->primary == NULL || lkpi_dev_dip(dev->dev) == NULL)
		return (-ENODEV);
	hp = kzalloc(sizeof (*hp), GFP_KERNEL);
	if (hp == NULL)
		return (-ENOMEM);
	hp->dev = dev;
	hp->next = jiffies;
	INIT_DELAYED_WORK(&hp->work, hotplug_work);
	spin_lock_irqsave(&hotplug_lock, flags);
	list_add_tail(&hp->link, &hotplug_devices);
	spin_unlock_irqrestore(&hotplug_lock, flags);
	return (0);
}

void
drm_kms_hotplug_unregister(struct drm_device *dev)
{
	struct drm_hotplug *hp, *found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&hotplug_lock, flags);
	list_for_each_entry(hp, &hotplug_devices, link) {
		if (hp->dev == dev) {
			hp->stopped = true;
			list_del(&hp->link);
			found = hp;
			break;
		}
	}
	spin_unlock_irqrestore(&hotplug_lock, flags);
	if (found != NULL) {
		cancel_delayed_work_sync(&found->work);
		kfree(found);
	}
}

void
drm_kms_hotplug_notify(struct drm_device *dev, uint32_t id, uint32_t reasons)
{
	struct drm_hotplug *hp;
	unsigned long flags;

	spin_lock_irqsave(&hotplug_lock, flags);
	list_for_each_entry(hp, &hotplug_devices, link) {
		if (hp->dev != dev)
			continue;
		if (hp->dirty) {
			drm_hotplug_coalesced++;
			if (hp->connector != id)
				hp->connector = 0;
		} else {
			hp->connector = id;
		}
		hp->dirty = true;
		hp->reasons |= reasons;
		hp->retries = 0;
		hotplug_queue(hp);
		break;
	}
	spin_unlock_irqrestore(&hotplug_lock, flags);
}
