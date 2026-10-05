/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's byte-order conversions for little-endian x86 (Linux's
 * <asm/byteorder.h> and <linux/byteorder/little_endian.h>, GPL-2.0 WITH
 * Linux-syscall-note; reimplemented). Not named asm/byteorder.h: illumos
 * has a header of that name that its own headers include.
 */
#ifndef _LKPI_BYTEORDER_H
#define	_LKPI_BYTEORDER_H

#include <linux/types.h>

#ifndef	__LITTLE_ENDIAN
#define	__LITTLE_ENDIAN		1234
#endif
#ifndef	__LITTLE_ENDIAN_BITFIELD
#define	__LITTLE_ENDIAN_BITFIELD
#endif

#define	__swab16(x)	((u16)__builtin_bswap16((u16)(x)))
#define	__swab32(x)	((u32)__builtin_bswap32((u32)(x)))
#define	__swab64(x)	((u64)__builtin_bswap64((u64)(x)))
#define	swab16(x)	__swab16(x)
#define	swab32(x)	__swab32(x)
#define	swab64(x)	__swab64(x)

#define	cpu_to_le16(x)	((__le16)(u16)(x))
#define	cpu_to_le32(x)	((__le32)(u32)(x))
#define	cpu_to_le64(x)	((__le64)(u64)(x))
#define	le16_to_cpu(x)	((u16)(__le16)(x))
#define	le32_to_cpu(x)	((u32)(__le32)(x))
#define	le64_to_cpu(x)	((u64)(__le64)(x))
#define	cpu_to_be16(x)	((__be16)__swab16(x))
#define	cpu_to_be32(x)	((__be32)__swab32(x))
#define	cpu_to_be64(x)	((__be64)__swab64(x))
#define	be16_to_cpu(x)	__swab16((u16)(__be16)(x))
#define	be32_to_cpu(x)	__swab32((u32)(__be32)(x))
#define	be64_to_cpu(x)	__swab64((u64)(__be64)(x))
#define	__cpu_to_le16(x)	cpu_to_le16(x)
#define	__cpu_to_le32(x)	cpu_to_le32(x)
#define	__cpu_to_le64(x)	cpu_to_le64(x)
#define	__le16_to_cpu(x)	le16_to_cpu(x)
#define	__le32_to_cpu(x)	le32_to_cpu(x)
#define	__le64_to_cpu(x)	le64_to_cpu(x)
#define	__cpu_to_be32(x)	cpu_to_be32(x)
#define	__be32_to_cpu(x)	be32_to_cpu(x)
#define	le16_to_cpup(p)		(*(const u16 *)(p))
#define	le32_to_cpup(p)		(*(const u32 *)(p))
#define	le64_to_cpup(p)		(*(const u64 *)(p))
#define	cpu_to_le32s(p)		((void)(p))
#define	le32_to_cpus(p)		((void)(p))
#define	le32_add_cpu(p, v)	(*(p) = cpu_to_le32(le32_to_cpu(*(p)) + (v)))

#endif
