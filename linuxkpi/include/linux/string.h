/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's string functions (include/linux/string.h, GPL-2.0;
 * reimplemented). The C library subset comes from illumos' kernel
 * (<sys/systm.h>: strlen, strcmp, strcpy, strncpy, strlcpy, strchr,
 * memcpy, memset, ...); the Linux additions are in src/lkpi_string.c.
 */
#ifndef _LINUX_STRING_H
#define	_LINUX_STRING_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/overflow.h>

extern size_t lkpi_strnlen(const char *s, size_t max);
extern void *lkpi_memchr(const void *s, int c, size_t n);
extern void *memchr_inv(const void *s, int c, size_t n);
extern ssize_t sized_strscpy(char *dst, const char *src, size_t count);
extern ssize_t strscpy_pad(char *dst, const char *src, size_t count);
extern char *kstrdup(const char *s, gfp_t gfp) __malloc;
extern char *kstrndup(const char *s, size_t max, gfp_t gfp) __malloc;
extern const char *kstrdup_const(const char *s, gfp_t gfp);
extern void kfree_const(const void *x);
extern void *kmemdup(const void *src, size_t len, gfp_t gfp) __malloc;
extern void *kmemdup_array(const void *src, size_t count, size_t element_size,
    gfp_t gfp) __malloc;
extern char *kmemdup_nul(const char *s, size_t len, gfp_t gfp) __malloc;
extern char *strim(char *s);
extern char *skip_spaces(const char *s);
extern char *strreplace(char *s, char old, char new_c);
extern char *lkpi_strsep(char **s, const char *ct);
extern char *strnstr(const char *s1, const char *s2, size_t len);
extern char *strnchr(const char *s, size_t count, int c);
extern char *strchrnul(const char *s, int c);
extern bool sysfs_streq(const char *s1, const char *s2);
extern int match_string(const char * const *array, size_t n,
    const char *string);
static inline void
memset_p(void **p, void *v, size_t n)
{
	while (n-- > 0)
		*p++ = v;
}

extern void *memset32(u32 *s, u32 v, size_t count);
extern void *memset64(u64 *s, u64 v, size_t count);
extern void *memdup_user(const void __user *src, size_t len);
extern void *vmemdup_user(const void __user *src, size_t len);
extern void *memdup_user_nul(const void __user *src, size_t len);
extern char *strndup_user(const char __user *s, long n);

#define	strnlen(s, m)		lkpi_strnlen(s, m)
#define	memchr(s, c, n)		lkpi_memchr(s, c, n)
#define	strsep(s, ct)		lkpi_strsep(s, ct)
#define	strscpy(dst, src, ...) \
	__lkpi_strscpy_sel(_, ##__VA_ARGS__, 1, 2)(dst, src, ##__VA_ARGS__)
#define	__lkpi_strscpy_sel(a, b, n, ...)	__lkpi_strscpy##n
#define	__lkpi_strscpy1(dst, src, size)	sized_strscpy(dst, src, size)
#define	__lkpi_strscpy2(dst, src, ...)	sized_strscpy(dst, src, sizeof (dst))
#define	strtomem(dest, src)	strncpy((dest), (src), sizeof (dest))
#define	strtomem_pad(dest, src, pad) do { \
	size_t __dl = sizeof (dest); size_t __sl = strnlen((src), __dl); \
	memcpy((dest), (src), __sl); \
	if (__sl < __dl) memset((char *)(dest) + __sl, (pad), __dl - __sl); \
	} while (0)
#define	memcpy_and_pad(dest, dest_len, src, count, pad) do { \
	size_t __d = (dest_len), __c = (count); \
	if (__d > __c) { memcpy(dest, src, __c); \
		memset((char *)(dest) + __c, pad, __d - __c); } \
	else memcpy(dest, src, __d); } while (0)
#define	unsafe_memcpy(dst, src, bytes, justification)	memcpy(dst, src, bytes)
#define	memzero_explicit(s, n)	do { memset((s), 0, (n)); barrier(); } while (0)
#define	memset_after(obj, v, member) \
	memset((u8 *)(obj) + offsetofend(typeof (*(obj)), member), v, \
	    sizeof (*(obj)) - offsetofend(typeof (*(obj)), member))
#define	memset_startat(obj, v, member) \
	memset((u8 *)(obj) + offsetof(typeof (*(obj)), member), v, \
	    sizeof (*(obj)) - offsetof(typeof (*(obj)), member))

static inline bool
str_has_prefix_len(const char *str, const char *prefix, size_t *len)
{
	size_t n = strlen(prefix);

	*len = n;
	return (strncmp(str, prefix, n) == 0);
}

static inline size_t
str_has_prefix(const char *str, const char *prefix)
{
	size_t len = strlen(prefix);

	return (strncmp(str, prefix, len) == 0 ? len : 0);
}

static inline bool
strstarts(const char *str, const char *prefix)
{
	return (strncmp(str, prefix, strlen(prefix)) == 0);
}

static inline const char *
kbasename(const char *path)
{
	const char *tail = strrchr(path, '/');

	return (tail != NULL ? tail + 1 : path);
}

#endif
