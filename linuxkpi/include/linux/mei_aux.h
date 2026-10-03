/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The Intel MEI auxiliary-device description (GSC on discrete parts). Not
 * supported: the GSC auxiliary device is not created.
 */
#ifndef _LINUX_MEI_AUX_H
#define	_LINUX_MEI_AUX_H
#include <linux/types.h>
#include <linux/ioport.h>
struct mei_aux_device {
	struct resource irq_res;
	struct resource bar;
	struct resource ext_op_mem;
	bool slow_firmware;
};
#endif
