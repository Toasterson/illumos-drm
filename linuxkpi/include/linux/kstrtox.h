/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's kstrto*() (include/linux/kstrtox.h, GPL-2.0; reimplemented in
 * src/lkpi_string.c): 0 or -EINVAL/-ERANGE, a trailing newline allowed.
 */
#ifndef _LINUX_KSTRTOX_H
#define	_LINUX_KSTRTOX_H

#include <linux/types.h>

extern int kstrtoull(const char *s, unsigned int base, unsigned long long *res);
extern int kstrtoll(const char *s, unsigned int base, long long *res);
extern int kstrtoul(const char *s, unsigned int base, unsigned long *res);
extern int kstrtol(const char *s, unsigned int base, long *res);
extern int kstrtouint(const char *s, unsigned int base, unsigned int *res);
extern int kstrtoint(const char *s, unsigned int base, int *res);
extern int kstrtou16(const char *s, unsigned int base, u16 *res);
extern int kstrtou8(const char *s, unsigned int base, u8 *res);
extern int kstrtos8(const char *s, unsigned int base, s8 *res);
extern int kstrtobool(const char *s, bool *res);
extern int kstrtouint_from_user(const char __user *s, size_t count,
    unsigned int base, unsigned int *res);
extern int kstrtoint_from_user(const char __user *s, size_t count,
    unsigned int base, int *res);
extern int kstrtou64_from_user(const char __user *s, size_t count,
    unsigned int base, u64 *res);
extern int kstrtobool_from_user(const char __user *s, size_t count, bool *res);
extern unsigned long long simple_strtoull(const char *cp, char **endp,
    unsigned int base);
extern unsigned long simple_strtoul(const char *cp, char **endp,
    unsigned int base);
extern long simple_strtol(const char *cp, char **endp, unsigned int base);

static inline int
kstrtou64(const char *s, unsigned int base, u64 *res)
{
	return (kstrtoull(s, base, (unsigned long long *)res));
}

static inline int
kstrtos64(const char *s, unsigned int base, s64 *res)
{
	return (kstrtoll(s, base, (long long *)res));
}

static inline int
kstrtou32(const char *s, unsigned int base, u32 *res)
{
	return (kstrtouint(s, base, res));
}

static inline int
kstrtos32(const char *s, unsigned int base, s32 *res)
{
	return (kstrtoint(s, base, res));
}

#endif
