/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's DRM event functions (drm_event_reserve_init(),
 * drm_send_event(), ...; Linux has them in drm_file.c) for the carried KMS
 * code, reimplemented over the core's event queue (facade 2.0,
 * drm_lkms.h). The core owns each file's read queue and its 4096-byte
 * budget (Linux's event_list and event_space); this module keeps Linux's
 * pending events (struct drm_pending_event, drm_file.pending_event_list)
 * and their semantics:
 *
 * - Reserving charges the event's length to the file's budget
 *   (drm_illumos_event_reserve(); -ENOMEM when it is exhausted) and links
 *   the event into the file's pending list.
 * - Sending signals and releases the completion, signals the fence (with
 *   the given timestamp) and drops its reference, and, if the file is
 *   still open, copies the event into the file's read queue
 *   (drm_illumos_event_deliver(), whose space stays charged until
 *   read(2) consumed it). The pending event is freed either way: the core
 *   keeps its own copy, where Linux frees the event after read(2).
 * - Cancelling gives the budget back, unlinks and frees the event.
 * - A file that closes before its events are sent: drm_kms_events_release()
 *   (the file_close hook) unlinks them and clears their file pointer, so a
 *   later send only signals and frees them.
 *
 * Everything that touches a file's pending list runs under dev->event_lock
 * (the KMS code's spin lock, also taken in the vblank interrupt); the core's
 * event functions may be called with it held.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/dma-fence.h>
#include <linux/export.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_lkms.h>
#include <uapi/drm/drm.h>

#include "drm_kms_illumos.h"

int
drm_event_reserve_init_locked(struct drm_device *dev,
    struct drm_file *file_priv, struct drm_pending_event *p,
    struct drm_event *e)
{
	int ret;

	lockdep_assert_held(&dev->event_lock);
	if (file_priv == NULL || p == NULL || e == NULL)
		return (-EINVAL);
	ret = drm_illumos_event_reserve(file_priv, e->length);
	if (ret != 0)
		return (ret);
	p->event = e;
	list_add(&p->pending_link, &file_priv->pending_event_list);
	p->file_priv = file_priv;
	return (0);
}
EXPORT_SYMBOL(drm_event_reserve_init_locked);

int
drm_event_reserve_init(struct drm_device *dev, struct drm_file *file_priv,
    struct drm_pending_event *p, struct drm_event *e)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&dev->event_lock, flags);
	ret = drm_event_reserve_init_locked(dev, file_priv, p, e);
	spin_unlock_irqrestore(&dev->event_lock, flags);
	return (ret);
}
EXPORT_SYMBOL(drm_event_reserve_init);

void
drm_event_cancel_free(struct drm_device *dev, struct drm_pending_event *p)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	if (p->file_priv != NULL) {
		drm_illumos_event_unreserve(p->file_priv, p->event->length);
		list_del(&p->pending_link);
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);
	if (p->fence != NULL)
		dma_fence_put(p->fence);
	kfree(p);
}
EXPORT_SYMBOL(drm_event_cancel_free);

/* Sends e with dev->event_lock held; timestamp 0: signal at "now". */
static void
drm_kms_send_event(struct drm_device *dev, struct drm_pending_event *e,
    ktime_t timestamp)
{
	lockdep_assert_held(&dev->event_lock);

	if (e->completion != NULL) {
		complete_all(e->completion);
		if (e->completion_release != NULL)
			e->completion_release(e->completion);
		e->completion = NULL;
	}
	if (e->fence != NULL) {
		if (timestamp != 0)
			dma_fence_signal_timestamp(e->fence, timestamp);
		else
			dma_fence_signal(e->fence);
		dma_fence_put(e->fence);
		e->fence = NULL;
	}
	if (e->file_priv != NULL) {
		list_del(&e->pending_link);
		drm_illumos_event_deliver(e->file_priv, e->event);
	}
	kfree(e);
}

void
drm_send_event_timestamp_locked(struct drm_device *dev,
    struct drm_pending_event *e, ktime_t timestamp)
{
	drm_kms_send_event(dev, e, timestamp);
}
EXPORT_SYMBOL(drm_send_event_timestamp_locked);

void
drm_send_event_locked(struct drm_device *dev, struct drm_pending_event *e)
{
	drm_kms_send_event(dev, e, 0);
}
EXPORT_SYMBOL(drm_send_event_locked);

void
drm_send_event(struct drm_device *dev, struct drm_pending_event *e)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	drm_kms_send_event(dev, e, 0);
	spin_unlock_irqrestore(&dev->event_lock, flags);
}
EXPORT_SYMBOL(drm_send_event);

/*
 * The file is closing: its pending events stay with their senders (a
 * flip or vblank still completes and frees them) but are no longer
 * delivered. Their budget goes with the file.
 */
void
drm_kms_events_release(struct drm_device *dev, struct drm_file *file)
{
	struct drm_pending_event *e, *et;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	list_for_each_entry_safe(e, et, &file->pending_event_list,
	    pending_link) {
		list_del(&e->pending_link);
		e->file_priv = NULL;
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);
}
