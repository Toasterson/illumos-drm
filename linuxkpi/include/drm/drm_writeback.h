/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/drm_writeback.h> (GPL-2.0; independent declarations of the
 * structures and functions the carried KMS core refers to). illumos-drm
 * has no writeback connectors: drm_writeback.c is not carried, and
 * misc/drm_kms' stubs (drm_kms_stubs.c) refuse to create one
 * (drm_writeback_connector_init: -ENODEV). Without a writeback connector
 * the core never creates a writeback job, so the job functions are
 * reached only through a programming error: drm_writeback_set_fb fails
 * with -EINVAL, drm_writeback_prepare_job succeeds doing nothing,
 * drm_writeback_cleanup_job releases the job's framebuffer and fence
 * references and frees it, drm_writeback_get_out_fence returns NULL.
 */
#ifndef __DRM_WRITEBACK_H__
#define	__DRM_WRITEBACK_H__

#include <drm/drm_connector.h>
#include <drm/drm_encoder.h>
#include <linux/workqueue.h>

struct drm_writeback_connector {
	struct drm_connector base;
	struct drm_encoder encoder;
	struct drm_property_blob *pixel_formats_blob_ptr;
	spinlock_t job_lock;
	struct list_head job_queue;
	unsigned int fence_context;
	spinlock_t fence_lock;
	unsigned long fence_seqno;
	char timeline_name[32];
};

struct drm_writeback_job {
	struct drm_writeback_connector *connector;
	bool prepared;
	struct work_struct cleanup_work;
	struct list_head list_entry;
	struct drm_framebuffer *fb;
	struct dma_fence *out_fence;
	void *priv;
};

static inline struct drm_writeback_connector *
drm_connector_to_writeback(struct drm_connector *connector)
{
	return (container_of(connector, struct drm_writeback_connector, base));
}

int drm_writeback_connector_init(struct drm_device *dev,
    struct drm_writeback_connector *wb_connector,
    const struct drm_connector_funcs *con_funcs,
    const struct drm_encoder_helper_funcs *enc_helper_funcs,
    const u32 *formats, int n_formats, u32 possible_crtcs);
int drm_writeback_set_fb(struct drm_connector_state *conn_state,
    struct drm_framebuffer *fb);
int drm_writeback_prepare_job(struct drm_writeback_job *job);
void drm_writeback_queue_job(struct drm_writeback_connector *wb_connector,
    struct drm_connector_state *conn_state);
void drm_writeback_cleanup_job(struct drm_writeback_job *job);
void drm_writeback_signal_completion(
    struct drm_writeback_connector *wb_connector, int status);
struct dma_fence *drm_writeback_get_out_fence(
    struct drm_writeback_connector *wb_connector);

#endif
