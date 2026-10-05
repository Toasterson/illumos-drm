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
 * Facade 2.0: the client capabilities keep Linux's names and order; the
 * core sets them when SET_CLIENT_CAP succeeds (stereo_allowed,
 * universal_planes, atomic, aspect_ratio_allowed, writeback_connectors;
 * plane_color_pipeline and supports_virtualized_cursor_plane stay false
 * because the core refuses those capabilities) and keeps was_master and
 * is_master current (is_master: the file holds its device's master; read
 * them as a snapshot, drm_is_current_master() is the authority). The KMS
 * area __kms (DRM_FACADE_KMS_FILE_WORDS words, zeroed at open, never read
 * by the core) holds a Linux KMS implementation's per-file fields
 * (fbs, fbs_lock, blobs, pending_event_list) under their Linux names when
 * it defines DRM_FACADE_FILE_KMS as their declarations (drm_lkms.h).
 *
 * pid is the opening process's ID (illumos pid_t, not Linux's struct
 * pid), client_id a number unique among the files of the system since the
 * module loaded, lhead the link in drm_device.filelist, minor the node the
 * file was opened on. filp is NULL: an illumos open file has no struct
 * file the driver could use.
 */
#define	DRM_FACADE_KMS_FILE_WORDS	32

struct drm_file {
	bool stereo_allowed;
	bool universal_planes;
	bool atomic;
	bool aspect_ratio_allowed;
	bool writeback_connectors;
	bool plane_color_pipeline;
	bool was_master;
	bool is_master;
	bool supports_virtualized_cursor_plane;
	pid_t pid;
	uint64_t client_id;
	struct list_head lhead;
	struct drm_minor *minor;
	DRM_FACADE_FILE_T *filp;
	void *driver_priv;
	/* facade 2.0: the mode-setting area */
	union {
#ifdef	DRM_FACADE_FILE_KMS
		struct {
			DRM_FACADE_FILE_KMS
		};
#endif
		uint64_t __kms[DRM_FACADE_KMS_FILE_WORDS];
	};
	/* private: the core's state */
	uint64_t __core[4];
};

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_FILE_H */
