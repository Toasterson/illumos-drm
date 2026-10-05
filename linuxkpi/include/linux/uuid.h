/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's uuid_t and guid_t (16 bytes). */
#ifndef _LINUX_UUID_H
#define	_LINUX_UUID_H
#include <linux/types.h>
#include <linux/string.h>
#define	UUID_SIZE	16
#define	UUID_STRING_LEN	36
typedef struct { __u8 b[UUID_SIZE]; } guid_t;
typedef struct { __u8 b[UUID_SIZE]; } lkpi_uuid_t;
#define	uuid_t	lkpi_uuid_t
#define	GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) ((guid_t) \
	{{ (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, \
	(b) & 0xff, ((b) >> 8) & 0xff, (c) & 0xff, ((c) >> 8) & 0xff, \
	(d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) }})
extern bool uuid_is_valid(const char *uuid);
#define	guid_equal(a, b)	(memcmp((a), (b), sizeof (guid_t)) == 0)
#define	uuid_equal(a, b)	(memcmp((a), (b), sizeof (uuid_t)) == 0)
#define	guid_copy(d, s)		memcpy((d), (s), sizeof (guid_t))
#define	uuid_copy(d, s)		memcpy((d), (s), sizeof (uuid_t))
#endif
