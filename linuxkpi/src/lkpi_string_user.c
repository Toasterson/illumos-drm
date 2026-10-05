/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The <linux/kstrtox.h> functions that parse a number from user memory,
 * over copy_from_user() (the DRM facade's drm_copy_from_user(),
 * ddi_copyin(9F) in the calling ioctl's data model). They may sleep.
 * Kept apart from src/lkpi_string.c so that the pure string code builds
 * in the host tests. memdup_user() and friends of <linux/string.h> are
 * in src/lkpi_uaccess.c.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/kstrtox.h>
#include <linux/uaccess.h>
#include <linux/errno.h>

/* At most 66 characters: a 64-bit number in base 2, a sign and "\n". */
#define	LKPI_KSTRTO_USER_MAX	67

static int
lkpi_copy_num(char *buf, const char __user *s, size_t count)
{
	if (count > LKPI_KSTRTO_USER_MAX - 1)
		count = LKPI_KSTRTO_USER_MAX - 1;
	if (copy_from_user(buf, s, count) != 0)
		return (-EFAULT);
	buf[count] = '\0';
	return (0);
}

int
kstrtouint_from_user(const char __user *s, size_t count, unsigned int base,
    unsigned int *res)
{
	char buf[LKPI_KSTRTO_USER_MAX];
	int err = lkpi_copy_num(buf, s, count);

	return (err != 0 ? err : kstrtouint(buf, base, res));
}

int
kstrtoint_from_user(const char __user *s, size_t count, unsigned int base,
    int *res)
{
	char buf[LKPI_KSTRTO_USER_MAX];
	int err = lkpi_copy_num(buf, s, count);

	return (err != 0 ? err : kstrtoint(buf, base, res));
}

int
kstrtou64_from_user(const char __user *s, size_t count, unsigned int base,
    u64 *res)
{
	char buf[LKPI_KSTRTO_USER_MAX];
	int err = lkpi_copy_num(buf, s, count);

	return (err != 0 ? err : kstrtou64(buf, base, res));
}

int
kstrtobool_from_user(const char __user *s, size_t count, bool *res)
{
	char buf[LKPI_KSTRTO_USER_MAX];
	int err = lkpi_copy_num(buf, s, count);

	return (err != 0 ? err : kstrtobool(buf, res));
}
