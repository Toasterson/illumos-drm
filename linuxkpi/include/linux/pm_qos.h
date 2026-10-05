/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's power-management QoS requests. Not supported: illumos has no
 * CPU-latency QoS interface for drivers; requests are recorded and have
 * no effect.
 */
#ifndef _LINUX_PM_QOS_H
#define	_LINUX_PM_QOS_H
#include <linux/types.h>
#define	PM_QOS_DEFAULT_VALUE	(-1)
#define	PM_QOS_CPU_LATENCY_DEFAULT_VALUE	(2000 * USEC_PER_SEC)
struct pm_qos_request {
	s32 value;
	bool active;
};
#define	cpu_latency_qos_add_request(r, v)	((r)->value = (v), (r)->active = true)
#define	cpu_latency_qos_update_request(r, v)	((r)->value = (v))
#define	cpu_latency_qos_remove_request(r)	((r)->active = false)
#define	cpu_latency_qos_request_active(r)	((r)->active)
#endif
