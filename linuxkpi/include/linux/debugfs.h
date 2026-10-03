/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's debugfs. Not supported: illumos has no debugfs. The create
 * functions return ERR_PTR(-ENODEV), which callers treat as "debugfs not
 * available" (Linux returns that when debugfs is not configured); removal
 * does nothing.
 */
#ifndef _LINUX_DEBUGFS_H
#define	_LINUX_DEBUGFS_H
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/err.h>
struct dentry;
struct debugfs_regset32;
struct debugfs_blob_wrapper {
	void *data;
	unsigned long size;
};
#define	__lkpi_nodebugfs	((struct dentry *)ERR_PTR(-ENODEV))
#define	debugfs_create_dir(n, p)		__lkpi_nodebugfs
#define	debugfs_create_file(n, m, p, d, f)	__lkpi_nodebugfs
#define	debugfs_create_file_unsafe(n, m, p, d, f)	__lkpi_nodebugfs
#define	debugfs_create_file_size(n, m, p, d, f, s)	((void)0)
#define	debugfs_create_bool(n, m, p, v)		((void)0)
#define	debugfs_create_u32(n, m, p, v)		((void)0)
#define	debugfs_create_u64(n, m, p, v)		((void)0)
#define	debugfs_create_x32(n, m, p, v)		((void)0)
#define	debugfs_create_ulong(n, m, p, v)	((void)0)
#define	debugfs_create_atomic_t(n, m, p, v)	((void)0)
#define	debugfs_create_blob(n, m, p, b)		__lkpi_nodebugfs
#define	debugfs_create_symlink(n, p, t)		__lkpi_nodebugfs
#define	debugfs_lookup(n, p)			((struct dentry *)NULL)
#define	debugfs_remove(d)			((void)(d))
#define	debugfs_remove_recursive(d)		((void)(d))
#define	debugfs_lookup_and_remove(n, p)		((void)(n))
#define	debugfs_initialized()			false
#define	DEFINE_DEBUGFS_ATTRIBUTE(fops, get, set, fmt) \
	static const struct file_operations fops = { .owner = NULL }
#define	DEFINE_DEBUGFS_ATTRIBUTE_SIGNED(fops, get, set, fmt) \
	DEFINE_DEBUGFS_ATTRIBUTE(fops, get, set, fmt)
#define	DEFINE_SIMPLE_ATTRIBUTE(fops, get, set, fmt) \
	DEFINE_DEBUGFS_ATTRIBUTE(fops, get, set, fmt)
#endif
