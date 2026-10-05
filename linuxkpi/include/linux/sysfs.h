/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's sysfs. Not supported: illumos has no sysfs. Creating files and
 * groups succeeds without effect (returns 0) because no driver function
 * depends on a sysfs file existing; removing does nothing. Attributes
 * keep their layout so drivers' tables compile.
 */
#ifndef _LINUX_SYSFS_H
#define	_LINUX_SYSFS_H

#include <linux/types.h>
#include <linux/stat.h>
#include <linux/compiler.h>
#include <linux/sprintf.h>
#include <linux/stringify.h>

struct kobject;
struct lx_module;
struct bin_attribute;

struct attribute {
	const char *name;
	umode_t mode;
};

struct attribute_group {
	const char *name;
	umode_t (*is_visible)(struct kobject *, struct attribute *, int);
	umode_t (*is_bin_visible)(struct kobject *,
	    const struct bin_attribute *, int);
	struct attribute **attrs;
	const struct bin_attribute *const *bin_attrs;
};

struct bin_attribute {
	struct attribute attr;
	size_t size;
	void *private;
	ssize_t (*read)(struct lx_file *, struct kobject *,
	    const struct bin_attribute *, char *, loff_t, size_t);
	ssize_t (*write)(struct lx_file *, struct kobject *,
	    const struct bin_attribute *, char *, loff_t, size_t);
	int (*mmap)(struct lx_file *, struct kobject *,
	    const struct bin_attribute *, struct vm_area_struct *);
};

struct sysfs_ops {
	ssize_t (*show)(struct kobject *, struct attribute *, char *);
	ssize_t (*store)(struct kobject *, struct attribute *, const char *,
	    size_t);
};

#define	__ATTR(_name, _mode, _show, _store) { \
	.attr = { .name = __stringify(_name), .mode = (_mode) }, \
	.show = _show, .store = _store }
#define	__ATTR_RO(_name)	__ATTR(_name, 0444, _name##_show, NULL)
#define	__ATTR_WO(_name)	__ATTR(_name, 0200, NULL, _name##_store)
#define	__ATTR_RW(_name)	__ATTR(_name, 0644, _name##_show, _name##_store)
#define	__ATTR_RW_MODE(_name, _mode)	__ATTR(_name, _mode, _name##_show, _name##_store)
#define	__ATTR_NULL		{ .attr = { .name = NULL } }
#define	__BIN_ATTR(_name, _mode, _read, _write, _size) { \
	.attr = { .name = __stringify(_name), .mode = (_mode) }, \
	.read = _read, .write = _write, .size = _size }
#define	BIN_ATTR(_name, _mode, _read, _write, _size) \
	struct bin_attribute bin_attr_##_name = __BIN_ATTR(_name, _mode, _read, _write, _size)
#define	ATTRIBUTE_GROUPS(_name) \
	static const struct attribute_group _name##_group = { .attrs = _name##_attrs }; \
	static const struct attribute_group *_name##_groups[] = { &_name##_group, NULL }
#define	sysfs_attr_init(a)	((void)(a))
extern const char power_group_name[];
#define	sysfs_bin_attr_init(a)	((void)(a))

#define	sysfs_create_file(k, a)		((void)(k), (void)(a), 0)
#define	sysfs_create_files(k, a)	((void)(k), (void)(a), 0)
#define	sysfs_remove_file(k, a)		((void)(k), (void)(a))
#define	sysfs_remove_files(k, a)	((void)(k), (void)(a))
#define	sysfs_create_group(k, g)	((void)(k), (void)(g), 0)
#define	sysfs_create_groups(k, g)	((void)(k), (void)(g), 0)
#define	sysfs_remove_group(k, g)	((void)(k), (void)(g))
#define	sysfs_remove_groups(k, g)	((void)(k), (void)(g))
#define	sysfs_merge_group(k, g)		((void)(k), (void)(g), 0)
#define	sysfs_unmerge_group(k, g)	((void)(k), (void)(g))
#define	sysfs_create_bin_file(k, a)	((void)(k), (void)(a), 0)
#define	sysfs_remove_bin_file(k, a)	((void)(k), (void)(a))
#define	sysfs_create_link(k, t, n)	((void)(k), (void)(t), 0)
#define	sysfs_remove_link(k, n)		((void)(k))
#define	sysfs_notify(k, d, a)		((void)(k))
#define	sysfs_update_group(k, g)	((void)(k), (void)(g), 0)
extern int sysfs_emit(char *buf, const char *fmt, ...) __printf(2, 3);
extern int sysfs_emit_at(char *buf, int at, const char *fmt, ...)
    __printf(3, 4);

#endif
