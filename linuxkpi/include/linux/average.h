/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's exponentially weighted moving averages (include/linux/
 * average.h, GPL-2.0; reimplemented): DECLARE_EWMA(name, precision,
 * weight_rcp) defines struct ewma_<name> with _init, _read and _add.
 */
#ifndef _LINUX_AVERAGE_H
#define	_LINUX_AVERAGE_H
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/log2.h>
#define	DECLARE_EWMA(name, _precision, _weight_rcp) \
	struct ewma_##name { \
		unsigned long internal; \
	}; \
	static inline void ewma_##name##_init(struct ewma_##name *e) \
	{ \
		e->internal = 0; \
	} \
	static inline unsigned long ewma_##name##_read(struct ewma_##name *e) \
	{ \
		return (e->internal >> (_precision)); \
	} \
	static inline void ewma_##name##_add(struct ewma_##name *e, unsigned long val) \
	{ \
		unsigned long internal = READ_ONCE(e->internal); \
		unsigned long weight_rcp = ilog2(_weight_rcp); \
		unsigned long precision = _precision; \
		WRITE_ONCE(e->internal, internal ? \
		    (((internal << weight_rcp) - internal) + \
		    (val << precision)) >> weight_rcp : \
		    (val << precision)); \
	}
#endif
