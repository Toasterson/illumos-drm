/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's string additions (<linux/string.h>, <linux/hex.h>,
 * <linux/kstrtox.h>, <linux/string_helpers.h>) and sscanf(): an
 * independent implementation of their documented semantics. The C
 * library subset itself is illumos' (<sys/systm.h>). kstrto*_from_user()
 * are in src/lkpi_string_user.c, memdup_user() and friends in
 * src/lkpi_uaccess.c.
 *
 * kstrdup_const() and kvasprintf_const() always copy, so kfree_const()
 * always frees: it must only be given what those return (Linux also
 * accepts a pointer into .rodata, which the layer cannot recognise).
 */

#include <linux/kernel.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include <linux/string_helpers.h>
#include <linux/hex.h>
#include <linux/kstrtox.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/limits.h>

const char hex_asc[] = "0123456789abcdef";
const char hex_asc_upper[] = "0123456789ABCDEF";

/* --- string.h ------------------------------------------------------------ */

size_t
lkpi_strnlen(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n] != '\0')
		n++;
	return (n);
}

void *
lkpi_memchr(const void *s, int c, size_t n)
{
	const u8 *p = s;

	for (; n > 0; n--, p++) {
		if (*p == (u8)c)
			return ((void *)p);
	}
	return (NULL);
}

void *
memchr_inv(const void *s, int c, size_t n)
{
	const u8 *p = s;

	for (; n > 0; n--, p++) {
		if (*p != (u8)c)
			return ((void *)p);
	}
	return (NULL);
}

/* The length copied, or -E2BIG when src did not fit (dst is cut). */
ssize_t
sized_strscpy(char *dst, const char *src, size_t count)
{
	size_t i;

	if (count == 0 || count > INT_MAX)
		return (-E2BIG);
	for (i = 0; i < count; i++) {
		dst[i] = src[i];
		if (src[i] == '\0')
			return ((ssize_t)i);
	}
	dst[count - 1] = '\0';
	return (-E2BIG);
}

ssize_t
strscpy_pad(char *dst, const char *src, size_t count)
{
	ssize_t n = sized_strscpy(dst, src, count);

	if (n >= 0 && (size_t)n + 1 < count)
		(void) memset(dst + n + 1, 0, count - (size_t)n - 1);
	return (n);
}

char *
kstrdup(const char *s, gfp_t gfp)
{
	size_t len;
	char *p;

	if (s == NULL)
		return (NULL);
	len = strlen(s) + 1;
	p = kmalloc(len, gfp);
	if (p != NULL)
		(void) memcpy(p, s, len);
	return (p);
}

char *
kstrndup(const char *s, size_t max, gfp_t gfp)
{
	size_t len;
	char *p;

	if (s == NULL)
		return (NULL);
	len = lkpi_strnlen(s, max);
	p = kmalloc(len + 1, gfp);
	if (p != NULL) {
		(void) memcpy(p, s, len);
		p[len] = '\0';
	}
	return (p);
}

const char *
kstrdup_const(const char *s, gfp_t gfp)
{
	return (kstrdup(s, gfp));
}

void
kfree_const(const void *x)
{
	kfree(x);
}

void *
kmemdup(const void *src, size_t len, gfp_t gfp)
{
	void *p = kmalloc(len, gfp);

	if (p != NULL && len != 0)
		(void) memcpy(p, src, len);
	return (p);
}

void *
kmemdup_array(const void *src, size_t count, size_t element_size, gfp_t gfp)
{
	size_t len;

	if (check_mul_overflow(count, element_size, &len))
		return (NULL);
	return (kmemdup(src, len, gfp));
}

char *
kmemdup_nul(const char *s, size_t len, gfp_t gfp)
{
	char *p;

	if (s == NULL || len == SIZE_MAX)
		return (NULL);
	p = kmalloc(len + 1, gfp);
	if (p != NULL) {
		(void) memcpy(p, s, len);
		p[len] = '\0';
	}
	return (p);
}

char *
skip_spaces(const char *s)
{
	while (isspace(*s))
		s++;
	return ((char *)s);
}

/* Cuts trailing white space in place; returns s after leading space. */
char *
strim(char *s)
{
	size_t len = strlen(s);

	while (len > 0 && isspace(s[len - 1]))
		s[--len] = '\0';
	return (skip_spaces(s));
}

/* Replaces every old by new_c; returns s (Linux 6.9 and later). */
char *
strreplace(char *s, char old, char new_c)
{
	char *p;

	for (p = s; *p != '\0'; p++) {
		if (*p == old)
			*p = new_c;
	}
	return (s);
}

char *
lkpi_strsep(char **s, const char *ct)
{
	char *start = *s, *p;
	const char *d;

	if (start == NULL)
		return (NULL);
	for (p = start; *p != '\0'; p++) {
		for (d = ct; *d != '\0'; d++) {
			if (*p == *d) {
				*p = '\0';
				*s = p + 1;
				return (start);
			}
		}
	}
	*s = NULL;
	return (start);
}

char *
strnstr(const char *s1, const char *s2, size_t len)
{
	size_t l2 = strlen(s2), i;

	if (l2 == 0)
		return ((char *)s1);
	while (len >= l2 && *s1 != '\0') {
		for (i = 0; i < l2 && s1[i] == s2[i]; i++)
			;
		if (i == l2)
			return ((char *)s1);
		s1++;
		len--;
	}
	return (NULL);
}

char *
strnchr(const char *s, size_t count, int c)
{
	for (; count > 0 && *s != '\0'; count--, s++) {
		if (*s == (char)c)
			return ((char *)s);
	}
	return (NULL);
}

char *
strchrnul(const char *s, int c)
{
	while (*s != '\0' && *s != (char)c)
		s++;
	return ((char *)s);
}

/* Equal, ignoring one trailing newline on either side. */
bool
sysfs_streq(const char *s1, const char *s2)
{
	while (*s1 != '\0' && *s1 == *s2) {
		s1++;
		s2++;
	}
	if (*s1 == *s2)
		return (true);
	if (*s1 == '\0' && s2[0] == '\n' && s2[1] == '\0')
		return (true);
	if (*s2 == '\0' && s1[0] == '\n' && s1[1] == '\0')
		return (true);
	return (false);
}

/* The index of string in array (n entries, or up to a NULL), -EINVAL. */
int
match_string(const char * const *array, size_t n, const char *string)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (array[i] == NULL)
			break;
		if (strcmp(array[i], string) == 0)
			return ((int)i);
	}
	return (-EINVAL);
}

void *
memset32(u32 *s, u32 v, size_t count)
{
	u32 *p = s;

	while (count-- > 0)
		*p++ = v;
	return (s);
}

void *
memset64(u64 *s, u64 v, size_t count)
{
	u64 *p = s;

	while (count-- > 0)
		*p++ = v;
	return (s);
}

/* --- hex.h --------------------------------------------------------------- */

int
hex_to_bin(unsigned char ch)
{
	if (ch >= '0' && ch <= '9')
		return (ch - '0');
	if (ch >= 'a' && ch <= 'f')
		return (ch - 'a' + 10);
	if (ch >= 'A' && ch <= 'F')
		return (ch - 'A' + 10);
	return (-1);
}

int
hex2bin(u8 *dst, const char *src, size_t count)
{
	while (count-- > 0) {
		int hi = hex_to_bin((unsigned char)*src++);
		int lo;

		if (hi < 0)
			return (-EINVAL);
		lo = hex_to_bin((unsigned char)*src++);
		if (lo < 0)
			return (-EINVAL);
		*dst++ = (u8)((hi << 4) | lo);
	}
	return (0);
}

char *
bin2hex(char *dst, const void *src, size_t count)
{
	const u8 *p = src;

	while (count-- > 0)
		dst = hex_byte_pack(dst, *p++);
	return (dst);
}

/* --- kstrtox.h ----------------------------------------------------------- */

/*
 * Linux's radix rules: base 0 means 16 for "0x" followed by a hex digit,
 * 8 for a leading '0' and 10 otherwise; base 16 skips a "0x" prefix.
 */
static const char *
lkpi_fixup_radix(const char *s, unsigned int *base)
{
	if (*base == 0) {
		if (s[0] == '0') {
			if (_tolower(s[1]) == 'x' && isxdigit(s[2]))
				*base = 16;
			else
				*base = 8;
		} else {
			*base = 10;
		}
	}
	if (*base == 16 && s[0] == '0' && _tolower(s[1]) == 'x')
		s += 2;
	return (s);
}

/*
 * Parses digits of `base`; returns the number of characters used and sets
 * *overflow if the value does not fit in 64 bits.
 */
static size_t
lkpi_parse_digits(const char *s, unsigned int base, unsigned long long *res,
    bool *overflow)
{
	unsigned long long v = 0;
	size_t n = 0;

	*overflow = false;
	for (;; n++) {
		int d = hex_to_bin((unsigned char)s[n]);

		if (d < 0 || (unsigned int)d >= base)
			break;
		if (v > (ULLONG_MAX - (unsigned int)d) / base)
			*overflow = true;
		v = v * base + (unsigned int)d;
	}
	*res = v;
	return (n);
}

int
kstrtoull(const char *s, unsigned int base, unsigned long long *res)
{
	unsigned long long v;
	bool ovf;
	size_t n;

	if (s == NULL || base == 1 || base > 16)
		return (-EINVAL);
	if (s[0] == '+')
		s++;
	s = lkpi_fixup_radix(s, &base);
	n = lkpi_parse_digits(s, base, &v, &ovf);
	if (n == 0)
		return (-EINVAL);
	if (ovf)
		return (-ERANGE);
	s += n;
	if (*s == '\n')
		s++;
	if (*s != '\0')
		return (-EINVAL);
	*res = v;
	return (0);
}

int
kstrtoll(const char *s, unsigned int base, long long *res)
{
	unsigned long long v;
	int err;

	if (s == NULL)
		return (-EINVAL);
	if (s[0] == '-') {
		err = kstrtoull(s + 1, base, &v);
		if (err != 0)
			return (err);
		if (v > (unsigned long long)LLONG_MAX + 1)
			return (-ERANGE);
		*res = (long long)(0ULL - v);
		return (0);
	}
	err = kstrtoull(s, base, &v);
	if (err != 0)
		return (err);
	if (v > (unsigned long long)LLONG_MAX)
		return (-ERANGE);
	*res = (long long)v;
	return (0);
}

int
kstrtoul(const char *s, unsigned int base, unsigned long *res)
{
	unsigned long long v;
	int err = kstrtoull(s, base, &v);

	if (err == 0)
		*res = (unsigned long)v;
	return (err);
}

int
kstrtol(const char *s, unsigned int base, long *res)
{
	long long v;
	int err = kstrtoll(s, base, &v);

	if (err == 0)
		*res = (long)v;
	return (err);
}

int
kstrtouint(const char *s, unsigned int base, unsigned int *res)
{
	unsigned long long v;
	int err = kstrtoull(s, base, &v);

	if (err != 0)
		return (err);
	if (v > UINT_MAX)
		return (-ERANGE);
	*res = (unsigned int)v;
	return (0);
}

int
kstrtoint(const char *s, unsigned int base, int *res)
{
	long long v;
	int err = kstrtoll(s, base, &v);

	if (err != 0)
		return (err);
	if (v < INT_MIN || v > INT_MAX)
		return (-ERANGE);
	*res = (int)v;
	return (0);
}

int
kstrtou16(const char *s, unsigned int base, u16 *res)
{
	unsigned long long v;
	int err = kstrtoull(s, base, &v);

	if (err != 0)
		return (err);
	if (v > USHRT_MAX)
		return (-ERANGE);
	*res = (u16)v;
	return (0);
}

int
kstrtou8(const char *s, unsigned int base, u8 *res)
{
	unsigned long long v;
	int err = kstrtoull(s, base, &v);

	if (err != 0)
		return (err);
	if (v > UCHAR_MAX)
		return (-ERANGE);
	*res = (u8)v;
	return (0);
}

int
kstrtos8(const char *s, unsigned int base, s8 *res)
{
	long long v;
	int err = kstrtoll(s, base, &v);

	if (err != 0)
		return (err);
	if (v < SCHAR_MIN || v > SCHAR_MAX)
		return (-ERANGE);
	*res = (s8)v;
	return (0);
}

/* y/Y/1/on are true, n/N/0/off false (only the first letters count). */
int
kstrtobool(const char *s, bool *res)
{
	if (s == NULL)
		return (-EINVAL);
	switch (s[0]) {
	case 'y': case 'Y': case 't': case 'T': case '1':
		*res = true;
		return (0);
	case 'n': case 'N': case 'f': case 'F': case '0':
		*res = false;
		return (0);
	case 'o': case 'O':
		if (s[1] == 'n' || s[1] == 'N') {
			*res = true;
			return (0);
		}
		if (s[1] == 'f' || s[1] == 'F') {
			*res = false;
			return (0);
		}
		break;
	default:
		break;
	}
	return (-EINVAL);
}

unsigned long long
simple_strtoull(const char *cp, char **endp, unsigned int base)
{
	unsigned long long v;
	bool ovf;
	size_t n;

	if (base > 16)
		base = 10;
	cp = lkpi_fixup_radix(cp, &base);
	n = lkpi_parse_digits(cp, base, &v, &ovf);
	if (endp != NULL)
		*endp = (char *)cp + n;
	return (v);
}

unsigned long
simple_strtoul(const char *cp, char **endp, unsigned int base)
{
	return ((unsigned long)simple_strtoull(cp, endp, base));
}

long
simple_strtol(const char *cp, char **endp, unsigned int base)
{
	if (*cp == '-')
		return (-(long)simple_strtoull(cp + 1, endp, base));
	return ((long)simple_strtoull(cp, endp, base));
}

/* --- string_helpers.h ---------------------------------------------------- */

/*
 * size * blk_size with three significant digits and a unit, e.g.
 * "4.00 MiB", "9.77 kB", "512 B"; the length the full text needs.
 */
int
string_get_size(u64 size, u64 blk_size, enum string_size_units units,
    char *buf, int len)
{
	static const char *const u10[] =
	    { "B", "kB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB" };
	static const char *const u2[] =
	    { "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB" };
	const char *const *names = (units & STRING_UNITS_MASK) ==
	    STRING_UNITS_2 ? u2 : u10;
	u64 div = (units & STRING_UNITS_MASK) == STRING_UNITS_2 ? 1024 : 1000;
	u64 rem = 0, milli, unit, scaled, pow;
	const char *sep = (units & STRING_UNITS_NO_SPACE) ? "" : " ";
	char unitbuf[8];
	int i = 0, dec;

	if (len < 0)
		len = 0;
	if (blk_size == 0)
		size = 0;
	/* Shrink the factors until the product fits in 64 bits. */
	while (blk_size >= div && blk_size % div == 0) {
		blk_size /= div;
		i++;
	}
	while (blk_size > 1 && size > U64_MAX / blk_size) {
		size /= div;
		i++;
	}
	size *= blk_size;
	while (size >= div && i < 8) {
		rem = size % div;
		size /= div;
		i++;
	}
	(void) strlcpy(unitbuf, names[i], sizeof (unitbuf));
	if (units & STRING_UNITS_NO_BYTES) {
		size_t l = strlen(unitbuf);

		unitbuf[l - 1] = '\0';
	}
	if (i == 0)
		return (lkpi_snprintf(buf, (size_t)len, "%llu%s%s",
		    (unsigned long long)size, sep, unitbuf));

	/* size is 1..div-1, rem/div the fraction: round to 3 digits. */
	milli = size * 1000 + rem * 1000 / div;
	dec = size >= 100 ? 0 : size >= 10 ? 1 : 2;
	unit = dec == 0 ? 1000 : dec == 1 ? 100 : 10;
	scaled = (milli + unit / 2) / unit;
	pow = dec == 0 ? 1 : dec == 1 ? 10 : 100;
	if (dec == 0)
		return (lkpi_snprintf(buf, (size_t)len, "%llu%s%s",
		    (unsigned long long)scaled, sep, unitbuf));
	return (lkpi_snprintf(buf, (size_t)len, "%llu.%0*llu%s%s",
	    (unsigned long long)(scaled / pow), dec,
	    (unsigned long long)(scaled % pow), sep, unitbuf));
}

/* --- sscanf() ------------------------------------------------------------ */

enum lkpi_scan_len { SL_INT, SL_HH, SL_H, SL_L, SL_LL, SL_Z };

static void
lkpi_scan_store(void *p, enum lkpi_scan_len len, unsigned long long v)
{
	switch (len) {
	case SL_HH:
		*(unsigned char *)p = (unsigned char)v;
		break;
	case SL_H:
		*(unsigned short *)p = (unsigned short)v;
		break;
	case SL_L:
		*(unsigned long *)p = (unsigned long)v;
		break;
	case SL_LL:
		*(unsigned long long *)p = v;
		break;
	case SL_Z:
		*(size_t *)p = (size_t)v;
		break;
	default:
		*(unsigned int *)p = (unsigned int)v;
		break;
	}
}

/*
 * The subset of C's sscanf() the drivers use: %d %i %u %x %X %o %s %c %n
 * and %%, with '*', a field width and the lengths hh h l ll z j t. White
 * space in the format matches any amount of input white space. Returns
 * the number of conversions assigned.
 */
int
vsscanf(const char *buf, const char *fmt, va_list ap)
{
	const char *s = buf;
	int assigned = 0;

	while (*fmt != '\0') {
		enum lkpi_scan_len len = SL_INT;
		bool skip = false;
		unsigned int width = 0, base;
		char c;

		if (isspace(*fmt)) {
			while (isspace(*fmt))
				fmt++;
			s = skip_spaces(s);
			continue;
		}
		if (*fmt != '%') {
			if (*s != *fmt)
				break;
			s++;
			fmt++;
			continue;
		}
		fmt++;
		if (*fmt == '%') {
			s = skip_spaces(s);
			if (*s != '%')
				break;
			s++;
			fmt++;
			continue;
		}
		if (*fmt == '*') {
			skip = true;
			fmt++;
		}
		while (isdigit(*fmt)) {
			if (width < 100000)
				width = width * 10 + (unsigned int)(*fmt - '0');
			fmt++;
		}
		if (*fmt == 'h') {
			fmt++;
			len = SL_H;
			if (*fmt == 'h') {
				fmt++;
				len = SL_HH;
			}
		} else if (*fmt == 'l') {
			fmt++;
			len = SL_L;
			if (*fmt == 'l') {
				fmt++;
				len = SL_LL;
			}
		} else if (*fmt == 'L' || *fmt == 'q') {
			fmt++;
			len = SL_LL;
		} else if (*fmt == 'z' || *fmt == 'Z' || *fmt == 'j' ||
		    *fmt == 't') {
			fmt++;
			len = SL_Z;
		}
		c = *fmt++;
		switch (c) {
		case 'n':
			if (!skip)
				lkpi_scan_store(va_arg(ap, void *), len,
				    (unsigned long long)(s - buf));
			continue;
		case 'c': {
			char *d = skip ? NULL : va_arg(ap, char *);
			unsigned int i;

			if (width == 0)
				width = 1;
			for (i = 0; i < width; i++) {
				if (s[i] == '\0')
					break;
			}
			if (i < width)
				goto out;
			if (d != NULL)
				(void) memcpy(d, s, width);
			s += width;
			if (!skip)
				assigned++;
			continue;
		}
		case 's': {
			char *d = skip ? NULL : va_arg(ap, char *);
			unsigned int i = 0;

			s = skip_spaces(s);
			if (*s == '\0')
				goto out;
			while (*s != '\0' && !isspace(*s) &&
			    (width == 0 || i < width)) {
				if (d != NULL)
					d[i] = *s;
				i++;
				s++;
			}
			if (d != NULL) {
				d[i] = '\0';
				assigned++;
			}
			continue;
		}
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
			char num[72];
			unsigned long long v;
			unsigned int i = 0, maxw;
			bool neg = false, ovf;
			const char *p;
			size_t n;

			/* Copy the field (width-limited), then parse it. */
			s = skip_spaces(s);
			maxw = (width == 0 || width > sizeof (num) - 1) ?
			    sizeof (num) - 1 : width;
			while (s[i] != '\0' && !isspace(s[i]) && i < maxw) {
				num[i] = s[i];
				i++;
			}
			num[i] = '\0';
			p = num;
			if ((c == 'd' || c == 'i') && (*p == '-' || *p == '+'))
				neg = (*p++ == '-');
			else if (*p == '+')
				p++;
			base = (c == 'x' || c == 'X') ? 16 : (c == 'o') ? 8 :
			    (c == 'i') ? 0 : 10;
			p = lkpi_fixup_radix(p, &base);
			n = lkpi_parse_digits(p, base, &v, &ovf);
			if (n == 0)
				goto out;
			s += (size_t)(p + n - num);
			if (!skip) {
				lkpi_scan_store(va_arg(ap, void *), len,
				    neg ? 0ULL - v : v);
				assigned++;
			}
			continue;
		}
		default:
			goto out;
		}
	}
out:
	return (assigned);
}

int
sscanf(const char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsscanf(buf, fmt, ap);
	va_end(ap);
	return (n);
}
