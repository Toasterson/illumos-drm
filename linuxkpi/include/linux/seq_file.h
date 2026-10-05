/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's seq_file, as a formatting target only (include/linux/
 * seq_file.h, GPL-2.0; reimplemented in src/lkpi_seq_file.c): there are
 * no debugfs or proc files on illumos, so nothing reads a seq_file through
 * a file; drivers' show functions can still print into one (the layer
 * uses it for drm_printer output to a buffer). DEFINE_SHOW_ATTRIBUTE
 * declares file_operations that are never opened.
 */
#ifndef _LINUX_SEQ_FILE_H
#define	_LINUX_SEQ_FILE_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/string.h>

struct seq_file {
	char *buf;
	size_t size;
	size_t count;
	loff_t index;
	void *private;
	const struct seq_operations *op;
	struct lx_file *file;
};

struct seq_operations {
	void *(*start)(struct seq_file *m, loff_t *pos);
	void (*stop)(struct seq_file *m, void *v);
	void *(*next)(struct seq_file *m, void *v, loff_t *pos);
	int (*show)(struct seq_file *m, void *v);
};

extern void seq_printf(struct seq_file *m, const char *fmt, ...)
    __printf(2, 3);
extern void seq_vprintf(struct seq_file *m, const char *fmt, va_list ap)
    __printf(2, 0);
extern void seq_puts(struct seq_file *m, const char *s);
extern void seq_putc(struct seq_file *m, char c);
extern void seq_write(struct seq_file *m, const void *data, size_t len);
extern int single_open(struct lx_file *file,
    int (*show)(struct seq_file *, void *), void *data);
extern int single_open_size(struct lx_file *file,
    int (*show)(struct seq_file *, void *), void *data, size_t size);
extern int single_release(struct inode *inode, struct lx_file *file);
extern ssize_t seq_read(struct lx_file *file, char __user *buf, size_t size,
    loff_t *ppos);
extern loff_t seq_lseek(struct lx_file *file, loff_t offset, int whence);
#define	seq_has_overflowed(m)	((m)->count >= (m)->size)
#define	seq_put_decimal_ull(m, d, n)	seq_printf(m, "%s%llu", d, (unsigned long long)(n))

#define	DEFINE_SHOW_ATTRIBUTE(__name) \
	static int __name##_open(struct inode *inode, struct lx_file *file) \
	{ return (single_open(file, __name##_show, inode->i_private)); } \
	static const struct file_operations __name##_fops = { \
		.open = __name##_open, .read = seq_read, \
		.llseek = seq_lseek, .release = single_release }
#define	DEFINE_SHOW_STORE_ATTRIBUTE(__name)	DEFINE_SHOW_ATTRIBUTE(__name)
#define	DEFINE_SEQ_ATTRIBUTE(__name) \
	static const struct file_operations __name##_fops = { .owner = NULL }

#endif
