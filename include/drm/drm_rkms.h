/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Mode setting through the core's own implementation (milestone R3), for
 * the Rust drivers of illumos-drm. The core keeps the mode objects,
 * properties, framebuffers, blobs, atomic state, vblank counters and
 * events; a driver describes its display (layout), reports what is
 * connected (probe), checks and commits the per-object state, and reports
 * vblanks. C drivers get Linux's mode-setting core instead (milestone
 * R5i); this interface is not Linux's. MIT-licensed.
 */

#ifndef _DRM_RKMS_H
#define	_DRM_RKMS_H

#include <drm/drm_facade_types.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* Linux's plane types and connector states (the uAPI's values). */
enum drm_rkms_plane_type {
	DRM_RKMS_PLANE_OVERLAY = 0,
	DRM_RKMS_PLANE_PRIMARY = 1,
	DRM_RKMS_PLANE_CURSOR = 2
};

enum drm_rkms_connector_status {
	DRM_RKMS_CONNECTED = 1,
	DRM_RKMS_DISCONNECTED = 2,
	DRM_RKMS_UNKNOWN = 3
};

/*
 * A display mode, with the layout of the uAPI's struct drm_mode_modeinfo
 * (the aspect ratio in flags).
 */
struct drm_rkms_mode {
	uint32_t clock;
	uint16_t hdisplay;
	uint16_t hsync_start;
	uint16_t hsync_end;
	uint16_t htotal;
	uint16_t hskew;
	uint16_t vdisplay;
	uint16_t vsync_start;
	uint16_t vsync_end;
	uint16_t vtotal;
	uint16_t vscan;
	uint32_t vrefresh;
	uint32_t flags;
	uint32_t type;
	char name[32];
};

struct drm_rkms_plane_desc {
	uint32_t type;			/* enum drm_rkms_plane_type */
	uint32_t possible_crtcs;	/* bit mask of CRTC indices */
	const uint32_t *formats;	/* DRM_FORMAT_* fourcc codes */
	uint32_t num_formats;
	const uint64_t *modifiers;
	uint32_t num_modifiers;
};

struct drm_rkms_crtc_desc {
	uint32_t primary;		/* plane index */
	int32_t cursor;			/* plane index, or -1 */
	uint32_t gamma_size;		/* 0 for no gamma table */
};

struct drm_rkms_encoder_desc {
	uint32_t type;			/* DRM_MODE_ENCODER_* */
	uint32_t possible_crtcs;
	uint32_t possible_clones;
};

struct drm_rkms_mode_limits {
	uint32_t max_width;
	uint32_t max_height;
	uint32_t max_clock;		/* kHz */
	bool interlace;
	bool dblscan;
};

struct drm_rkms_connector_desc {
	uint32_t type;			/* DRM_MODE_CONNECTOR_* */
	uint32_t encoders;		/* bit mask of encoder indices */
	struct drm_rkms_mode_limits limits;
};

/*
 * The display's objects, fixed for the device's life. The layout callback
 * hands it to drm_rkms_layout_set(), which copies it; the arrays need only
 * stay valid during that call.
 */
struct drm_rkms_layout {
	uint32_t min_width;
	uint32_t max_width;
	uint32_t min_height;
	uint32_t max_height;
	const struct drm_rkms_plane_desc *planes;
	uint32_t num_planes;
	const struct drm_rkms_crtc_desc *crtcs;
	uint32_t num_crtcs;
	const struct drm_rkms_encoder_desc *encoders;
	uint32_t num_encoders;
	const struct drm_rkms_connector_desc *connectors;
	uint32_t num_connectors;
};

/* A framebuffer: a counted reference to a core object. */
struct drm_rkms_fb;
/* Where the layout callback puts the layout. */
struct drm_rkms_layout_sink;
/* What a probe found, filled in by the driver. */
struct drm_rkms_probe;
/* The rest of an atomic update; not readable in facade version 0. */
struct drm_rkms_view;

/*
 * The state of a CRTC: `enable` (a mode is set), `active` (it scans out)
 * and the mode, valid when has_mode.
 */
struct drm_rkms_crtc_state {
	bool active;
	bool enable;
	bool has_mode;
	struct drm_rkms_mode mode;
};

/*
 * The state of a plane. crtc is -1 for none; fb is NULL for none and is
 * valid during the callback (take drm_rkms_fb_get() to keep it).
 * Source coordinates are 16.16 fixed point.
 */
struct drm_rkms_plane_state {
	int32_t crtc;
	struct drm_rkms_fb *fb;
	int32_t crtc_x;
	int32_t crtc_y;
	uint32_t crtc_w;
	uint32_t crtc_h;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_w;
	uint32_t src_h;
};

struct drm_rkms_connector_state {
	int32_t crtc;			/* -1 for none */
	int32_t encoder;		/* -1 for none */
	uint32_t link_status;
};

/* The fixed properties of a framebuffer. */
struct drm_rkms_fb_info {
	uint32_t id;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint64_t modifier;
	uint32_t flags;
	uint32_t pitches[4];
	uint32_t offsets[4];
	int32_t hot_x;
	int32_t hot_y;
	uint32_t cpp0;			/* bytes per pixel of plane 0, 0 if unknown */
	uint32_t num_objects;		/* planes with an object */
};

/* A damaged rectangle of DIRTYFB: x2 and y2 exclusive. */
struct drm_rkms_rect {
	uint32_t x1;
	uint32_t y1;
	uint32_t x2;
	uint32_t y2;
};

/*
 * The driver's mode-setting callbacks; the semantics are R3's KmsDriver
 * trait (README "Mode setting (R3)"). Indices are into the layout's
 * arrays. Check callbacks return 0 or a negative errno and must not
 * change anything; commit callbacks cannot fail. Every callback runs in
 * thread context. Required: layout (which calls drm_rkms_layout_set()),
 * probe.
 *
 * set_gamma	r, g and b have `size` entries (the CRTC's gamma_size).
 * dirty_fb	DIRTYFB: `planes` lists the planes showing fb, `clips` the
 *		damage (num_clips 0 means all of it). -ENOSYS if NULL.
 */
struct drm_rkms_funcs {
	int (*layout)(struct drm_device *dev,
	    struct drm_rkms_layout_sink *sink);
	void (*probe)(struct drm_device *dev, uint32_t connector,
	    struct drm_rkms_probe *out);
	bool (*format_mod_supported)(struct drm_device *dev, uint32_t plane,
	    uint32_t format, uint64_t modifier);
	int (*check_plane)(struct drm_device *dev, uint32_t plane,
	    const struct drm_rkms_plane_state *new_state,
	    const struct drm_rkms_crtc_state *crtc_state);
	int (*check_crtc)(struct drm_device *dev, uint32_t crtc,
	    const struct drm_rkms_crtc_state *new_state,
	    const struct drm_rkms_view *view);
	int (*check_connector)(struct drm_device *dev, uint32_t connector,
	    const struct drm_rkms_connector_state *new_state);
	void (*commit_crtc)(struct drm_device *dev, uint32_t crtc,
	    const struct drm_rkms_crtc_state *old_state,
	    const struct drm_rkms_crtc_state *new_state);
	void (*commit_plane)(struct drm_device *dev, uint32_t plane,
	    const struct drm_rkms_plane_state *old_state,
	    const struct drm_rkms_plane_state *new_state);
	void (*commit_connector)(struct drm_device *dev, uint32_t connector,
	    const struct drm_rkms_connector_state *old_state,
	    const struct drm_rkms_connector_state *new_state);
	void (*set_gamma)(struct drm_device *dev, uint32_t crtc,
	    const uint16_t *r, const uint16_t *g, const uint16_t *b,
	    uint32_t size);
	int (*dirty_fb)(struct drm_device *dev, struct drm_rkms_fb *fb,
	    const uint32_t *planes, uint32_t num_planes,
	    const struct drm_rkms_rect *clips, uint32_t num_clips);
};

/*
 * The layout (inside the layout callback): copies it. Returns 0, or
 * -EINVAL for a NULL layout or a second call.
 */
extern int drm_rkms_layout_set(struct drm_rkms_layout_sink *sink,
    const struct drm_rkms_layout *layout);

/* Probe results (inside the probe callback). */
extern void drm_rkms_probe_set_status(struct drm_rkms_probe *probe,
    uint32_t status);
extern int drm_rkms_probe_set_edid(struct drm_rkms_probe *probe,
    const void *edid, size_t len);
extern int drm_rkms_probe_add_mode(struct drm_rkms_probe *probe,
    const struct drm_rkms_mode *mode);
extern void drm_rkms_probe_set_size_mm(struct drm_rkms_probe *probe,
    uint32_t width_mm, uint32_t height_mm);

/*
 * Framebuffers. drm_rkms_fb_get() takes a reference that the driver drops
 * with drm_rkms_fb_put() (thread context). drm_rkms_fb_object() is plane
 * `i`'s GEM object, borrowed from the framebuffer, or NULL.
 */
extern struct drm_rkms_fb *drm_rkms_fb_get(struct drm_rkms_fb *fb);
extern void drm_rkms_fb_put(struct drm_rkms_fb *fb);
extern void drm_rkms_fb_info(const struct drm_rkms_fb *fb,
    struct drm_rkms_fb_info *info);
extern struct drm_gem_object *drm_rkms_fb_object(const struct drm_rkms_fb *fb,
    uint32_t i);

/*
 * R3's check_plane_helper: a visible plane on an enabled CRTC must cover
 * the whole mode unless can_position, and must not scale unless
 * can_scale. 0 or -EINVAL.
 */
extern int drm_rkms_check_plane_helper(
    const struct drm_rkms_plane_state *new_state,
    const struct drm_rkms_crtc_state *crtc_state, bool can_position,
    bool can_scale);

/*
 * Reports a vblank of `crtc` at `time_ns` (gethrtime(9F) time): counts it,
 * completes page flips and sends the events. Thread context; between
 * drm_dev_register() and the illumos_before_teardown callback.
 */
extern void drm_rkms_vblank(struct drm_device *dev, uint32_t crtc,
    int64_t time_ns);

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_RKMS_H */
