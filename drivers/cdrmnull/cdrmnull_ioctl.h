/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The driver-private ioctls of cdrmnull, the C DRM null driver of
 * illumos-drm (drivers/cdrmnull/cdrmnull.c). They exist only for the
 * kernel tests (tools/test/drm_r1_test.c, drm_r2_test.c) and are no
 * interface for anything else. Numbers, structures and semantics are those
 * of the Rust test driver rdrmnull, so that one test serves both drivers;
 * CDRMNULL_IOCTL_PRIME_SELFTEST is new in cdrmnull.
 *
 * Userland includes this after libdrm's "drm.h" is reachable (-I with
 * libdrm's include/drm directory, as for the other drm uAPI headers); the
 * kernel build finds <drm/drm.h> in the system headers.
 * MIT-licensed, as the facade headers.
 */

#ifndef _CDRMNULL_IOCTL_H
#define	_CDRMNULL_IOCTL_H

#if defined(_KERNEL)
#include <drm/drm.h>
#else
#include "drm.h"
#endif

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Command numbers, from DRM_COMMAND_BASE (0x40). 0x03 is rdrmnull's
 * SCANOUT, which belongs to the virtual display that cdrmnull does not
 * have yet (milestone R5i); the number stays reserved and fails with
 * ENOTTY.
 */
#define	DRM_CDRMNULL_CREATE_MOVABLE	0x00
#define	DRM_CDRMNULL_MOVE		0x01
#define	DRM_CDRMNULL_PUT_LATER		0x02
#define	DRM_CDRMNULL_SCANOUT_RESERVED	0x03
#define	DRM_CDRMNULL_HOLD		0x04
#define	DRM_CDRMNULL_FENCE		0x05
#define	DRM_CDRMNULL_BO_WAIT		0x06
#define	DRM_CDRMNULL_PRIME_SELFTEST	0x07
#define	DRM_CDRMNULL_FENCE_SELFTEST	0x08

/*
 * CREATE_MOVABLE: a test object with two backings of the same size, the
 * stand-in for an object a GPU's memory manager moves between system
 * memory and VRAM. size in (bytes, rounded up to whole pages; 0 is
 * EINVAL, more than half the largest object ENOMEM), handle out; pad must
 * be 0.
 */
struct cdrmnull_create_movable {
	__u64 size;
	__u32 handle;
	__u32 pad;
};

/*
 * MOVE: invalidates every user mapping of a movable object with
 * devmap_unload(9F) and, unless CDRMNULL_MOVE_UNLOAD_ONLY, switches the
 * object to its other backing first (CDRMNULL_MOVE_FILL fills that
 * backing with the byte in bits 0 to 7 of flags before the switch).
 * handle and flags in; pieces (mapping pieces invalidated) and backing
 * (0 or 1, the one now in use) out. EINVAL for another kind of object.
 */
struct cdrmnull_move {
	__u32 handle;
	__u32 flags;
	__u32 pieces;
	__u32 backing;
};

#define	CDRMNULL_MOVE_FILL		(1u << 8)
#define	CDRMNULL_MOVE_UNLOAD_ONLY	(1u << 9)

/*
 * PUT_LATER: takes a reference to the object of handle and drops it from
 * a timeout(9F) callback after delay_ms (1 to 10000) milliseconds, where
 * the core parks a last reference and releases it from its task queue.
 * One at a time (EBUSY).
 */
struct cdrmnull_put_later {
	__u32 handle;
	__u32 delay_ms;
};

/*
 * HOLD: with hold 1 the driver keeps a reference to the object of handle
 * (EBUSY if it keeps one already) across the close of every file, which
 * makes detach(9E) refuse; hold 0 drops it (ENOENT if there is none,
 * handle ignored). Other values of hold are EINVAL.
 */
struct cdrmnull_hold {
	__u32 handle;
	__u32 hold;
};

/*
 * FENCE: a fence of the device's test timeline that signals delay_ms
 * (0 to 10000) milliseconds later from a soft interrupt, optionally with
 * the positive errno error, put into the sync object syncobj at point
 * (0 replaces its fence), into the reservation of the buffer bo as a
 * writer or a reader, and/or into a new close-on-exec sync file whose
 * descriptor is returned in fd. seqno out: the fence's number on the
 * timeline. Render nodes may call it.
 */
struct cdrmnull_fence {
	__u32 delay_ms;
	__u32 flags;
	__u32 syncobj;
	__u32 bo;
	__u64 point;
	__s32 fd;
	__s32 error;
	__u64 seqno;
};

#define	CDRMNULL_FENCE_SYNCOBJ		(1u << 0)
#define	CDRMNULL_FENCE_BO_WRITE		(1u << 1)
#define	CDRMNULL_FENCE_BO_READ		(1u << 2)
#define	CDRMNULL_FENCE_SYNC_FILE	(1u << 3)
#define	CDRMNULL_FENCE_FLAGS		(CDRMNULL_FENCE_SYNCOBJ | \
	CDRMNULL_FENCE_BO_WRITE | CDRMNULL_FENCE_BO_READ | \
	CDRMNULL_FENCE_SYNC_FILE)

/*
 * BO_WAIT: waits for the reservation of the buffer handle: with
 * CDRMNULL_BO_WAIT_WRITERS for its kernel and write fences (before
 * reading), otherwise for all but bookkeeping fences (before writing).
 * timeout_ns is relative; negative waits for ever, 0 polls. ETIME on
 * timeout, EINTR when a signal interrupts the wait. Render nodes may call
 * it.
 */
struct cdrmnull_bo_wait {
	__u32 handle;
	__u32 flags;
	__s64 timeout_ns;
};

#define	CDRMNULL_BO_WAIT_WRITERS	(1u << 0)

/*
 * PRIME_SELFTEST: drives the facade's dma-buf functions from a driver.
 * CDRMNULL_PRIME_IMPORT: fd (a dma-buf) in, handle out, through
 * dma_buf_get(), drm_gem_prime_import() and drm_gem_handle_create(); a
 * dma-buf of this device gives a new handle of the exporting object.
 * CDRMNULL_PRIME_EXPORT: handle in, fd out (close-on-exec, writable),
 * through drm_gem_prime_export() and dma_buf_fd(). Exactly one of the two
 * flags; pad must be 0. Render nodes may call it.
 */
struct cdrmnull_prime_selftest {
	__s32 fd;
	__u32 flags;
	__u32 handle;
	__u32 pad;
};

#define	CDRMNULL_PRIME_IMPORT		(1u << 0)
#define	CDRMNULL_PRIME_EXPORT		(1u << 1)

/*
 * FENCE_SELFTEST: drives the facade's fence callbacks from a driver.
 * syncobj in (a sync object of the caller's file, used to get a second
 * structure for one core fence), passed out (the CDRMNULL_FENCE_T_* bits
 * of the checks that passed); pad must be 0. Render nodes may call it.
 *
 * SELF_PUT	a callback drops the last reference to its own structure
 *		while the fence signals through another structure.
 * REMOVE	a callback removed before the fence signals never runs.
 * SIGNALED	adding a callback to a signalled fence fails with ENOENT.
 * RELEASED	a callback still added when its structure is released runs
 *		once, with -EDEADLK (Linux's rule; facade 1.0).
 * QUEUED	a callback queued behind one that drops the structure's last
 *		reference still runs (the release waits; facade 1.0).
 */
struct cdrmnull_fence_selftest {
	__u32 syncobj;
	__u32 passed;
	__u32 pad;
	__u32 pad2;
};

#define	CDRMNULL_FENCE_T_SELF_PUT	(1u << 0)
#define	CDRMNULL_FENCE_T_REMOVE		(1u << 1)
#define	CDRMNULL_FENCE_T_SIGNALED	(1u << 2)
#define	CDRMNULL_FENCE_T_RELEASED	(1u << 3)
#define	CDRMNULL_FENCE_T_QUEUED		(1u << 4)
#define	CDRMNULL_FENCE_T_ALL		0x1fu

#define	DRM_IOCTL_CDRMNULL_CREATE_MOVABLE \
	DRM_IOWR((DRM_COMMAND_BASE + DRM_CDRMNULL_CREATE_MOVABLE), \
	    struct cdrmnull_create_movable)
#define	DRM_IOCTL_CDRMNULL_MOVE \
	DRM_IOWR((DRM_COMMAND_BASE + DRM_CDRMNULL_MOVE), struct cdrmnull_move)
#define	DRM_IOCTL_CDRMNULL_PUT_LATER \
	DRM_IOW((DRM_COMMAND_BASE + DRM_CDRMNULL_PUT_LATER), \
	    struct cdrmnull_put_later)
#define	DRM_IOCTL_CDRMNULL_HOLD \
	DRM_IOW((DRM_COMMAND_BASE + DRM_CDRMNULL_HOLD), struct cdrmnull_hold)
#define	DRM_IOCTL_CDRMNULL_FENCE \
	DRM_IOWR((DRM_COMMAND_BASE + DRM_CDRMNULL_FENCE), struct cdrmnull_fence)
#define	DRM_IOCTL_CDRMNULL_BO_WAIT \
	DRM_IOW((DRM_COMMAND_BASE + DRM_CDRMNULL_BO_WAIT), \
	    struct cdrmnull_bo_wait)
#define	DRM_IOCTL_CDRMNULL_PRIME_SELFTEST \
	DRM_IOWR((DRM_COMMAND_BASE + DRM_CDRMNULL_PRIME_SELFTEST), \
	    struct cdrmnull_prime_selftest)
#define	DRM_IOCTL_CDRMNULL_FENCE_SELFTEST \
	DRM_IOWR((DRM_COMMAND_BASE + DRM_CDRMNULL_FENCE_SELFTEST), \
	    struct cdrmnull_fence_selftest)

/* The names the rdrmnull tests use, with this driver's prefix. */
#define	CDRMNULL_IOCTL_CREATE_MOVABLE	DRM_IOCTL_CDRMNULL_CREATE_MOVABLE
#define	CDRMNULL_IOCTL_MOVE		DRM_IOCTL_CDRMNULL_MOVE
#define	CDRMNULL_IOCTL_PUT_LATER	DRM_IOCTL_CDRMNULL_PUT_LATER
#define	CDRMNULL_IOCTL_HOLD		DRM_IOCTL_CDRMNULL_HOLD
#define	CDRMNULL_IOCTL_FENCE		DRM_IOCTL_CDRMNULL_FENCE
#define	CDRMNULL_IOCTL_BO_WAIT		DRM_IOCTL_CDRMNULL_BO_WAIT
#define	CDRMNULL_IOCTL_PRIME_SELFTEST	DRM_IOCTL_CDRMNULL_PRIME_SELFTEST
#define	CDRMNULL_IOCTL_FENCE_SELFTEST	DRM_IOCTL_CDRMNULL_FENCE_SELFTEST

#ifdef	__cplusplus
}
#endif

#endif	/* _CDRMNULL_IOCTL_H */
