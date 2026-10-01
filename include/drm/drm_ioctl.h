/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Driver-private ioctls in the illumos-drm facade.
 *
 * Origin: struct drm_ioctl_desc, enum drm_ioctl_flags and
 * DRM_IOCTL_DEF_DRV() follow Linux v7.2's include/drm/drm_ioctl.h (MIT),
 * with Linux's flag values. drm_copy_from_user() and drm_copy_to_user()
 * are illumos additions. This file is a reimplementation for illumos,
 * MIT-licensed.
 */

#ifndef _DRM_IOCTL_H
#define	_DRM_IOCTL_H

#include <drm/drm_facade_types.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* Who may call an ioctl; Linux's values. */
enum drm_ioctl_flags {
	DRM_AUTH = (1 << 0),
	DRM_MASTER = (1 << 1),
	DRM_ROOT_ONLY = (1 << 2),
	DRM_RENDER_ALLOW = (1 << 5)
};

/*
 * A driver ioctl handler. `data` is a kernel copy of the argument
 * structure: the core copies the caller's structure in (for _IOW and
 * _IOWR commands), zero-extends it to the table's size, calls the
 * handler, and copies it back out (for _IOR and _IOWR) if the handler
 * returned 0. Returns 0 or a negative errno.
 */
typedef int drm_ioctl_t(struct drm_device *dev, void *data,
    struct drm_file *file_priv);

/*
 * One driver ioctl. `cmd` is the complete command value of the uAPI
 * header (DRM_IOWR(DRM_COMMAND_BASE + n, struct ...)), with a number from
 * DRM_COMMAND_BASE (0x40) to below 0xa0.
 *
 * Matching: a command whose complete value equals an entry's `cmd` uses
 * that entry with the entry's size, so a table may list one number
 * several times with different structure sizes (older layouts of one
 * ioctl). Otherwise the first entry with the same number and direction is
 * used with Linux's size rule: a shorter caller structure is
 * zero-extended, a longer one keeps its extra bytes, and the handler
 * always sees at least the table's size. A command of no entry fails with
 * ENOTTY, and any driver command from a 32-bit process with ENOTSUP.
 */
struct drm_ioctl_desc {
	unsigned int cmd;
	enum drm_ioctl_flags flags;
	drm_ioctl_t *func;
	const char *name;
};

#define	DRM_IOCTL_DEF_DRV(ioctl, _func, _flags) \
	{ .cmd = DRM_IOCTL_##ioctl, .flags = (_flags), .func = (_func), \
	    .name = #ioctl }

/*
 * Copies between a user address named in an ioctl's argument structure
 * (u64_to_user_ptr() in Linux) and the kernel, with the data model and
 * the FKIOCTL flag of the ioctl the calling thread is serving. Valid only
 * inside a drm_ioctl_t handler. Return 0, or -EFAULT for a bad address or
 * when called outside a handler.
 */
extern int drm_copy_from_user(void *dst, uint64_t uaddr, size_t len);
extern int drm_copy_to_user(uint64_t uaddr, const void *src, size_t len);

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_IOCTL_H */
