/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's CEC notifier (include/media/cec-notifier.h and the connector
 * information of include/media/cec.h, GPL-2.0; independent declarations),
 * as on Linux without CONFIG_CEC_NOTIFIER: no CEC adapters on illumos.
 * cec_notifier_conn_register() returns ERR_PTR(-ENODEV) (non-NULL, as
 * Linux's stub returns a non-NULL cookie, so drivers do not log a
 * failure), and the other functions accept it and do nothing.
 */
#ifndef _MEDIA_CEC_NOTIFIER_H
#define	_MEDIA_CEC_NOTIFIER_H

#include <linux/types.h>
#include <linux/err.h>
#include <linux/cec.h>

struct device;
struct edid;
struct cec_adapter;
struct cec_notifier;
struct drm_connector;

struct cec_drm_connector_info {
	__u32 card_no;
	__u32 connector_id;
};

struct cec_connector_info {
	__u32 type;
	union {
		struct cec_drm_connector_info drm;
		__u32 raw[16];
	};
};

static inline void
cec_fill_conn_info_from_drm(struct cec_connector_info *conn_info,
    const struct drm_connector *connector)
{
	conn_info->type = 0;
}

static inline struct cec_notifier *
cec_notifier_conn_register(struct device *hdmi_dev, const char *port_name,
    const struct cec_connector_info *conn_info)
{
	return (ERR_PTR(-ENODEV));
}

static inline void
cec_notifier_conn_unregister(struct cec_notifier *n)
{
}

static inline void
cec_notifier_set_phys_addr(struct cec_notifier *n, u16 pa)
{
}

static inline void
cec_notifier_set_phys_addr_from_edid(struct cec_notifier *n,
    const struct edid *edid)
{
}

static inline void
cec_notifier_phys_addr_invalidate(struct cec_notifier *n)
{
}

#endif
