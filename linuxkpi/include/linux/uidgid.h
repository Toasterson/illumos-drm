/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's kuid_t/kgid_t (unused identities). */
#ifndef _LINUX_UIDGID_H
#define	_LINUX_UIDGID_H
typedef struct { uid_t val; } kuid_t;
typedef struct { gid_t val; } kgid_t;
#define	GLOBAL_ROOT_UID	((kuid_t){ 0 })
#define	GLOBAL_ROOT_GID	((kgid_t){ 0 })
#endif
