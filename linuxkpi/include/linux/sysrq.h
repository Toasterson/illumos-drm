/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's magic SysRq keys. Not supported: no keys are registered.
 */
#ifndef _LINUX_SYSRQ_H
#define	_LINUX_SYSRQ_H
struct sysrq_key_op;
#define	register_sysrq_key(k, op)	0
#define	unregister_sysrq_key(k, op)	0
#endif
