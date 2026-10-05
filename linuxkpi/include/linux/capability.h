/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's capability checks over illumos privileges: capable() is true
 * when the calling thread's credential is privileged (drv_priv(9F) on
 * ddi_get_cred(9F), the DDI's test for the all-privileges case used by
 * device drivers); every Linux capability maps to it.
 */
#ifndef _LINUX_CAPABILITY_H
#define	_LINUX_CAPABILITY_H
#include <linux/types.h>
#define	CAP_SYS_ADMIN		21
#define	CAP_SYS_NICE		23
#define	CAP_SYS_RAWIO		17
#define	CAP_PERFMON		38
extern bool lkpi_capable(int cap);
#define	capable(cap)		lkpi_capable(cap)
#define	perfmon_capable()	lkpi_capable(CAP_PERFMON)
#define	ns_capable(ns, cap)	lkpi_capable(cap)
#endif
