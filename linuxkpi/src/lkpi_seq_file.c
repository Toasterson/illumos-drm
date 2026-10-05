/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's seq_file (<linux/seq_file.h>): formatting into a seq_file's
 * buffer, and single_open() files whose whole text is produced by one
 * show() call. illumos has no debugfs or procfs, so nothing opens such
 * a file through the VFS; the read path still behaves as Linux's for a
 * caller that does (seq_read() fills the text at position 0, doubling
 * the buffer until show() fits, and copies it out with copy_to_user()).
 *
 * Overflow is as on Linux: an append that does not fit (with room for a
 * NUL) sets count = size, which seq_has_overflowed() reports.
 */

#include <linux/kernel.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
#include <linux/err.h>

/* show() output larger than this fails with -ENOMEM. */
#define	LKPI_SEQ_MAX	(8UL * 1024 * 1024)

/* single_open()'s state: the seq_file, its operations and a read lock. */
struct lkpi_single {
	struct seq_file ls_m;
	struct seq_operations ls_ops;
	size_t ls_initial;
	kmutex_t ls_lock;
	bool ls_filled;
};

static inline void
seq_set_overflow(struct seq_file *m)
{
	m->count = m->size;
}

void
seq_vprintf(struct seq_file *m, const char *fmt, va_list ap)
{
	int n;

	if (m->buf != NULL && m->count < m->size) {
		n = lkpi_vsnprintf(m->buf + m->count, m->size - m->count, fmt,
		    ap);
		if (n >= 0 && (size_t)n < m->size - m->count) {
			m->count += (size_t)n;
			return;
		}
	}
	seq_set_overflow(m);
}

void
seq_printf(struct seq_file *m, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	seq_vprintf(m, fmt, ap);
	va_end(ap);
}

void
seq_write(struct seq_file *m, const void *data, size_t len)
{
	if (m->buf != NULL && m->count < m->size &&
	    len < m->size - m->count) {
		(void) memcpy(m->buf + m->count, data, len);
		m->count += len;
		return;
	}
	seq_set_overflow(m);
}

void
seq_puts(struct seq_file *m, const char *s)
{
	seq_write(m, s, strlen(s));
}

void
seq_putc(struct seq_file *m, char c)
{
	if (m->buf != NULL && m->count < m->size)
		m->buf[m->count++] = c;
}

static void *
single_start(struct seq_file *m, loff_t *pos)
{
	return (*pos == 0 ? (void *)1 : NULL);
}

static void *
single_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return (NULL);
}

static void
single_stop(struct seq_file *m, void *v)
{
}

int
single_open_size(struct lx_file *file,
    int (*show)(struct seq_file *, void *), void *data, size_t size)
{
	struct lkpi_single *s;

	if (file == NULL || show == NULL)
		return (-EINVAL);
	s = kzalloc(sizeof (*s), GFP_KERNEL);
	if (s == NULL)
		return (-ENOMEM);
	s->ls_ops.start = single_start;
	s->ls_ops.next = single_next;
	s->ls_ops.stop = single_stop;
	s->ls_ops.show = show;
	s->ls_initial = size < 64 ? 64 : size;
	(mutex_init)(&s->ls_lock, NULL, MUTEX_DRIVER, NULL);
	s->ls_m.op = &s->ls_ops;
	s->ls_m.private = data;
	s->ls_m.file = file;
	file->private_data = &s->ls_m;
	return (0);
}

int
single_open(struct lx_file *file, int (*show)(struct seq_file *, void *),
    void *data)
{
	return (single_open_size(file, show, data, PAGE_SIZE));
}

int
single_release(struct inode *inode, struct lx_file *file)
{
	struct seq_file *m = file->private_data;
	struct lkpi_single *s;

	if (m == NULL)
		return (0);
	s = container_of(m, struct lkpi_single, ls_m);
	file->private_data = NULL;
	kfree(m->buf);
	(mutex_destroy)(&s->ls_lock);
	kfree(s);
	return (0);
}

/* Runs the show() traversal into a buffer large enough for all of it. */
static int
seq_fill(struct lkpi_single *s)
{
	struct seq_file *m = &s->ls_m;
	size_t size = m->size != 0 ? m->size : s->ls_initial;

	for (;;) {
		loff_t pos = 0;
		void *v;
		int err = 0;

		if (m->buf == NULL || m->size != size) {
			kfree(m->buf);
			m->buf = kmalloc(size, GFP_KERNEL);
			m->size = m->buf != NULL ? size : 0;
			if (m->buf == NULL)
				return (-ENOMEM);
		}
		m->count = 0;
		m->index = 0;
		v = m->op->start(m, &pos);
		while (v != NULL && !IS_ERR(v)) {
			err = m->op->show(m, v);
			if (err < 0 || seq_has_overflowed(m))
				break;
			v = m->op->next(m, v, &pos);
		}
		m->op->stop(m, v);
		if (IS_ERR(v))
			return ((int)PTR_ERR(v));
		if (err < 0)
			return (err);
		if (!seq_has_overflowed(m))
			return (0);
		if (size >= LKPI_SEQ_MAX)
			return (-ENOMEM);
		size *= 2;
	}
}

ssize_t
seq_read(struct lx_file *file, char __user *buf, size_t size, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct lkpi_single *s;
	ssize_t ret;
	size_t n;
	int err;

	if (m == NULL || ppos == NULL || *ppos < 0)
		return (-EINVAL);
	s = container_of(m, struct lkpi_single, ls_m);
	mutex_enter(&s->ls_lock);
	if (*ppos == 0 || !s->ls_filled) {
		err = seq_fill(s);
		s->ls_filled = (err == 0);
		if (err != 0) {
			mutex_exit(&s->ls_lock);
			return (err);
		}
	}
	if ((size_t)*ppos >= m->count) {
		mutex_exit(&s->ls_lock);
		return (0);
	}
	n = m->count - (size_t)*ppos;
	if (n > size)
		n = size;
	if (copy_to_user(buf, m->buf + *ppos, n) != 0) {
		ret = -EFAULT;
	} else {
		*ppos += (loff_t)n;
		ret = (ssize_t)n;
	}
	mutex_exit(&s->ls_lock);
	return (ret);
}

loff_t
seq_lseek(struct lx_file *file, loff_t offset, int whence)
{
	loff_t pos;

	switch (whence) {
	case SEEK_SET:
		pos = offset;
		break;
	case SEEK_CUR:
		pos = file->f_pos + offset;
		break;
	default:
		return (-EINVAL);
	}
	if (pos < 0)
		return (-EINVAL);
	file->f_pos = pos;
	return (pos);
}
