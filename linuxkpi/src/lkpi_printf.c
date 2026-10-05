/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The layer's formatted output: a complete C99 vsnprintf() (flags, width
 * and precision including '*', the length modifiers hh h l ll z t j and
 * the conversions d i u o x X c s p %) with Linux's %p extensions, and on
 * top of it snprintf()/scnprintf()/sprintf()/kasprintf(), printk() over
 * cmn_err(9F), the hex dump helpers and the WARN()/BUG() back ends.
 * An independent implementation written from the C standard and from the
 * documented behaviour of Linux's format extensions
 * (Documentation/core-api/printk-formats.rst):
 *
 *	%pa[p] %pad	phys_addr_t * / dma_addr_t *: 0x%016llx of the value
 *	%pe		an ERR_PTR(): "-ENOMEM" for known errnos, else "-123"
 *	%ph[CDN]	a buffer in hex, the field width is the byte count
 *			(default 1, at most 64); separators ' ', ':', '-', none
 *	%pV		struct va_format *: its format and arguments
 *	%pS %ps %pB %pF %pf	the address, 0x and 16 hex digits (no symbols)
 *	%p %px %pK	the pointer, 16 hex digits (%p of NULL: "(null)")
 *	%pR %pr		struct resource *: [mem 0x...-0x... flags 0x...]
 *	%pU[bBlL]	a 16-byte UUID, big- (b) or little-endian (l) layout
 *	%p4cc		a fourcc code (u32 *): four characters and endianness
 *
 * Any other %p<ext> prints the pointer and skips the extension's letters
 * and digits. A dereferencing extension given NULL prints "(null)", one
 * given an address in the first page or an ERR_PTR() "(efault)": the
 * formatter never faults on the bad pointers Linux tolerates.
 */

#include <linux/kernel.h>
#include <linux/ctype.h>
#include <linux/sprintf.h>
#include <linux/printk.h>
#include <linux/bug.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <drm/drm_print.h>
#include <sys/random.h>

/* The tunable of pr_debug() (see <linux/printk.h>). */
volatile int lkpi_debug = 0;

/* printk()'s line buffer; longer messages are cut and end in "...". */
#define	LKPI_PRINTK_MAX		512

/* %pV may nest; a format that recurses deeper prints nothing more. */
#define	LKPI_FMT_MAX_DEPTH	4

#define	FL_MINUS	0x01
#define	FL_PLUS		0x02
#define	FL_SPACE	0x04
#define	FL_HASH		0x08
#define	FL_ZERO		0x10
#define	FL_UPPER	0x20
#define	FL_WIDTH	0x40	/* a field width was given */

struct lkpi_out {
	char *buf;
	size_t size;
	size_t len;		/* would-be length so far */
};

struct lkpi_spec {
	uint_t flags;
	int width;
	int prec;		/* -1: none */
};

static const char lkpi_digits_lc[] = "0123456789abcdef";
static const char lkpi_digits_uc[] = "0123456789ABCDEF";

/*
 * The value %p prints for a pointer: a keyed 64-bit mix of the address,
 * stable while the layer is loaded (so equal pointers print equal), with
 * a random key taken at first use.
 */
static volatile uint64_t lkpi_ptr_key;

static uintptr_t
lkpi_ptr_hash(const void *p)
{
	uint64_t k = lkpi_ptr_key, x;

	if (k == 0) {
		uint64_t nk = 0;

		(void) random_get_pseudo_bytes((uint8_t *)&nk, sizeof (nk));
		nk |= 1;
		(void) atomic_cas_64(&lkpi_ptr_key, 0, nk);
		k = lkpi_ptr_key;
	}
	x = (uint64_t)(uintptr_t)p ^ k;
	x ^= x >> 30;
	x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27;
	x *= 0x94d049bb133111ebULL;
	x ^= x >> 31;
	return ((uintptr_t)(x & 0xffffffffULL));
}

static void
out_c(struct lkpi_out *o, char c)
{
	if (o->size != 0 && o->len < o->size - 1)
		o->buf[o->len] = c;
	o->len++;
}

static void
out_pad(struct lkpi_out *o, char c, int n)
{
	while (n-- > 0)
		out_c(o, c);
}

static void
out_mem(struct lkpi_out *o, const char *s, size_t n)
{
	while (n-- > 0)
		out_c(o, *s++);
}

/* True for a pointer the formatter must not dereference. */
static bool
lkpi_bad_ptr(const void *p)
{
	return ((uintptr_t)p < PAGE_SIZE || IS_ERR(p));
}

/* A string with width (minimum) and precision (maximum length). */
static void
out_str(struct lkpi_out *o, const char *s, const struct lkpi_spec *sp)
{
	size_t n = 0;

	if (s == NULL)
		s = "(null)";
	else if (lkpi_bad_ptr(s))
		s = "(efault)";
	while (s[n] != '\0' && (sp->prec < 0 || n < (size_t)sp->prec))
		n++;
	if (!(sp->flags & FL_MINUS) && sp->width > (int)n)
		out_pad(o, ' ', sp->width - (int)n);
	out_mem(o, s, n);
	if ((sp->flags & FL_MINUS) && sp->width > (int)n)
		out_pad(o, ' ', sp->width - (int)n);
}

/*
 * An integer: `neg` says the value is the magnitude of a negative number.
 * C99 rules: a precision is the minimum digit count (and disables the 0
 * flag); precision 0 prints nothing for 0 except the '#' octal "0".
 */
static void
out_num(struct lkpi_out *o, uint64_t v, bool neg, uint_t base,
    const struct lkpi_spec *sp)
{
	const char *dig = (sp->flags & FL_UPPER) ? lkpi_digits_uc :
	    lkpi_digits_lc;
	char tmp[24];
	char pfx[3];
	int nd = 0, np = 0, zeros = 0, total, prec = sp->prec;
	uint64_t x = v;

	while (x != 0) {
		tmp[nd++] = dig[x % base];
		x /= base;
	}
	if (prec < 0)
		prec = 1;
	if (nd < prec)
		zeros = prec - nd;
	if (base == 8 && (sp->flags & FL_HASH) && zeros == 0 &&
	    (nd == 0 || tmp[nd - 1] != '0'))
		zeros = 1;

	if (neg)
		pfx[np++] = '-';
	else if (sp->flags & FL_PLUS)
		pfx[np++] = '+';
	else if (sp->flags & FL_SPACE)
		pfx[np++] = ' ';
	if (base == 16 && (sp->flags & FL_HASH) && v != 0) {
		pfx[np++] = '0';
		pfx[np++] = (sp->flags & FL_UPPER) ? 'X' : 'x';
	}

	total = np + zeros + nd;
	if ((sp->flags & (FL_ZERO | FL_MINUS)) == FL_ZERO && sp->prec < 0 &&
	    sp->width > total) {
		zeros += sp->width - total;
		total = sp->width;
	}
	if (!(sp->flags & FL_MINUS) && sp->width > total)
		out_pad(o, ' ', sp->width - total);
	out_mem(o, pfx, np);
	out_pad(o, '0', zeros);
	while (nd > 0)
		out_c(o, tmp[--nd]);
	if ((sp->flags & FL_MINUS) && sp->width > total)
		out_pad(o, ' ', sp->width - total);
}

/* A pointer value as Linux prints %px: 16 hex digits, no prefix. */
static void
out_ptr(struct lkpi_out *o, const void *p, const struct lkpi_spec *sp)
{
	struct lkpi_spec s = *sp;

	if (!(s.flags & FL_WIDTH)) {
		s.width = 16;
		s.flags |= FL_ZERO;
	}
	s.flags &= ~(FL_PLUS | FL_SPACE);
	out_num(o, (uintptr_t)p, false, 16, &s);
}

/* NULL for %p: "(null)", right-aligned in a pointer's width (as Linux). */
static void
out_null_ptr(struct lkpi_out *o, const struct lkpi_spec *sp)
{
	struct lkpi_spec s = *sp;

	if (!(s.flags & FL_WIDTH))
		s.width = 16;
	s.prec = -1;
	out_str(o, "(null)", &s);
}

/* "0x" and 16 digits, for %pa/%pad and the addresses of %pS. */
static void
out_hex64(struct lkpi_out *o, uint64_t v, int digits)
{
	struct lkpi_spec s = { .flags = FL_ZERO, .width = digits, .prec = -1 };

	out_c(o, '0');
	out_c(o, 'x');
	out_num(o, v, false, 16, &s);
}

/* --- %pe: the names of the common errnos ----------------------------- */

struct lkpi_errname {
	int err;
	const char *name;
};

/* First match wins, so aliases with equal values are harmless. */
static const struct lkpi_errname lkpi_errnames[] = {
	{ EPERM, "EPERM" }, { ENOENT, "ENOENT" }, { ESRCH, "ESRCH" },
	{ EINTR, "EINTR" }, { EIO, "EIO" }, { ENXIO, "ENXIO" },
	{ E2BIG, "E2BIG" }, { ENOEXEC, "ENOEXEC" }, { EBADF, "EBADF" },
	{ ECHILD, "ECHILD" }, { EAGAIN, "EAGAIN" }, { ENOMEM, "ENOMEM" },
	{ EACCES, "EACCES" }, { EFAULT, "EFAULT" }, { EBUSY, "EBUSY" },
	{ EEXIST, "EEXIST" }, { EXDEV, "EXDEV" }, { ENODEV, "ENODEV" },
	{ ENOTDIR, "ENOTDIR" }, { EISDIR, "EISDIR" }, { EINVAL, "EINVAL" },
	{ ENFILE, "ENFILE" }, { EMFILE, "EMFILE" }, { ENOTTY, "ENOTTY" },
	{ EFBIG, "EFBIG" }, { ENOSPC, "ENOSPC" }, { ESPIPE, "ESPIPE" },
	{ EROFS, "EROFS" }, { EMLINK, "EMLINK" }, { EPIPE, "EPIPE" },
	{ EDOM, "EDOM" }, { ERANGE, "ERANGE" }, { EDEADLK, "EDEADLK" },
	{ ENAMETOOLONG, "ENAMETOOLONG" }, { ENOLCK, "ENOLCK" },
	{ ENOSYS, "ENOSYS" }, { ENOTEMPTY, "ENOTEMPTY" }, { ELOOP, "ELOOP" },
	{ ENOMSG, "ENOMSG" }, { EIDRM, "EIDRM" }, { ENODATA, "ENODATA" },
	{ ETIME, "ETIME" }, { ENOLINK, "ENOLINK" }, { EPROTO, "EPROTO" },
	{ EBADMSG, "EBADMSG" }, { EOVERFLOW, "EOVERFLOW" },
	{ EILSEQ, "EILSEQ" }, { EUSERS, "EUSERS" }, { ENOTSOCK, "ENOTSOCK" },
	{ EMSGSIZE, "EMSGSIZE" }, { EOPNOTSUPP, "EOPNOTSUPP" },
	{ EADDRINUSE, "EADDRINUSE" }, { ENETDOWN, "ENETDOWN" },
	{ ECONNRESET, "ECONNRESET" }, { ENOBUFS, "ENOBUFS" },
	{ ETIMEDOUT, "ETIMEDOUT" }, { ECONNREFUSED, "ECONNREFUSED" },
	{ EHOSTUNREACH, "EHOSTUNREACH" }, { EALREADY, "EALREADY" },
	{ EINPROGRESS, "EINPROGRESS" }, { ECANCELED, "ECANCELED" },
	{ EOWNERDEAD, "EOWNERDEAD" }, { ENOTRECOVERABLE, "ENOTRECOVERABLE" },
	{ ENOTSUP, "ENOTSUP" },
	{ ERESTARTSYS, "ERESTARTSYS" }, { ERESTARTNOINTR, "ERESTARTNOINTR" },
	{ ERESTARTNOHAND, "ERESTARTNOHAND" }, { ENOIOCTLCMD, "ENOIOCTLCMD" },
	{ EPROBE_DEFER, "EPROBE_DEFER" }, { ENOTSUPP, "ENOTSUPP" },
	{ ETOOSMALL, "ETOOSMALL" }, { EREMOTEIO, "EREMOTEIO" },
	{ ENOMEDIUM, "ENOMEDIUM" }, { EHWPOISON, "EHWPOISON" },
};

static void
out_errptr(struct lkpi_out *o, const void *p, const struct lkpi_spec *sp)
{
	long err = PTR_ERR(p);
	char tmp[24];
	size_t i;
	struct lkpi_spec s = *sp;

	if (!IS_ERR(p)) {
		out_ptr(o, p, sp);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(lkpi_errnames); i++) {
		if (lkpi_errnames[i].err == -err) {
			tmp[0] = '-';
			(void) strlcpy(tmp + 1, lkpi_errnames[i].name,
			    sizeof (tmp) - 1);
			s.prec = -1;
			out_str(o, tmp, &s);
			return;
		}
	}
	s.flags &= ~(FL_PLUS | FL_SPACE | FL_HASH);
	out_num(o, (uint64_t)-err, true, 10, &s);
}

/* --- the other %p extensions ----------------------------------------- */

static void
out_hexbuf(struct lkpi_out *o, const u8 *b, const struct lkpi_spec *sp,
    char sep)
{
	int n = (sp->flags & FL_WIDTH) ? sp->width : 1;
	int i;

	if (n > 64)
		n = 64;
	for (i = 0; i < n; i++) {
		if (i != 0 && sep != '\0')
			out_c(o, sep);
		out_c(o, lkpi_digits_lc[b[i] >> 4]);
		out_c(o, lkpi_digits_lc[b[i] & 0xf]);
	}
}

static void
out_uuid(struct lkpi_out *o, const u8 *b, char kind)
{
	/* Little-endian: the first three fields are byte-reversed. */
	static const u8 le_order[16] =
	    { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
	const char *dig = (kind == 'B' || kind == 'L') ? lkpi_digits_uc :
	    lkpi_digits_lc;
	bool le = (kind == 'l' || kind == 'L');
	int i;

	for (i = 0; i < 16; i++) {
		u8 c = b[le ? le_order[i] : i];

		if (i == 4 || i == 6 || i == 8 || i == 10)
			out_c(o, '-');
		out_c(o, dig[c >> 4]);
		out_c(o, dig[c & 0xf]);
	}
}

static void
out_resource(struct lkpi_out *o, const struct resource *r)
{
	const char *type = (r->flags & IORESOURCE_IO) ? "io" :
	    (r->flags & IORESOURCE_MEM) ? "mem" : "???";
	struct lkpi_spec s = { .flags = FL_HASH, .width = 0, .prec = -1 };

	out_c(o, '[');
	out_mem(o, type, strlen(type));
	out_c(o, ' ');
	out_hex64(o, r->start, (r->flags & IORESOURCE_IO) ? 4 : 8);
	out_c(o, '-');
	out_hex64(o, r->end, (r->flags & IORESOURCE_IO) ? 4 : 8);
	out_mem(o, " flags ", 7);
	out_num(o, r->flags, false, 16, &s);
	out_c(o, ']');
}

/* Linux's %p4cc: the four characters, then the byte order (bit 31). */
static void
out_fourcc(struct lkpi_out *o, const u32 *p)
{
	u32 v = *p & ~(1U << 31);
	int i;

	for (i = 0; i < 4; i++) {
		char c = (char)((v >> (i * 8)) & 0xff);

		out_c(o, isprint(c) ? c : '.');
	}
	if (*p & (1U << 31))
		out_mem(o, " (big-endian)", 13);
	else
		out_mem(o, " (little-endian)", 16);
}

static void lkpi_format(struct lkpi_out *o, const char *fmt, va_list *ap,
    int depth);

/*
 * One %p conversion; `fmtp` points after the 'p' and is advanced past the
 * extension letters consumed.
 */
static void
out_pext(struct lkpi_out *o, const char **fmtp, const void *p,
    const struct lkpi_spec *sp, int depth)
{
	const char *f = *fmtp;
	char e = *f;

	switch (e) {
	case 'a':
		f++;
		if (*f == 'd' || *f == 'p')
			f++;
		if (p == NULL || lkpi_bad_ptr(p))
			out_str(o, p == NULL ? NULL : "(efault)", sp);
		else
			out_hex64(o, *(const u64 *)p, 16);
		break;
	case 'e':
		f++;
		out_errptr(o, p, sp);
		break;
	case 'h': {
		char sep = ' ';

		f++;
		if (*f == 'C')
			sep = ':';
		else if (*f == 'D')
			sep = '-';
		else if (*f == 'N')
			sep = '\0';
		if (sep != ' ')
			f++;
		if ((sp->flags & FL_WIDTH) && sp->width == 0)
			break;
		if (p == NULL || lkpi_bad_ptr(p))
			out_str(o, p == NULL ? NULL : "(efault)",
			    &(struct lkpi_spec){ 0, 0, -1 });
		else
			out_hexbuf(o, p, sp, sep);
		break;
	}
	case 'V': {
		const struct va_format *vaf = p;
		va_list va;

		f++;
		if (p == NULL || lkpi_bad_ptr(p) || vaf->fmt == NULL ||
		    lkpi_bad_ptr(vaf->fmt) || vaf->va == NULL) {
			out_str(o, p == NULL ? NULL : "(efault)", sp);
			break;
		}
		if (depth >= LKPI_FMT_MAX_DEPTH)
			break;
		va_copy(va, *vaf->va);
		lkpi_format(o, vaf->fmt, &va, depth + 1);
		va_end(va);
		break;
	}
	case 'S': case 's': case 'B': case 'F': case 'f':
		while (isalnum(*f))
			f++;
		if (p == NULL)
			out_null_ptr(o, sp);
		else
			out_hex64(o, (uintptr_t)p, 16);
		break;
	case 'R': case 'r':
		f++;
		if (*f == 'a')
			f++;
		if (p == NULL || lkpi_bad_ptr(p))
			out_str(o, p == NULL ? NULL : "(efault)", sp);
		else
			out_resource(o, p);
		break;
	case 'U': {
		char kind = 'b';

		f++;
		if (*f == 'b' || *f == 'B' || *f == 'l' || *f == 'L')
			kind = *f++;
		if (p == NULL || lkpi_bad_ptr(p))
			out_str(o, p == NULL ? NULL : "(efault)", sp);
		else
			out_uuid(o, p, kind);
		break;
	}
	case '4':
		if (f[1] == 'c' && f[2] == 'c') {
			f += 3;
			if (p == NULL || lkpi_bad_ptr(p))
				out_str(o, p == NULL ? NULL : "(efault)", sp);
			else
				out_fourcc(o, p);
			break;
		}
		while (isalnum(*f))
			f++;
		out_ptr(o, p, sp);
		break;
	default:
		/*
		 * %p, %px, %pK and every extension the layer lacks. As on
		 * Linux, only %px prints the address: the others print a
		 * keyed hash of it, so that kernel addresses stay out of the
		 * world-readable system log.
		 */
		while (isalnum(*f))
			f++;
		if (p == NULL && e != 'x' && e != 'K')
			out_null_ptr(o, sp);
		else if (e == 'x')
			out_ptr(o, p, sp);
		else
			out_ptr(o, (const void *)lkpi_ptr_hash(p), sp);
		break;
	}
	*fmtp = f;
}

enum lkpi_len {
	LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_Z, LEN_T, LEN_J
};

static void
lkpi_format(struct lkpi_out *o, const char *fmt, va_list *ap, int depth)
{
	while (*fmt != '\0') {
		struct lkpi_spec sp = { .flags = 0, .width = 0, .prec = -1 };
		enum lkpi_len len = LEN_NONE;
		uint64_t uv;
		int64_t sv;
		uint_t base;
		char c = *fmt++;

		if (c != '%') {
			out_c(o, c);
			continue;
		}

		for (;;) {
			c = *fmt;
			if (c == '-')
				sp.flags |= FL_MINUS;
			else if (c == '+')
				sp.flags |= FL_PLUS;
			else if (c == ' ')
				sp.flags |= FL_SPACE;
			else if (c == '#')
				sp.flags |= FL_HASH;
			else if (c == '0')
				sp.flags |= FL_ZERO;
			else
				break;
			fmt++;
		}
		if (*fmt == '*') {
			fmt++;
			sp.width = va_arg(*ap, int);
			sp.flags |= FL_WIDTH;
			if (sp.width < 0) {
				sp.flags |= FL_MINUS;
				sp.width = sp.width == INT_MIN ? INT_MAX :
				    -sp.width;
			}
		} else if (isdigit(*fmt)) {
			sp.flags |= FL_WIDTH;
			while (isdigit(*fmt)) {
				if (sp.width < 100000)
					sp.width = sp.width * 10 + (*fmt - '0');
				fmt++;
			}
		}
		if (*fmt == '.') {
			fmt++;
			sp.prec = 0;
			if (*fmt == '*') {
				fmt++;
				sp.prec = va_arg(*ap, int);
				if (sp.prec < 0)
					sp.prec = -1;
			} else {
				while (isdigit(*fmt)) {
					if (sp.prec < 100000)
						sp.prec = sp.prec * 10 +
						    (*fmt - '0');
					fmt++;
				}
			}
		}

		switch (*fmt) {
		case 'h':
			fmt++;
			len = LEN_H;
			if (*fmt == 'h') {
				fmt++;
				len = LEN_HH;
			}
			break;
		case 'l':
			fmt++;
			len = LEN_L;
			if (*fmt == 'l') {
				fmt++;
				len = LEN_LL;
			}
			break;
		case 'L':
		case 'q':
			fmt++;
			len = LEN_LL;
			break;
		case 'z':
		case 'Z':
			fmt++;
			len = LEN_Z;
			break;
		case 't':
			fmt++;
			len = LEN_T;
			break;
		case 'j':
			fmt++;
			len = LEN_J;
			break;
		default:
			break;
		}

		c = *fmt;
		if (c == '\0') {
			/* A lone '%' at the end prints as itself. */
			out_c(o, '%');
			break;
		}
		fmt++;
		switch (c) {
		case 'd':
		case 'i':
			switch (len) {
			case LEN_HH:
				sv = (signed char)va_arg(*ap, int);
				break;
			case LEN_H:
				sv = (short)va_arg(*ap, int);
				break;
			case LEN_L:
				sv = va_arg(*ap, long);
				break;
			case LEN_LL:
				sv = va_arg(*ap, long long);
				break;
			case LEN_Z:
				sv = va_arg(*ap, ssize_t);
				break;
			case LEN_T:
				sv = va_arg(*ap, ptrdiff_t);
				break;
			case LEN_J:
				sv = va_arg(*ap, intmax_t);
				break;
			default:
				sv = va_arg(*ap, int);
				break;
			}
			out_num(o, sv < 0 ? -(uint64_t)sv : (uint64_t)sv,
			    sv < 0, 10, &sp);
			break;
		case 'u':
		case 'o':
		case 'x':
		case 'X':
			switch (len) {
			case LEN_HH:
				uv = (unsigned char)va_arg(*ap, uint_t);
				break;
			case LEN_H:
				uv = (unsigned short)va_arg(*ap, uint_t);
				break;
			case LEN_L:
				uv = va_arg(*ap, unsigned long);
				break;
			case LEN_LL:
				uv = va_arg(*ap, unsigned long long);
				break;
			case LEN_Z:
				uv = va_arg(*ap, size_t);
				break;
			case LEN_T:
				uv = (uint64_t)va_arg(*ap, ptrdiff_t);
				break;
			case LEN_J:
				uv = va_arg(*ap, uintmax_t);
				break;
			default:
				uv = va_arg(*ap, uint_t);
				break;
			}
			base = (c == 'u') ? 10 : (c == 'o') ? 8 : 16;
			if (c == 'X')
				sp.flags |= FL_UPPER;
			sp.flags &= ~(FL_PLUS | FL_SPACE);
			out_num(o, uv, false, base, &sp);
			break;
		case 'c': {
			char ch = (char)va_arg(*ap, int);

			if (!(sp.flags & FL_MINUS) && sp.width > 1)
				out_pad(o, ' ', sp.width - 1);
			out_c(o, ch);
			if ((sp.flags & FL_MINUS) && sp.width > 1)
				out_pad(o, ' ', sp.width - 1);
			break;
		}
		case 's':
			out_str(o, va_arg(*ap, const char *), &sp);
			break;
		case 'p':
			out_pext(o, &fmt, va_arg(*ap, const void *), &sp,
			    depth);
			break;
		case 'n':
			/* Not supported (as on Linux): the argument is skipped. */
			(void) va_arg(*ap, void *);
			break;
		case '%':
			out_c(o, '%');
			break;
		default:
			/* An unknown conversion prints as written. */
			out_c(o, '%');
			out_c(o, c);
			break;
		}
	}
}

int
lkpi_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct lkpi_out o = { .buf = buf, .size = size, .len = 0 };
	va_list aq;

	if (size > INT_MAX) {
		WARN_ONCE(1, "vsnprintf: size %zu too large", size);
		return (0);
	}
	if (fmt == NULL) {
		if (size != 0)
			buf[0] = '\0';
		return (0);
	}
	va_copy(aq, ap);
	lkpi_format(&o, fmt, &aq, 0);
	va_end(aq);
	if (size != 0)
		buf[o.len < size ? o.len : size - 1] = '\0';
	return (o.len > INT_MAX ? INT_MAX : (int)o.len);
}

int
lkpi_snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = lkpi_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return (n);
}

int
lkpi_vscnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	int n = lkpi_vsnprintf(buf, size, fmt, ap);

	if (n < 0 || size == 0)
		return (0);
	if ((size_t)n < size)
		return (n);
	return ((int)(size - 1));
}

int
lkpi_scnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = lkpi_vscnprintf(buf, size, fmt, ap);
	va_end(ap);
	return (n);
}

int
lkpi_sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = lkpi_vsnprintf(buf, INT_MAX, fmt, ap);
	va_end(ap);
	return (n);
}

char *
kvasprintf(gfp_t gfp, const char *fmt, va_list ap)
{
	va_list aq;
	char *p;
	int n;

	va_copy(aq, ap);
	n = lkpi_vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	p = kmalloc((size_t)n + 1, gfp);
	if (p == NULL)
		return (NULL);
	(void) lkpi_vsnprintf(p, (size_t)n + 1, fmt, ap);
	return (p);
}

/*
 * Always a copy: kfree_const() frees what kstrdup_const() and
 * kvasprintf_const() return (see src/lkpi_string.c).
 */
const char *
kvasprintf_const(gfp_t gfp, const char *fmt, va_list ap)
{
	return (kvasprintf(gfp, fmt, ap));
}

char *
kasprintf(gfp_t gfp, const char *fmt, ...)
{
	va_list ap;
	char *p;

	va_start(ap, fmt);
	p = kvasprintf(gfp, fmt, ap);
	va_end(ap);
	return (p);
}

/* --- printk() ---------------------------------------------------------- */

/*
 * Linux parses the level from the formatted text (so a level passed with
 * "%s" counts). Levels: '0'..'4' CE_WARN, '5' CE_NOTE, '6'/'7' CE_NOTE
 * to the system log only, 'c' continuation (CE_CONT); no level is
 * Linux's default (KERN_WARNING's console visibility, printed as CE_NOTE).
 */
int
lkpi_vprintk(const char *fmt, va_list ap)
{
	char buf[LKPI_PRINTK_MAX];
	const char *text = buf;
	int n, level = -1;
	size_t len;

	n = lkpi_vsnprintf(buf, sizeof (buf), fmt, ap);
	if (n >= (int)sizeof (buf)) {
		/* Cut: mark the cut with "..." and keep the final newline. */
		bcopy("...\n", buf + sizeof (buf) - 5, 5);
	}
	while (text[0] == KERN_SOH_ASCII && text[1] != '\0') {
		if ((text[1] >= '0' && text[1] <= '7') || text[1] == 'c')
			level = text[1];
		text += 2;
	}
	len = strlen(text);
	if (level == 'c') {
		if (len != 0)
			cmn_err(CE_CONT, "%s", text);
		return ((int)len);
	}
	/* cmn_err() ends the line itself. */
	if (len != 0 && text[len - 1] == '\n')
		buf[(text - buf) + --len] = '\0';
	if (level >= '0' && level <= '4')
		cmn_err(CE_WARN, "%s", text);
	else if (level == '6' || level == '7')
		cmn_err(CE_NOTE, "!%s", text);
	else
		cmn_err(CE_NOTE, "%s", text);
	return ((int)len);
}

int
lkpi_printk(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = lkpi_vprintk(fmt, ap);
	va_end(ap);
	return (n);
}

/* --- hex dumps --------------------------------------------------------- */

/*
 * One line of a hex dump as Linux formats it: groups of `groupsize`
 * bytes (1, 2, 4 or 8; read in host byte order) separated by spaces, and
 * with `ascii` the printable characters from column
 * rowsize * 2 + rowsize / groupsize + 1. Returns the length the full line
 * needs (excluding the NUL), like snprintf().
 */
int
hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize,
    char *linebuf, size_t linebuflen, bool ascii)
{
	const u8 *p = buf;
	struct lkpi_out o = { .buf = linebuf, .size = linebuflen, .len = 0 };
	int ngroups, i, j, ascii_col;

	if (rowsize != 16 && rowsize != 32)
		rowsize = 16;
	if (len > (size_t)rowsize)
		len = rowsize;
	if (groupsize != 1 && groupsize != 2 && groupsize != 4 &&
	    groupsize != 8)
		groupsize = 1;
	if ((len % groupsize) != 0)
		groupsize = 1;
	ngroups = (int)len / groupsize;
	ascii_col = rowsize * 2 + rowsize / groupsize + 1;

	if (len != 0 && p != NULL) {
		for (i = 0; i < ngroups; i++) {
			uint64_t v = 0;
			u8 *vb = (u8 *)&v;

			if (i != 0)
				out_c(&o, ' ');
			for (j = 0; j < groupsize; j++)
				vb[j] = p[i * groupsize + j];
			/* Host (little-endian) order: the value's digits. */
			for (j = groupsize - 1; j >= 0; j--) {
				out_c(&o, lkpi_digits_lc[vb[j] >> 4]);
				out_c(&o, lkpi_digits_lc[vb[j] & 0xf]);
			}
		}
		if (ascii) {
			while (o.len < (size_t)ascii_col)
				out_c(&o, ' ');
			for (i = 0; i < (int)len; i++)
				out_c(&o, (p[i] >= 0x20 && p[i] < 0x7f) ?
				    (char)p[i] : '.');
		}
	}
	if (linebuflen != 0)
		linebuf[o.len < linebuflen ? o.len : linebuflen - 1] = '\0';
	return ((int)o.len);
}

void
print_hex_dump(const char *level, const char *prefix_str, int prefix_type,
    int rowsize, int groupsize, const void *buf, size_t len, bool ascii)
{
	const u8 *p = buf;
	char line[32 * 3 + 2 + 32 + 1];
	size_t i, n;

	if (rowsize != 16 && rowsize != 32)
		rowsize = 16;
	if (level == NULL)
		level = "";
	if (prefix_str == NULL)
		prefix_str = "";
	for (i = 0; i < len; i += rowsize) {
		n = len - i < (size_t)rowsize ? len - i : (size_t)rowsize;
		(void) hex_dump_to_buffer(p + i, n, rowsize, groupsize, line,
		    sizeof (line), ascii);
		switch (prefix_type) {
		case DUMP_PREFIX_ADDRESS:
			lkpi_printk("%s%s%px: %s\n", level, prefix_str,
			    p + i, line);
			break;
		case DUMP_PREFIX_OFFSET:
			lkpi_printk("%s%s%.8zx: %s\n", level, prefix_str, i,
			    line);
			break;
		default:
			lkpi_printk("%s%s%s\n", level, prefix_str, line);
			break;
		}
	}
}

/* --- WARN() and BUG() -------------------------------------------------- */

/* The number of WARN()s that fired since the layer loaded (tests read it). */
volatile uint64_t lkpi_warnings;

void
lkpi_warn(const char *file, int line, const char *fmt, ...)
{
	char msg[LKPI_PRINTK_MAX];
	va_list ap;
	size_t len;

	atomic_inc_64(&lkpi_warnings);
	msg[0] = '\0';
	if (fmt != NULL) {
		va_start(ap, fmt);
		(void) lkpi_vsnprintf(msg, sizeof (msg), fmt, ap);
		va_end(ap);
	}
	len = strlen(msg);
	if (len != 0 && msg[len - 1] == '\n')
		msg[--len] = '\0';
	cmn_err(CE_WARN, "WARNING at %s:%d%s%s", file, line,
	    len != 0 ? ": " : "", msg);
}

void
lkpi_bug(const char *file, int line)
{
	panic("kernel BUG at %s:%d", file, line);
}
