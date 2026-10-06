/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/drm_file.h>: the facade's struct drm_file (include_next)
 * and the Linux helpers on it. The handle table is the core's
 * (drm_illumos_gem_for_each_handle() replaces walks of object_idr).
 *
 * Facade 2.0: DRM_FACADE_FILE_KMS gives the file's KMS area Linux's
 * per-file mode-setting fields (fbs, fbs_lock, blobs, pending_event_list;
 * misc/drm_kms initialises them in its file_open hook), checked to fit
 * after the facade header. struct drm_pending_event is Linux's (from
 * Linux v7.2's include/drm/drm_file.h, MIT, comments shortened), and the
 * event functions are misc/drm_kms' reimplementations over the core's
 * event queue (drm_lkms.h):
 *
 * drm_event_reserve_init_locked(dev, file, p, e)
 *	charges e->length bytes to the file's event budget, links p into
 *	the file's pending_event_list and sets p->event = e, p->file_priv;
 *	-ENOMEM when the budget is exhausted. dev->event_lock held.
 * drm_event_reserve_init(dev, file, p, e)
 *	the same, taking dev->event_lock.
 * drm_event_cancel_free(dev, p)
 *	for an event that was reserved but will not be sent: gives the
 *	budget back, unlinks and frees it (and its fence reference).
 * drm_send_event_locked(dev, p), drm_send_event(dev, p),
 * drm_send_event_timestamp_locked(dev, p, timestamp)
 *	signals and releases p->completion, signals p->fence (with the
 *	timestamp for the last one) and, if the file is still open, queues
 *	p->event for read(2); then frees p. The event of a file that was
 *	closed before (p->file_priv NULL) is only freed.
 */
#ifndef _LKPI_DRM_FILE_H
#define	_LKPI_DRM_FILE_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/ktime.h>
#include <drm/drm_device.h>

#ifdef	DRM_FACADE_FILE_KMS
#error "DRM_FACADE_FILE_KMS is defined by <drm/drm_file.h> of the layer"
#endif
#define	DRM_FACADE_FILE_KMS \
	struct list_head fbs; \
	struct mutex fbs_lock; \
	struct list_head blobs; \
	struct list_head pending_event_list;

#include_next <drm/drm_file.h>

/* Linux's per-file mode-setting fields must fit the facade's KMS area. */
struct lkpi_drm_file_kms {
	DRM_FACADE_FILE_KMS
};
_Static_assert(sizeof (struct lkpi_drm_file_kms) <=
    sizeof (((struct drm_file *)0)->__kms),
    "Linux's KMS fields of struct drm_file do not fit the facade's KMS "
    "area (DRM_FACADE_KMS_FILE_WORDS)");
_Static_assert(_Alignof (struct lkpi_drm_file_kms) <= _Alignof (uint64_t),
    "Linux's KMS fields of struct drm_file need more alignment than the "
    "facade's KMS area has");

struct completion;
struct dma_fence;
struct drm_event;

/*
 * Linux's struct drm_pending_event (MIT, include/drm/drm_file.h of Linux
 * v7.2): an event the KMS code sends to a file, and the completion and
 * fence that go with it.
 */
struct drm_pending_event {
	/* signalled by drm_send_event(); optional */
	struct completion *completion;
	/* releases what completion is embedded in; optional */
	void (*completion_release)(struct completion *completion);
	/* the event for read(2); optional */
	struct drm_event *event;
	/* signalled by drm_send_event(); optional */
	struct dma_fence *fence;
	/* the file the event goes to; set only with event */
	struct drm_file *file_priv;
	/* the sender's list until drm_send_event() */
	struct list_head link;
	/* entry on drm_file.pending_event_list */
	struct list_head pending_link;
};

extern int drm_event_reserve_init_locked(struct drm_device *dev,
    struct drm_file *file_priv, struct drm_pending_event *p,
    struct drm_event *e);
extern int drm_event_reserve_init(struct drm_device *dev,
    struct drm_file *file_priv, struct drm_pending_event *p,
    struct drm_event *e);
extern void drm_event_cancel_free(struct drm_device *dev,
    struct drm_pending_event *p);
extern void drm_send_event_locked(struct drm_device *dev,
    struct drm_pending_event *e);
extern void drm_send_event(struct drm_device *dev,
    struct drm_pending_event *e);
extern void drm_send_event_timestamp_locked(struct drm_device *dev,
    struct drm_pending_event *e, ktime_t timestamp);

static inline bool
drm_is_render_client(const struct drm_file *file_priv)
{
	return (file_priv->minor != NULL &&
	    file_priv->minor->type == DRM_MINOR_RENDER);
}

static inline bool
drm_is_primary_client(const struct drm_file *file_priv)
{
	return (file_priv->minor != NULL &&
	    file_priv->minor->type == DRM_MINOR_PRIMARY);
}

#define	drm_file_err(f, fmt, ...)	DRM_ERROR(fmt, ##__VA_ARGS__)

#endif
