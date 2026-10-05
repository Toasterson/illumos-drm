/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's seq_buf appends (<linux/seq_buf.h>, which describes the
 * overflow rules). Every append either fits completely or marks the
 * buffer overflowed; it never stores a partial piece.
 */

#include <linux/seq_buf.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#include <linux/bug.h>

/* Whether n more bytes fit (never true once overflowed). */
static bool
lkpi_seq_buf_fits(struct seq_buf *s, size_t n)
{
	return (s->len <= s->size && n <= s->size - s->len);
}

static int
lkpi_seq_buf_overflow(struct seq_buf *s)
{
	seq_buf_set_overflow(s);
	return (-1);
}

int
seq_buf_vprintf(struct seq_buf *s, const char *fmt, va_list ap)
{
	int n;

	WARN_ON(s->size == 0);
	if (s->len < s->size) {
		n = vsnprintf(s->buffer + s->len, s->size - s->len, fmt, ap);
		/* The text and its NUL must fit. */
		if (n >= 0 && (size_t)n < s->size - s->len) {
			s->len += (size_t)n;
			return (0);
		}
	}
	return (lkpi_seq_buf_overflow(s));
}

int
seq_buf_printf(struct seq_buf *s, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = seq_buf_vprintf(s, fmt, ap);
	va_end(ap);
	return (ret);
}

int
seq_buf_puts(struct seq_buf *s, const char *str)
{
	size_t n = strlen(str);

	WARN_ON(s->size == 0);
	/* Stores the NUL too, but len does not count it. */
	if (!lkpi_seq_buf_fits(s, n + 1))
		return (lkpi_seq_buf_overflow(s));
	memcpy(s->buffer + s->len, str, n + 1);
	s->len += n;
	return (0);
}

int
seq_buf_putc(struct seq_buf *s, unsigned char c)
{
	WARN_ON(s->size == 0);
	if (!lkpi_seq_buf_fits(s, 1))
		return (lkpi_seq_buf_overflow(s));
	s->buffer[s->len++] = (char)c;
	return (0);
}

int
seq_buf_putmem(struct seq_buf *s, const void *mem, unsigned int len)
{
	WARN_ON(s->size == 0);
	if (!lkpi_seq_buf_fits(s, len))
		return (lkpi_seq_buf_overflow(s));
	memcpy(s->buffer + s->len, mem, len);
	s->len += len;
	return (0);
}
