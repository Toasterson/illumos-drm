/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's anonymous inode files. Not supported (illumos has none; the DRM
 * core makes its own file descriptors): anon_inode_getfile() returns
 * ERR_PTR(-ENODEV).
 */
#ifndef _LINUX_ANON_INODES_H
#define	_LINUX_ANON_INODES_H
#include <linux/fs.h>
#define	anon_inode_getfile(name, fops, priv, flags) \
	((struct lx_file *)ERR_PTR(-ENODEV))
#define	anon_inode_getfd(name, fops, priv, flags)	(-ENODEV)
#endif
