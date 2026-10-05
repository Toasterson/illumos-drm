/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The DRM functions the carried KMS code and i915's display call whose
 * Linux files misc/drm_kms does not carry (design note
 * notes/i915n-kms-design.md, "Not carried"), written for illumos. Each
 * group says what it stands for and how it behaves; none of them is a
 * copy of Linux code.
 *
 * GEM helpers of drm_gem.c (the core owns GEM; the facade has no handle
 * counts) and drm_gem_atomic_helper.c's plane helper are reimplemented
 * over facade functions.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/string_choices.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_print.h>
#include <drm/drm_bridge.h>
#include <drm/drm_lease.h>
#include <drm/drm_panel.h>
#include <drm/drm_utils.h>
#include <drm/drm_sysfs.h>
#include <drm/drm_writeback.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_plane.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/display/drm_hdcp_helper.h>

#include "drm_internal.h"
#include "drm_crtc_internal.h"

/*
 * --- Bridges (drm_bridge.c) -----------------------------------------------
 * illumos-drm has no bridge drivers: no encoder ever has a bridge chain,
 * so the chain functions are only reached with a NULL bridge, where
 * Linux's do nothing (checks pass, every mode is valid).
 */

struct drm_bridge *
drm_bridge_get(struct drm_bridge *bridge)
{
	return (bridge);
}
EXPORT_SYMBOL(drm_bridge_get);

void
drm_bridge_put(struct drm_bridge *bridge)
{
}
EXPORT_SYMBOL(drm_bridge_put);

void
drm_bridge_detach(struct drm_bridge *bridge)
{
}

enum drm_mode_status
drm_bridge_chain_mode_valid(struct drm_bridge *bridge,
    const struct drm_display_info *info, const struct drm_display_mode *mode)
{
	return (MODE_OK);
}
EXPORT_SYMBOL(drm_bridge_chain_mode_valid);

void
drm_bridge_chain_mode_set(struct drm_bridge *bridge,
    const struct drm_display_mode *mode,
    const struct drm_display_mode *adjusted_mode)
{
}
EXPORT_SYMBOL(drm_bridge_chain_mode_set);

int
drm_atomic_bridge_chain_check(struct drm_bridge *bridge,
    struct drm_crtc_state *crtc_state, struct drm_connector_state *conn_state)
{
	return (0);
}
EXPORT_SYMBOL(drm_atomic_bridge_chain_check);

void
drm_atomic_bridge_chain_disable(struct drm_bridge *bridge,
    struct drm_atomic_commit *state)
{
}
EXPORT_SYMBOL(drm_atomic_bridge_chain_disable);

void
drm_atomic_bridge_chain_post_disable(struct drm_bridge *bridge,
    struct drm_atomic_commit *state)
{
}
EXPORT_SYMBOL(drm_atomic_bridge_chain_post_disable);

void
drm_atomic_bridge_chain_pre_enable(struct drm_bridge *bridge,
    struct drm_atomic_commit *state)
{
}
EXPORT_SYMBOL(drm_atomic_bridge_chain_pre_enable);

void
drm_atomic_bridge_chain_enable(struct drm_bridge *bridge,
    struct drm_atomic_commit *state)
{
}
EXPORT_SYMBOL(drm_atomic_bridge_chain_enable);

/*
 * --- Leases (drm_lease.c) ---------------------------------------------------
 * Leases are not offered (the lease ioctls fail with ENOTTY), so no file
 * is a lessee: every file holds every object, and the CRTC filter returns
 * its input, as Linux's functions do for a file without a lease.
 */

bool
_drm_lease_held(struct drm_file *file_priv, int id)
{
	return (true);
}
EXPORT_SYMBOL(_drm_lease_held);

bool
drm_lease_held(struct drm_file *file_priv, int id)
{
	return (true);
}
EXPORT_SYMBOL(drm_lease_held);

uint32_t
drm_lease_filter_crtcs(struct drm_file *file_priv, uint32_t crtcs_in)
{
	return (crtcs_in);
}
EXPORT_SYMBOL(drm_lease_filter_crtcs);

/*
 * --- sysfs (drm_sysfs.c) ----------------------------------------------------
 * illumos has no sysfs: connectors get no class device (connector->kdev
 * stays NULL) and hotplug and property events are not sent to user space
 * (Xorg on illumos has no udev and probes on its own).
 */

int
drm_sysfs_connector_add(struct drm_connector *connector)
{
	return (0);
}

int
drm_sysfs_connector_add_late(struct drm_connector *connector)
{
	return (0);
}

void
drm_sysfs_connector_remove_early(struct drm_connector *connector)
{
}

void
drm_sysfs_connector_remove(struct drm_connector *connector)
{
}

void
drm_sysfs_hotplug_event(struct drm_device *dev)
{
}
EXPORT_SYMBOL(drm_sysfs_hotplug_event);

void
drm_sysfs_connector_hotplug_event(struct drm_connector *connector)
{
}
EXPORT_SYMBOL(drm_sysfs_connector_hotplug_event);

void
drm_sysfs_connector_property_event(struct drm_connector *connector,
    struct drm_property *property)
{
}
EXPORT_SYMBOL(drm_sysfs_connector_property_event);

/*
 * --- Writeback connectors (drm_writeback.c) --------------------------------
 * None exist (drm_writeback_connector_init() is not provided), so the
 * atomic core never creates a writeback job; these are reached only
 * through a programming error and then fail safely.
 */

int
drm_writeback_set_fb(struct drm_connector_state *conn_state,
    struct drm_framebuffer *fb)
{
	return (-EINVAL);
}
EXPORT_SYMBOL(drm_writeback_set_fb);

int
drm_writeback_prepare_job(struct drm_writeback_job *job)
{
	return (0);
}
EXPORT_SYMBOL(drm_writeback_prepare_job);

void
drm_writeback_cleanup_job(struct drm_writeback_job *job)
{
	if (job == NULL)
		return;
	if (job->fb != NULL)
		drm_framebuffer_put(job->fb);
	if (job->out_fence != NULL)
		dma_fence_put(job->out_fence);
	kfree(job);
}
EXPORT_SYMBOL(drm_writeback_cleanup_job);

struct dma_fence *
drm_writeback_get_out_fence(struct drm_writeback_connector *wb_connector)
{
	return (NULL);
}
EXPORT_SYMBOL(drm_writeback_get_out_fence);

/*
 * --- Panels (drm_panel.c) ---------------------------------------------------
 * No panel drivers and no panel followers on illumos: allocating a panel
 * fails with -ENODEV (i915 then registers no drm_panel for its eDP/DSI
 * connector and drives the panel power itself, as before Linux's panel
 * followers existed); the other functions accept NULL and do nothing.
 */

void *
__devm_drm_panel_alloc(struct device *dev, size_t size, size_t offset,
    const struct drm_panel_funcs *funcs, int connector_type)
{
	return (ERR_PTR(-ENODEV));
}
EXPORT_SYMBOL(__devm_drm_panel_alloc);

void
drm_panel_add(struct drm_panel *panel)
{
}
EXPORT_SYMBOL(drm_panel_add);

void
drm_panel_remove(struct drm_panel *panel)
{
}
EXPORT_SYMBOL(drm_panel_remove);

void
drm_panel_prepare(struct drm_panel *panel)
{
}
EXPORT_SYMBOL(drm_panel_prepare);

void
drm_panel_unprepare(struct drm_panel *panel)
{
}
EXPORT_SYMBOL(drm_panel_unprepare);

/*
 * --- Panel orientation quirks (drm_panel_orientation_quirks.c) -------------
 * No quirk table: the orientation of a built-in panel is unknown unless
 * the driver reads it from the firmware (VBT).
 */

int
drm_get_panel_orientation_quirk(int width, int height)
{
	return (DRM_MODE_PANEL_ORIENTATION_UNKNOWN);
}
EXPORT_SYMBOL(drm_get_panel_orientation_quirk);

/*
 * --- HDCP helpers (display/drm_hdcp_helper.c, GPL-2.0-only) ----------------
 * HDCP is not supported on illumos (i915's HDCP code is stubbed; HDCP is
 * display version 9 and later): the content protection property cannot
 * be attached (-ENODEV), so it never changes, and no key selection vector
 * is ever found revoked.
 */

int
drm_hdcp_check_ksvs_revoked(struct drm_device *dev, u8 *ksvs, u32 ksv_count)
{
	return (0);
}
EXPORT_SYMBOL(drm_hdcp_check_ksvs_revoked);

int
drm_connector_attach_content_protection_property(
    struct drm_connector *connector, bool hdcp_content_type)
{
	return (-ENODEV);
}
EXPORT_SYMBOL(drm_connector_attach_content_protection_property);

void
drm_hdcp_update_content_protection(struct drm_connector *connector, u64 val)
{
}
EXPORT_SYMBOL(drm_hdcp_update_content_protection);

/*
 * --- GEM (drm_gem.c) ------------------------------------------------------
 * A framebuffer takes a "handle reference" on each of its objects in
 * Linux, which keeps the object's GEM handle count above zero while the
 * framebuffer exists. Handle counts are the core's (the facade does not
 * maintain drm_gem_object.handle_count and offers no handle reference),
 * so the reference taken here is an object reference: the object lives as
 * long as the framebuffer, but a handle the client closes is gone (GETFB
 * on such a framebuffer creates a new handle, as Linux does for an object
 * without handles).
 */

bool
drm_gem_object_handle_get_if_exists_unlocked(struct drm_gem_object *obj)
{
	drm_gem_object_get(obj);
	return (true);
}

void
drm_gem_object_handle_put_unlocked(struct drm_gem_object *obj)
{
	drm_gem_object_put(obj);
}

/* drm_gem_print_info(): the object's facts (no flink names on illumos). */
void
drm_gem_print_info(struct drm_printer *p, unsigned int indent,
    const struct drm_gem_object *obj)
{
	drm_printf_indent(p, indent, "refcount=%u\n",
	    kref_read(&obj->refcount));
	drm_printf_indent(p, indent, "start=%08lx\n",
	    (unsigned long)drm_vma_node_start(&obj->vma_node));
	drm_printf_indent(p, indent, "size=%zu\n", obj->size);
	drm_printf_indent(p, indent, "imported=%s\n",
	    str_yes_no(drm_gem_is_imported(obj)));
}

/*
 * --- drm_gem_plane_helper_prepare_fb() (drm_gem_atomic_helper.c) ----------
 * Implicit synchronisation of a plane update: without an explicit fence
 * (IN_FENCE_FD) the plane waits for the last writer of the framebuffer's
 * object, i.e. the reservation object's DMA_RESV_USAGE_WRITE fences
 * (dma_resv_get_singleton(): one fence, or an array of them). An explicit
 * fence is used alone. Deviations from Linux v7.2: only the first plane's
 * object is looked at (i915's framebuffers keep every plane in one
 * object), and with an explicit fence the reservation's kernel fences are
 * not chained behind it (fence chains are the core's).
 */
int
drm_gem_plane_helper_prepare_fb(struct drm_plane *plane,
    struct drm_plane_state *state)
{
	struct drm_gem_object *obj;
	struct dma_fence *fence;
	int ret;

	if (state->fb == NULL || state->fence != NULL)
		return (0);
	obj = state->fb->obj[0];
	if (obj == NULL)
		return (-EINVAL);
	ret = dma_resv_get_singleton(obj->resv, DMA_RESV_USAGE_WRITE, &fence);
	if (ret != 0)
		return (ret);
	state->fence = fence;
	return (0);
}
EXPORT_SYMBOL(drm_gem_plane_helper_prepare_fb);
