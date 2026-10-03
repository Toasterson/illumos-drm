/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Xen guest detection: illumos is not a Xen PV kernel here; xen_domain()
 * and friends are false.
 */
#ifndef _XEN_XEN_H
#define	_XEN_XEN_H
#define	xen_domain()		0
#define	xen_pv_domain()		0
#define	xen_hvm_domain()	0
#define	xen_initial_domain()	0
#endif
