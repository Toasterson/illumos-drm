/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's module macros (include/linux/module.h, GPL-2.0). An illumos
 * module has its own _init/_fini/_info (the driver's glue file, e.g.
 * drivers/i915/illumos/i915_illumos.c); module_init(fn) and
 * module_exit(fn) record the Linux entry points under fixed names
 * (lkpi_module_init, lkpi_module_exit) that the glue calls. The
 * MODULE_* information macros expand to nothing. THIS_MODULE is NULL:
 * module reference counting is illumos' (modctl), not per object.
 */
#ifndef _LINUX_MODULE_H
#define	_LINUX_MODULE_H

#include <linux/types.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/moduleparam.h>
#include <linux/kmod.h>
#include <linux/stringify.h>

struct lx_module;
#define	THIS_MODULE		((struct lx_module *)NULL)
#define	KBUILD_MODNAME_LKPI	KBUILD_MODNAME

#define	module_init(fn) \
	int lkpi_module_init(void); \
	int lkpi_module_init(void) { return (fn()); }
#define	module_exit(fn) \
	void lkpi_module_exit(void); \
	void lkpi_module_exit(void) { fn(); }
#define	MODULE_LICENSE(x)
#define	MODULE_AUTHOR(x)
#define	MODULE_DESCRIPTION(x)
#define	MODULE_VERSION(x)
#define	MODULE_ALIAS(x)
#define	MODULE_SOFTDEP(x)
#define	MODULE_FIRMWARE(x)
#define	MODULE_IMPORT_NS(x)
#define	MODULE_INFO(t, i)
#define	try_module_get(m)	true
#define	module_put(m)		((void)(m))
#define	__module_get(m)		((void)(m))
#define	module_name(m)		KBUILD_MODNAME
#define	request_module(...)	(-ENOENT)
#define	symbol_get(x)		((typeof (&x))NULL)
#define	symbol_put(x)		((void)0)

#endif
