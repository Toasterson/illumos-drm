/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <linux/ctype.h> (GPL-2.0; reimplemented for ASCII).
 */
#ifndef _LINUX_CTYPE_H
#define	_LINUX_CTYPE_H
static inline int lkpi_isdigit(int c) { return (c >= '0' && c <= '9'); }
static inline int lkpi_isupper(int c) { return (c >= 'A' && c <= 'Z'); }
static inline int lkpi_islower(int c) { return (c >= 'a' && c <= 'z'); }
static inline int lkpi_isalpha(int c) { return (lkpi_isupper(c) || lkpi_islower(c)); }
static inline int lkpi_isalnum(int c) { return (lkpi_isalpha(c) || lkpi_isdigit(c)); }
static inline int lkpi_isspace(int c) { return (c == ' ' || (c >= '\t' && c <= '\r')); }
static inline int lkpi_isxdigit(int c) { return (lkpi_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')); }
static inline int lkpi_isprint(int c) { return (c >= 0x20 && c < 0x7f); }
static inline int lkpi_isgraph(int c) { return (c > 0x20 && c < 0x7f); }
static inline int lkpi_ispunct(int c) { return (lkpi_isgraph(c) && !lkpi_isalnum(c)); }
static inline int lkpi_iscntrl(int c) { return ((c >= 0 && c < 0x20) || c == 0x7f); }
static inline int lkpi_tolower(int c) { return (lkpi_isupper(c) ? c - 'A' + 'a' : c); }
static inline int lkpi_toupper(int c) { return (lkpi_islower(c) ? c - 'a' + 'A' : c); }
#define	isdigit(c)	lkpi_isdigit(c)
#define	isupper(c)	lkpi_isupper(c)
#define	islower(c)	lkpi_islower(c)
#define	isalpha(c)	lkpi_isalpha(c)
#define	isalnum(c)	lkpi_isalnum(c)
#define	isspace(c)	lkpi_isspace(c)
#define	isxdigit(c)	lkpi_isxdigit(c)
#define	isprint(c)	lkpi_isprint(c)
#define	isgraph(c)	lkpi_isgraph(c)
#define	ispunct(c)	lkpi_ispunct(c)
#define	iscntrl(c)	lkpi_iscntrl(c)
#define	tolower(c)	lkpi_tolower(c)
#define	toupper(c)	lkpi_toupper(c)
#define	_tolower(c)	((c) | 0x20)
#endif
