/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <linux/cec.h> (the CEC uAPI, GPL-2.0 WITH Linux-syscall-note;
 * independent declaration), reduced to the physical address constant the
 * EDID parser uses: no CEC support on illumos (CONFIG_CEC_CORE is off).
 */
#ifndef _LINUX_CEC_H
#define	_LINUX_CEC_H

#include <linux/types.h>

#define	CEC_PHYS_ADDR_INVALID	0xffff

#endif
