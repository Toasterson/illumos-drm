/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's FIELD_PREP()/FIELD_GET() (include/linux/bitfield.h, GPL-2.0;
 * reimplemented without the compile-time mask checks). The mask must be a
 * non-zero constant with contiguous bits.
 */
#ifndef _LINUX_BITFIELD_H
#define	_LINUX_BITFIELD_H

#include <linux/build_bug.h>
#include <linux/bits.h>
#include <lkpi/byteorder.h>

#define	__bf_shf(x)		(__builtin_ffsll(x) - 1)
#define	FIELD_MAX(mask)		((typeof (mask))((mask) >> __bf_shf(mask)))
#define	FIELD_FIT(mask, val) \
	(!((((typeof (mask))(val)) << __bf_shf(mask)) & ~(mask)))
#define	FIELD_PREP(mask, val) \
	((typeof (mask))(((typeof (mask))(val) << __bf_shf(mask)) & (mask)))
#define	FIELD_PREP_CONST(mask, val) \
	(((typeof (mask))(val) << __bf_shf(mask)) & (mask))
#define	FIELD_GET(mask, reg) \
	((typeof (mask))(((reg) & (mask)) >> __bf_shf(mask)))
#define	FIELD_MODIFY(mask, reg_p, val) \
	(*(reg_p) = (*(reg_p) & ~(mask)) | FIELD_PREP(mask, val))

static inline u32
u32_encode_bits(u32 v, u32 field)
{
	return ((v << __bf_shf(field)) & field);
}

static inline u32
u32_get_bits(u32 v, u32 field)
{
	return ((v & field) >> __bf_shf(field));
}

static inline u64
u64_encode_bits(u64 v, u64 field)
{
	return ((v << __bf_shf(field)) & field);
}

static inline u64
u64_get_bits(u64 v, u64 field)
{
	return ((v & field) >> __bf_shf(field));
}

static inline u16
u16_encode_bits(u16 v, u16 field)
{
	return ((u16)((v << __bf_shf(field)) & field));
}

static inline u16
u16_get_bits(u16 v, u16 field)
{
	return ((u16)((v & field) >> __bf_shf(field)));
}

static inline u8
u8_encode_bits(u8 v, u8 field)
{
	return ((u8)((v << __bf_shf(field)) & field));
}

static inline u8
u8_get_bits(u8 v, u8 field)
{
	return ((u8)((v & field) >> __bf_shf(field)));
}

static inline void
u32p_replace_bits(u32 *p, u32 val, u32 field)
{
	*p = (*p & ~field) | u32_encode_bits(val, field);
}

static inline u32
u32_replace_bits(u32 old, u32 val, u32 field)
{
	return ((old & ~field) | u32_encode_bits(val, field));
}

static inline __le32
le32_encode_bits(u32 v, u32 field)
{
	return (cpu_to_le32(u32_encode_bits(v, field)));
}

static inline u32
le32_get_bits(__le32 v, u32 field)
{
	return (u32_get_bits(le32_to_cpu(v), field));
}

#endif
