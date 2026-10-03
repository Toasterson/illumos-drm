/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Stub of Linux's include/linux/spinlock.h for the carried helpers
 * (carried/). drm_mm.h includes it but takes no lock (its callers
 * serialise every call on one allocator), so only the facade's
 * spinlock_t is visible here; spin_lock() and friends are R5c's. Linux's
 * spinlock.h brings the general kernel helpers (kernel.h, bitops.h) in
 * through its own includes, and drm_mm.h depends on that, so this stub
 * includes them too.
 *
 * To be reconciled with R5c: R5c's spinlock.h replaces this stub.
 */

#ifndef _LINUXKPI_LINUX_SPINLOCK_H
#define	_LINUXKPI_LINUX_SPINLOCK_H

#include <linux/types.h>
#include <linux/kernel.h>

#endif	/* _LINUXKPI_LINUX_SPINLOCK_H */
