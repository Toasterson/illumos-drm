/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The open file of a DRM device in the illumos-drm facade.
 *
 * Origin: struct drm_file follows Linux v7.2's include/drm/drm_file.h
 * (MIT): the public fields keep Linux's names and relative order; the
 * authentication, master, client-capability, handle, sync-object, event
 * and PRIME state stays in the core (behind __core). Linux's object_idr
 * and table_lock, which drivers use to walk a file's handles, are not
 * provided: drm_illumos_gem_for_each_handle() (drm_gem.h) walks them, and
 * drm_gem_object_lookup() finds one (with a reference; Linux's lockless
 * idr_find() under RCU has no counterpart).
 * This file is a reimplementation for illumos, MIT-licensed.
 */

#ifndef _DRM_FILE_H
#define	_DRM_FILE_H

#include <drm/drm_facade_types.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * One open file of a device: every open(2) of a card or render node is a
 * clone open with its own device number and its own drm_file. The core
 * allocates it before the driver's open callback and frees it after the
 * driver's postclose callback; the driver keeps its per-file state in
 * driver_priv.
 *
 * pid is the opening process's ID (illumos pid_t, not Linux's struct
 * pid), client_id a number unique among the files of the system since the
 * module loaded, lhead the link in drm_device.filelist, minor the node the
 * file was opened on. filp is NULL: an illumos open file has no struct
 * file the driver could use.
 */
struct drm_file {
	pid_t pid;
	uint64_t client_id;
	struct list_head lhead;
	struct drm_minor *minor;
	void *filp;
	void *driver_priv;
	/* private: the core's state */
	uint64_t __core[4];
};

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_FILE_H */
