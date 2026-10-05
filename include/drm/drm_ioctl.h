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
 * structure, at least the table entry's size: the core copies the
 * caller's structure in, zero-extends it to the table's size, calls the
 * handler, and copies it back out if the handler returned 0. It copies in
 * and out only in the directions both the caller's command and the
 * entry's `cmd` have, as Linux does: an _IOW call of an _IOWR entry is
 * copied in and not back out, and an _IOR call of an _IOWR entry is not
 * copied in, so the handler starts from zeros. Returns 0 or a negative
 * errno.
 */
typedef int drm_ioctl_t(struct drm_device *dev, void *data,
    struct drm_file *file_priv);

/*
 * One driver ioctl. `cmd` is the complete command value of the uAPI
 * header (DRM_IOWR(DRM_COMMAND_BASE + n, struct ...)), with a number from
 * DRM_COMMAND_BASE (0x40) to below 0xa0.
 *
 * Matching, in three stages:
 *  1. the entry whose `cmd` equals the caller's complete command value,
 *     so a table may list one number several times with different
 *     structure sizes (older layouts of one ioctl) or directions;
 *  2. otherwise the first entry with the same number and direction, of
 *     any size;
 *  3. otherwise, as Linux's drm_ioctl() (which indexes the table by
 *     number), the first entry with the same number in any direction:
 *     i915 lists DRM_IOCTL_I915_GEM_EXECBUFFER2_WR (_IOWR) only, and Mesa
 *     calls DRM_IOCTL_I915_GEM_EXECBUFFER2 (_IOW, the same number).
 * After stages 2 and 3 the argument follows Linux's size rule: a shorter
 * caller structure is zero-extended, a longer one keeps its extra bytes,
 * and the handler always sees at least the table's size; it is copied in
 * the directions both commands have (drm_ioctl_t). A number with no entry
 * fails with ENOTTY, and any driver command from a 32-bit process with
 * ENOTSUP.
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

/*
 * The command value of the table entry the calling thread's handler was
 * matched with, and the caller's own command value (which differs in the
 * size or direction bits when the entry was matched by number); 0 outside
 * a handler. An illumos addition: one handler can serve several entries.
 */
extern unsigned int drm_illumos_ioctl_cmd(void);
extern unsigned int drm_illumos_ioctl_user_cmd(void);

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_IOCTL_H */
