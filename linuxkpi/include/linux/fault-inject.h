/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's fault injection. Not supported: no fault is ever injected
 * (should_fail() is false).
 */
#ifndef _LINUX_FAULT_INJECT_H
#define	_LINUX_FAULT_INJECT_H
#include <linux/types.h>
struct fault_attr {
	unsigned long probability;
	atomic_t times;
};
#define	DECLARE_FAULT_ATTR(name)	struct fault_attr name = { 0 }
#define	should_fail(attr, size)		false
#define	fault_create_debugfs_attr(n, p, a)	((struct dentry *)ERR_PTR(-ENODEV))
#endif
