/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's user-memory access (include/linux/uaccess.h, GPL-2.0;
 * reimplemented). Copies go through the DRM facade's drm_copy_from_user()
 * and drm_copy_to_user(), which use the calling ioctl's data model and
 * mode (ddi_copyin(9F)); copy_*_user() return the number of bytes not
 * copied (all of them on a fault), get_user()/put_user() 0 or -EFAULT.
 * access_ok() is true: the copy validates the address. The "unsafe" forms
 * are the checked ones.
 */
#ifndef _LINUX_UACCESS_H
#define	_LINUX_UACCESS_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/string.h>
#include <drm/drm_ioctl.h>

static inline unsigned long
copy_from_user(void *to, const void __user *from, unsigned long n)
{
	if (n == 0)
		return (0);
	return (drm_copy_from_user(to, (uint64_t)(uintptr_t)from, n) == 0 ?
	    0 : n);
}

static inline unsigned long
copy_to_user(void __user *to, const void *from, unsigned long n)
{
	if (n == 0)
		return (0);
	return (drm_copy_to_user((uint64_t)(uintptr_t)to, from, n) == 0 ?
	    0 : n);
}

extern unsigned long clear_user(void __user *to, unsigned long n);
extern long strncpy_from_user(char *dst, const char __user *src, long count);

#define	__copy_from_user(t, f, n)		copy_from_user(t, f, n)
#define	__copy_to_user(t, f, n)			copy_to_user(t, f, n)
#define	__copy_from_user_inatomic(t, f, n)	copy_from_user(t, f, n)
#define	__copy_to_user_inatomic(t, f, n)	copy_to_user(t, f, n)
#define	copy_from_user_inatomic_nontemporal(t, f, n)	copy_from_user(t, f, n)
#define	copy_from_user_inatomic_nocache(t, f, n)	copy_from_user(t, f, n)
#define	__copy_from_user_inatomic_nocache(t, f, n)	copy_from_user(t, f, n)
#define	access_ok(addr, size)			((void)(addr), (void)(size), true)
#define	get_user(x, ptr) ({ \
	typeof (*(ptr)) __gu_v; \
	int __gu_e = copy_from_user(&__gu_v, (ptr), sizeof (__gu_v)) ? -EFAULT : 0; \
	(x) = __gu_e == 0 ? __gu_v : (typeof (*(ptr)))0; \
	__gu_e; })
#define	put_user(x, ptr) ({ \
	typeof (*(ptr)) __pu_v = (x); \
	copy_to_user((ptr), &__pu_v, sizeof (__pu_v)) ? -EFAULT : 0; })
#define	__get_user(x, ptr)	get_user(x, ptr)
#define	__put_user(x, ptr)	put_user(x, ptr)
#define	user_access_begin(p, s)		((void)(p), (void)(s), true)
#define	user_access_end()		((void)0)
#define	user_read_access_begin(p, s)	user_access_begin(p, s)
#define	user_read_access_end()		((void)0)
#define	user_write_access_begin(p, s)	user_access_begin(p, s)
#define	user_write_access_end()		((void)0)
#define	unsafe_get_user(x, ptr, err) \
	do { if (get_user(x, ptr) != 0) goto err; } while (0)
#define	unsafe_put_user(x, ptr, err) \
	do { if (put_user(x, ptr) != 0) goto err; } while (0)
#define	unsafe_copy_to_user(d, s, l, err) \
	do { if (copy_to_user(d, s, l) != 0) goto err; } while (0)
#define	pagefault_disable()		((void)0)
#define	pagefault_enable()		((void)0)
#define	pagefault_disabled()		0
#define	faulthandler_disabled()		0
#define	check_zeroed_user(p, s)		lkpi_check_zeroed_user(p, s)
extern int lkpi_check_zeroed_user(const void __user *from, size_t size);

static inline int
copy_struct_from_user(void *dst, size_t ksize, const void __user *src,
    size_t usize)
{
	size_t size = ksize < usize ? ksize : usize;

	if (usize > ksize) {
		int r = lkpi_check_zeroed_user((const char __user *)src + ksize,
		    usize - ksize);
		if (r <= 0)
			return (r ? r : -E2BIG);
	}
	if (copy_from_user(dst, src, size))
		return (-EFAULT);
	if (ksize > usize)
		memset((char *)dst + size, 0, ksize - size);
	return (0);
}

#endif
