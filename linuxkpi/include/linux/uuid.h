/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's uuid_t and guid_t (16 bytes). */
#ifndef _LINUX_UUID_H
#define	_LINUX_UUID_H
#include <linux/types.h>
#include <linux/string.h>
#include <linux/random.h>
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
#define	guid_null	(*(const guid_t *)lkpi_guid_zero)
static const __u8 lkpi_guid_zero[UUID_SIZE] __attribute__((unused)) = { 0 };
#define	guid_is_null(g)		(memcmp((g), lkpi_guid_zero, sizeof (guid_t)) == 0)
#define	import_guid(dst, src)	memcpy((dst), (src), sizeof (guid_t))
#define	export_guid(dst, src)	memcpy((dst), (src), sizeof (guid_t))
/* A random (version 4, variant 1) GUID, as Linux's guid_gen(). */
static inline void
guid_gen(guid_t *g)
{
	get_random_bytes(g, sizeof (guid_t));
	g->b[7] = (g->b[7] & 0x0f) | 0x40;
	g->b[8] = (g->b[8] & 0x3f) | 0x80;
}
#define	guid_equal(a, b)	(memcmp((a), (b), sizeof (guid_t)) == 0)
#define	uuid_equal(a, b)	(memcmp((a), (b), sizeof (uuid_t)) == 0)
#define	guid_copy(d, s)		memcpy((d), (s), sizeof (guid_t))
#define	uuid_copy(d, s)		memcpy((d), (s), sizeof (uuid_t))
#endif
