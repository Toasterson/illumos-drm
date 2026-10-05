/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's out-of-memory notifier. Not supported: illumos has no OOM
 * notifier chain; registration succeeds without effect (the driver's
 * shrinker is reached through the layer's kmem reclaim hook instead, see
 * <linux/shrinker.h>).
 */
#ifndef _LINUX_OOM_H
#define	_LINUX_OOM_H
#include <linux/notifier.h>
#define	register_oom_notifier(nb)	((void)(nb), 0)
#define	unregister_oom_notifier(nb)	((void)(nb), 0)
#endif
