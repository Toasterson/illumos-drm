/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/drm_debugfs.h> without debugfs (CONFIG_DEBUG_FS is off on
 * illumos): the structures DRM code declares tables with, and no-op
 * registration functions. Independent of Linux's header (which pulls in
 * the GPU VA manager).
 */
#ifndef _DRM_DEBUGFS_H_
#define	_DRM_DEBUGFS_H_

#include <linux/types.h>
#include <linux/seq_file.h>

struct drm_device;
struct drm_file;
struct drm_minor;
struct dentry;

struct drm_info_list {
	const char *name;
	int (*show)(struct seq_file *m, void *arg);
	u32 driver_features;
	void *data;
};

struct drm_info_node {
	struct drm_minor *minor;
	const struct drm_info_list *info_ent;
	struct list_head list;
	struct dentry *dent;
};

struct drm_debugfs_info {
	const char *name;
	int (*show)(struct seq_file *m, void *data);
	u32 driver_features;
	void *data;
};

struct drm_debugfs_entry {
	struct drm_device *dev;
	struct drm_debugfs_info file;
	struct list_head list;
};

static inline void
drm_debugfs_create_files(const struct drm_info_list *files, int count,
    struct dentry *root, struct drm_minor *minor)
{
}

static inline int
drm_debugfs_remove_files(const struct drm_info_list *files, int count,
    struct dentry *root, struct drm_minor *minor)
{
	return (0);
}

static inline void
drm_debugfs_add_file(struct drm_device *dev, const char *name,
    int (*show)(struct seq_file *, void *), void *data)
{
}

static inline void
drm_debugfs_add_files(struct drm_device *dev,
    const struct drm_debugfs_info *files, int count)
{
}

static inline void
drm_debugfs_clients_add(struct drm_file *file)
{
}

static inline void
drm_debugfs_clients_remove(struct drm_file *file)
{
}

#endif
