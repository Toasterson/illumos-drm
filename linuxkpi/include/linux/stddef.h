/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <linux/stddef.h> (GPL-2.0; reimplemented): offsetofend(),
 * sizeof_field(), struct_group() and the flexible-array helpers.
 */
#ifndef _LINUX_STDDEF_H
#define	_LINUX_STDDEF_H

#include <linux/compiler_types.h>

#define	sizeof_field(TYPE, MEMBER)	sizeof ((((TYPE *)0)->MEMBER))
#define	offsetofend(TYPE, MEMBER) \
	(offsetof(TYPE, MEMBER) + sizeof_field(TYPE, MEMBER))
#define	__struct_group(TAG, NAME, ATTRS, MEMBERS...) \
	union { \
		struct { MEMBERS } ATTRS; \
		struct TAG { MEMBERS } ATTRS NAME; \
	} ATTRS
#define	struct_group(NAME, MEMBERS...)	__struct_group(, NAME, , MEMBERS)
#define	struct_group_attr(NAME, ATTRS, MEMBERS...) \
	__struct_group(, NAME, ATTRS, MEMBERS)
#define	struct_group_tagged(TAG, NAME, MEMBERS...) \
	__struct_group(TAG, NAME, , MEMBERS)
#define	DECLARE_FLEX_ARRAY(TYPE, NAME) \
	struct { struct { } __empty_ ## NAME; TYPE NAME[]; }
#define	__DECLARE_FLEX_ARRAY(T, N)	DECLARE_FLEX_ARRAY(T, N)
#define	TRAILING_OVERLAP(TYPE, NAME, FAM, MEMBERS) \
	union { TYPE NAME; struct { unsigned char __offset_to_##FAM[offsetof(TYPE, FAM)]; MEMBERS }; }

enum {
	__LKPI_STDDEF_UNUSED
};

#endif
