/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's poll table. Polling a DRM device is the DRM core's
 * (chpoll(9E)); driver poll functions are not called on illumos.
 */
#ifndef _LINUX_POLL_H
#define	_LINUX_POLL_H
#include <linux/fs.h>
#include <linux/wait.h>
typedef struct poll_table_struct {
	void *_qproc;
	__poll_t _key;
} poll_table;
#define	poll_wait(f, wq, p)	((void)(f), (void)(wq), (void)(p))
#define	EPOLLIN		0x00000001
#define	EPOLLPRI	0x00000002
#define	EPOLLOUT	0x00000004
#define	EPOLLERR	0x00000008
#define	EPOLLHUP	0x00000010
#define	EPOLLRDNORM	0x00000040
#define	EPOLLWRNORM	0x00000100
#endif
