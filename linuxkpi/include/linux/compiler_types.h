/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's annotations and type helpers (include/linux/compiler_types.h,
 * GPL-2.0). The sparse address spaces (__user, __iomem, __rcu, ...) are
 * empty; __user pointers are illumos user addresses that only the
 * uaccess functions dereference.
 */
#ifndef _LINUX_COMPILER_TYPES_H
#define	_LINUX_COMPILER_TYPES_H

#include <linux/compiler_attributes.h>

#define	__user
#define	__kernel
#define	__iomem
#define	__rcu
#define	__percpu
#define	__force
#define	__safe
#define	__nocast
#define	__chk_user_ptr(x)		((void)0)
#define	__chk_io_ptr(x)			((void)0)
#define	__bitwise
#define	__acquires(x)
#define	__releases(x)
#define	__acquire(x)			((void)0)
#define	__release(x)			((void)0)
#define	__cond_acquires(x)
#define	__cond_lock(x, c)		(c)
#define	__must_hold(x)
#define	__private
#define	ACCESS_PRIVATE(p, member)	((p)->member)
#define	__builtin_warning(x, y...)	(1)
#define	__randomize_layout
#define	__no_randomize_layout
#define	randomized_struct_fields_start	struct {
#define	randomized_struct_fields_end	};
#define	__latent_entropy
#define	__ro_after_init
#define	__read_mostly
#define	__cacheline_aligned		__aligned(64)
#define	__cacheline_aligned_in_smp	__aligned(64)
#define	____cacheline_aligned		__aligned(64)
#define	____cacheline_aligned_in_smp	__aligned(64)
#define	__init
#define	__exit
#define	__initdata
#define	__initconst
#define	__exitdata
#define	__devinit
#define	__visible			__attribute__((__externally_visible__))
#define	notrace
#define	__notrace_funcgraph
#define	__irq_entry
#define	__kprobes
#define	nokprobe_inline			__always_inline
#define	__nocfi
#define	__noscs
#define	__no_kcsan
#define	__percpu_qual
#define	asmlinkage
#define	__compiletime_error(msg)	__attribute__((__error__(msg)))
#define	__compiletime_warning(msg)	__attribute__((__warning__(msg)))
#define	__native_word(t) \
	(sizeof (t) == sizeof (char) || sizeof (t) == sizeof (short) || \
	sizeof (t) == sizeof (int) || sizeof (t) == sizeof (long))
#define	__same_type(a, b)	__builtin_types_compatible_p(typeof (a), typeof (b))
#define	__unqual_scalar_typeof(x)	typeof (x)
#define	__inline_maybe_unused		__maybe_unused
#define	noinline_for_stack		noinline
#define	__diag_push()
#define	__diag_pop()
#define	__diag_ignore(c, v, o, r)
#define	__diag_ignore_all(o, r)
#define	__diag_GCC(v, s, o)
#define	TYPEOF_UNQUAL(exp)		typeof (exp)
#define	auto_type			__auto_type

#endif
