/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's relay channels (high-speed log buffers to user space). Not
 * supported: relay_open() returns NULL, which i915's GuC log handles by
 * not creating the relay.
 */
#ifndef _LINUX_RELAY_H
#define	_LINUX_RELAY_H
#include <linux/types.h>
#include <linux/fs.h>
struct rchan;
struct rchan_buf {
	void *start;
	void *data;
	size_t offset;
	size_t subbufs_produced;
	struct rchan *chan;
};
struct rchan_callbacks {
	int (*subbuf_start)(struct rchan_buf *buf, void *subbuf, void *prev_subbuf, size_t prev_padding);
	struct dentry *(*create_buf_file)(const char *filename, struct dentry *parent, umode_t mode, struct rchan_buf *buf, int *is_global);
	int (*remove_buf_file)(struct dentry *dentry);
};
#define	relay_open(b, p, s, n, cb, d)	((struct rchan *)NULL)
#define	relay_close(c)			((void)(c))
#define	relay_flush(c)			((void)(c))
#define	relay_reserve(c, l)		((void *)NULL)
#define	relay_subbufs_consumed(c, cpu, n)	((void)(c))
#define	relay_late_setup_files(c, b, p)	(-ENODEV)
#define	relay_file_operations		(*(const struct file_operations *)NULL)
#endif
