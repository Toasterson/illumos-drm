/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's seq_buf (include/linux/seq_buf.h, GPL-2.0; reimplemented in
 * src/lkpi_seq_buf.c): text appended piecewise to a caller's fixed
 * buffer, as drm_edid.c and the i915 display code use it to build log
 * lines.
 *
 * A seq_buf is `size` bytes at `buffer` of which `len` are used. An
 * append that does not fit sets len to size + 1 (overflowed) and leaves
 * the text before it intact; seq_buf_has_overflowed() is len > size, and
 * every later append fails too until seq_buf_clear(). Appends return 0,
 * or -1 on overflow.
 *
 * seq_buf_printf()/seq_buf_vprintf() format with the layer's vsnprintf()
 * (Linux's %p extensions) and need room for the text and its NUL;
 * seq_buf_puts() needs room for the string and its NUL and stores both
 * (len does not count the NUL); seq_buf_putc()/seq_buf_putmem() need
 * room for the bytes only and store no NUL. seq_buf_str() NUL-terminates
 * the contents in place and returns the buffer; after an overflow it
 * overwrites the last byte of the buffer with the NUL (the text is then
 * truncated by one byte). seq_buf_used() is the number of bytes of text,
 * at most size.
 *
 * DECLARE_SEQ_BUF(name, size) defines a seq_buf over an anonymous
 * zero-filled array of `size` bytes (automatic in a function, static at
 * file scope). No function here sleeps or allocates.
 */
#ifndef _LINUX_SEQ_BUF_H
#define	_LINUX_SEQ_BUF_H

#include <linux/types.h>
#include <linux/bug.h>
#include <linux/minmax.h>
#include <linux/stdarg.h>

struct seq_buf {
	char *buffer;
	size_t size;
	size_t len;
};

#define	DECLARE_SEQ_BUF(NAME, SIZE) \
	struct seq_buf NAME = { \
		.buffer = (char[SIZE]) { 0 }, \
		.size = (SIZE), \
	}

static inline void
seq_buf_clear(struct seq_buf *s)
{
	s->len = 0;
	if (s->size != 0)
		s->buffer[0] = '\0';
}

static inline void
seq_buf_init(struct seq_buf *s, char *buf, unsigned int size)
{
	s->buffer = buf;
	s->size = size;
	seq_buf_clear(s);
}

static inline bool
seq_buf_has_overflowed(struct seq_buf *s)
{
	return (s->len > s->size);
}

static inline void
seq_buf_set_overflow(struct seq_buf *s)
{
	s->len = s->size + 1;
}

/* Bytes still free (0 after an overflow). */
static inline unsigned int
seq_buf_buffer_left(struct seq_buf *s)
{
	if (seq_buf_has_overflowed(s))
		return (0);
	return ((unsigned int)(s->size - s->len));
}

/* Bytes of text written (at most size). */
static inline unsigned int
seq_buf_used(struct seq_buf *s)
{
	return ((unsigned int)min(s->len, s->size));
}

static inline const char *
seq_buf_str(struct seq_buf *s)
{
	if (WARN_ON(s->size == 0))
		return ("");
	if (seq_buf_buffer_left(s) != 0)
		s->buffer[s->len] = '\0';
	else
		s->buffer[s->size - 1] = '\0';
	return (s->buffer);
}

extern int seq_buf_printf(struct seq_buf *s, const char *fmt, ...)
    __printf(2, 3);
extern int seq_buf_vprintf(struct seq_buf *s, const char *fmt, va_list ap)
    __printf(2, 0);
extern int seq_buf_puts(struct seq_buf *s, const char *str);
extern int seq_buf_putc(struct seq_buf *s, unsigned char c);
extern int seq_buf_putmem(struct seq_buf *s, const void *mem,
    unsigned int len);

#endif
