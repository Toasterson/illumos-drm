/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's iov_iter, reduced to a declaration (no driver path the layer
 * supports uses it); struct iovec is illumos' (<sys/uio.h>).
 */
#ifndef _LINUX_UIO_H
#define	_LINUX_UIO_H
#include <linux/types.h>
struct iov_iter;
#endif
