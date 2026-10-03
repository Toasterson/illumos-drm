/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's ISA Plug-and-Play bus. Not supported: no PnP devices are found.
 */
#ifndef _LINUX_PNP_H
#define	_LINUX_PNP_H
#include <linux/types.h>
struct pnp_dev;
#define	pnp_range_reserved(start, end)	false
#endif
