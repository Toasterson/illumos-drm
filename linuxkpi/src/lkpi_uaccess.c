/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * User-memory helpers over the DRM core's ioctl copies (the interfaces of
 * Linux's <linux/uaccess.h>, the memdup_user() family of
 * <linux/string.h> and simple_read_from_buffer() of <linux/fs.h>;
 * independent implementation).
 *
 * All user access goes through drm_copy_from_user()/drm_copy_to_user()
 * (the facade), which follow the data model of the ioctl being served and
 * fail outside one. Those copies are all or nothing, so the helpers that
 * scan or report partial progress copy at most up to the next page
 * boundary at a time: a page is either accessible or not, so a string
 * that ends before an inaccessible page is still read completely.
 */

#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/err.h>
#include "lkpi_internal.h"

/* The bytes from uaddr to the end of its page, at most `left`. */
static size_t
lkpi_uchunk(uint64_t uaddr, size_t left, size_t max)
{
	size_t n = PAGE_SIZE - (size_t)(uaddr & (PAGE_SIZE - 1));

	return (MIN(MIN(n, left), max));
}

unsigned long
clear_user(void __user *to, unsigned long n)
{
	char zero[128];
	uint64_t ua = (uint64_t)(uintptr_t)to;
	unsigned long done = 0;
	size_t c;

	bzero(zero, sizeof (zero));
	while (done < n) {
		c = lkpi_uchunk(ua + done, n - done, sizeof (zero));
		if (drm_copy_to_user(ua + done, zero, c) != 0)
			return (n - done);
		done += c;
	}
	return (0);
}

/*
 * The string's length if it ends within count bytes (dst holds it and its
 * NUL), count if it does not (dst is not terminated), or -EFAULT.
 */
long
strncpy_from_user(char *dst, const char __user *src, long count)
{
	uint64_t ua = (uint64_t)(uintptr_t)src;
	size_t done = 0, c, i;

	if (count <= 0)
		return (0);
	while (done < (size_t)count) {
		c = lkpi_uchunk(ua + done, (size_t)count - done, SIZE_MAX);
		if (drm_copy_from_user(dst + done, ua + done, c) != 0)
			return (-EFAULT);
		for (i = done; i < done + c; i++) {
			if (dst[i] == '\0')
				return ((long)i);
		}
		done += c;
	}
	return (count);
}

/* 1 if the user range is all zero, 0 if not, -EFAULT. */
int
lkpi_check_zeroed_user(const void __user *from, size_t size)
{
	unsigned char buf[64];
	uint64_t ua = (uint64_t)(uintptr_t)from;
	size_t done = 0, c, i;

	while (done < size) {
		c = lkpi_uchunk(ua + done, size - done, sizeof (buf));
		if (drm_copy_from_user(buf, ua + done, c) != 0)
			return (-EFAULT);
		for (i = 0; i < c; i++) {
			if (buf[i] != 0)
				return (0);
		}
		done += c;
	}
	return (1);
}

void *
memdup_user(const void __user *src, size_t len)
{
	void *p;

	if ((p = kmalloc(len, GFP_KERNEL)) == NULL)
		return (ERR_PTR(-ENOMEM));
	if (copy_from_user(p, src, len) != 0) {
		kfree(p);
		return (ERR_PTR(-EFAULT));
	}
	return (p);
}

void *
vmemdup_user(const void __user *src, size_t len)
{
	return (memdup_user(src, len));
}

void *
memdup_user_nul(const void __user *src, size_t len)
{
	char *p;

	if (len == SIZE_MAX || (p = kmalloc(len + 1, GFP_KERNEL)) == NULL)
		return (ERR_PTR(-ENOMEM));
	if (copy_from_user(p, src, len) != 0) {
		kfree(p);
		return (ERR_PTR(-EFAULT));
	}
	p[len] = '\0';
	return (p);
}

/* A copy of the user string, which must end within n bytes (-EINVAL). */
char *
strndup_user(const char __user *s, long n)
{
	char *p;
	long len;

	if (n <= 0)
		return (ERR_PTR(-EINVAL));
	if ((p = kmalloc((size_t)n, GFP_KERNEL)) == NULL)
		return (ERR_PTR(-ENOMEM));
	len = strncpy_from_user(p, s, n);
	if (len < 0 || len >= n) {
		kfree(p);
		return (ERR_PTR(len < 0 ? -EFAULT : -EINVAL));
	}
	return (p);
}

ssize_t
simple_read_from_buffer(void __user *to, size_t count, loff_t *ppos,
    const void *from, size_t available)
{
	loff_t pos = *ppos;
	size_t n;

	if (pos < 0)
		return (-EINVAL);
	if ((uint64_t)pos >= available || count == 0)
		return (0);
	n = MIN(count, available - (size_t)pos);
	if (copy_to_user(to, (const char *)from + pos, n) != 0)
		return (-EFAULT);
	*ppos = pos + n;
	return ((ssize_t)n);
}

ssize_t
simple_write_to_buffer(void *to, size_t available, loff_t *ppos,
    const void __user *from, size_t count)
{
	loff_t pos = *ppos;
	size_t n;

	if (pos < 0)
		return (-EINVAL);
	if ((uint64_t)pos >= available || count == 0)
		return (0);
	n = MIN(count, available - (size_t)pos);
	if (copy_from_user((char *)to + pos, from, n) != 0)
		return (-EFAULT);
	*ppos = pos + n;
	return ((ssize_t)n);
}
