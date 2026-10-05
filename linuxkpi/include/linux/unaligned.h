/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's unaligned accessors (include/linux/unaligned.h, GPL-2.0;
 * reimplemented with memcpy, which gcc turns into plain moves on x86).
 */
#ifndef _LINUX_UNALIGNED_H
#define	_LINUX_UNALIGNED_H

#include <linux/types.h>
#include <lkpi/byteorder.h>

#define	__lkpi_get_unaligned(T, p) ({ T __v; \
	__builtin_memcpy(&__v, (p), sizeof (__v)); __v; })
#define	__lkpi_put_unaligned(T, v, p) do { T __v = (v); \
	__builtin_memcpy((p), &__v, sizeof (__v)); } while (0)
#define	get_unaligned(p)	__lkpi_get_unaligned(typeof (*(p)), (p))
#define	put_unaligned(v, p)	__lkpi_put_unaligned(typeof (*(p)), (v), (p))
#define	get_unaligned_le16(p)	__lkpi_get_unaligned(u16, (p))
#define	get_unaligned_le32(p)	__lkpi_get_unaligned(u32, (p))
#define	get_unaligned_le64(p)	__lkpi_get_unaligned(u64, (p))
#define	get_unaligned_be16(p)	__swab16(__lkpi_get_unaligned(u16, (p)))
#define	get_unaligned_be32(p)	__swab32(__lkpi_get_unaligned(u32, (p)))
#define	get_unaligned_be64(p)	__swab64(__lkpi_get_unaligned(u64, (p)))
#define	put_unaligned_le16(v, p)	__lkpi_put_unaligned(u16, (v), (p))
#define	put_unaligned_le32(v, p)	__lkpi_put_unaligned(u32, (v), (p))
#define	put_unaligned_le64(v, p)	__lkpi_put_unaligned(u64, (v), (p))
#define	put_unaligned_be16(v, p)	__lkpi_put_unaligned(u16, __swab16(v), (p))
#define	put_unaligned_be32(v, p)	__lkpi_put_unaligned(u32, __swab32(v), (p))
#define	put_unaligned_be64(v, p)	__lkpi_put_unaligned(u64, __swab64(v), (p))

#endif
