/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's module parameters. Not supported as tunables yet: a parameter
 * keeps its compiled-in default (the variable's initialiser) and
 * module_param*() only declare that the variable is used. (Set them with
 * mdb -kw on the variable, or through driver.conf in the driver's glue.)
 */
#ifndef _LINUX_MODULEPARAM_H
#define	_LINUX_MODULEPARAM_H
#include <linux/types.h>
struct kernel_param;
struct kernel_param_ops {
	unsigned int flags;
	int (*set)(const char *val, const struct kernel_param *kp);
	int (*get)(char *buffer, const struct kernel_param *kp);
	void (*free)(void *arg);
};
struct kernel_param {
	const char *name;
	const struct kernel_param_ops *ops;
	void *arg;
};
#define	module_param(name, type, perm)
#define	module_param_named(name, value, type, perm)
#define	module_param_named_unsafe(name, value, type, perm)
#define	module_param_unsafe(name, type, perm)
#define	module_param_string(name, string, len, perm)
#define	module_param_array(name, type, nump, perm)
#define	module_param_array_named(name, array, type, nump, perm)
#define	module_param_cb(name, ops, arg, perm)
#define	module_param_call(name, set, get, arg, perm)
#define	MODULE_PARM_DESC(var, desc)
#define	param_check_int(name, p)
extern const struct kernel_param_ops param_ops_int;
extern const struct kernel_param_ops param_ops_uint;
extern const struct kernel_param_ops param_ops_bool;
extern const struct kernel_param_ops param_ops_charp;
extern int param_set_int(const char *val, const struct kernel_param *kp);
extern int param_get_int(char *buffer, const struct kernel_param *kp);
#endif
