/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's scope-based cleanup (include/linux/cleanup.h, GPL-2.0;
 * reimplemented on gcc's cleanup attribute). The macro names, their
 * arguments and the _T convention inside lock/unlock/free expressions
 * are Linux's interface; the expansion is the layer's own.
 *
 *	DEFINE_FREE(name, type, free-expr)  +  type p __free(name) = ...;
 *	DEFINE_CLASS(name, type, exit-expr, init-expr, init-args...)
 *	CLASS(name, var)(args)
 *	DEFINE_GUARD(name, type, lock-expr, unlock-expr)
 *	DEFINE_LOCK_GUARD_0/1(name, [type,] lock-expr, unlock-expr, extra...)
 *	guard(name)(args);  scoped_guard(name, args) { ... }
 *	no_free_ptr(p), return_ptr(p), retain_and_null_ptr(p)
 */
#ifndef _LINUX_CLEANUP_H
#define	_LINUX_CLEANUP_H

#include <linux/compiler.h>

#define	DEFINE_FREE(_name, _type, _free) \
	static inline void __free_##_name(void *p) { _type _T = *(_type *)p; _free; }
#define	__free(_name)	__cleanup(__free_##_name)
#define	__get_and_null(p, nullvalue) ({ \
	__auto_type __pp = &(p); __auto_type __v = *__pp; \
	*__pp = nullvalue; __v; })
#define	no_free_ptr(p)		((typeof (p))__get_and_null(p, NULL))
#define	return_ptr(p)		return (no_free_ptr(p))
#define	retain_and_null_ptr(p)	((void)__get_and_null(p, NULL))
#define	take_fd(fd)		__get_and_null(fd, -EBADF)

#define	DEFINE_CLASS(_name, _type, _exit, _init, _init_args...) \
typedef _type class_##_name##_t; \
static inline void class_##_name##_destructor(_type *p) \
	{ _type _T = *p; _exit; } \
static inline _type class_##_name##_constructor(_init_args) \
	{ _type t = _init; return (t); }
#define	EXTEND_CLASS(_name, ext, _init, _init_args...) \
typedef class_##_name##_t class_##_name##ext##_t; \
static inline void class_##_name##ext##_destructor(class_##_name##_t *p) \
	{ class_##_name##_destructor(p); } \
static inline class_##_name##_t class_##_name##ext##_constructor(_init_args) \
	{ class_##_name##_t t = _init; return (t); }
#define	CLASS(_name, var) \
	class_##_name##_t var __cleanup(class_##_name##_destructor) = \
	    class_##_name##_constructor

#define	__guard_ptr(_name)	class_##_name##_lock_ptr
#define	DEFINE_CLASS_IS_GUARD(_name) \
static inline void *class_##_name##_lock_ptr(class_##_name##_t *_T) \
	{ return ((void *)(uintptr_t)*_T); }
#define	DEFINE_GUARD(_name, _type, _lock, _unlock) \
	DEFINE_CLASS(_name, _type, if (_T) { _unlock; }, ({ _lock; _T; }), _type _T); \
	DEFINE_CLASS_IS_GUARD(_name)
#define	DEFINE_GUARD_COND(_name, _ext, _condlock, ...) \
	EXTEND_CLASS(_name, _ext, ({ void *_t = _T; if (_T && !(_condlock)) _t = NULL; _t; }), \
	    class_##_name##_t _T) \
	static inline void *class_##_name##_ext##_lock_ptr(class_##_name##_t *_T) \
	{ return (class_##_name##_lock_ptr(_T)); }

#define	guard(_name)	CLASS(_name, __UNIQUE_ID(guard))
#define	scoped_guard(_name, args...) \
	for (CLASS(_name, scope)(args), *__lkpi_done = NULL; \
	    __guard_ptr(_name)(&scope) != NULL && __lkpi_done == NULL; \
	    __lkpi_done = (void *)1)
#define	scoped_cond_guard(_name, _fail, args...) \
	for (CLASS(_name, scope)(args), *__lkpi_done = NULL; \
	    __lkpi_done == NULL; __lkpi_done = (void *)1) \
		if (__guard_ptr(_name)(&scope) == NULL) { _fail; } else

#define	__DEFINE_LOCK_GUARD_TYPE(_name, _type, extra...) \
typedef struct { _type *lock; extra; } class_##_name##_t; \
static inline void class_##_name##_destructor(class_##_name##_t *_T) \
	{ (void)_T; } \
static inline void *class_##_name##_lock_ptr(class_##_name##_t *_T) \
	{ return ((void *)_T->lock); }
#define	DEFINE_LOCK_GUARD_1(_name, _type, _lock, _unlock, extra...) \
typedef struct { _type *lock; extra; } class_##_name##_t; \
static inline void class_##_name##_destructor(class_##_name##_t *_T) \
	{ if (_T->lock) { _unlock; } } \
static inline void *class_##_name##_lock_ptr(class_##_name##_t *_T) \
	{ return ((void *)_T->lock); } \
static inline class_##_name##_t class_##_name##_constructor(_type *l) \
	{ class_##_name##_t _t = { .lock = l }, *_T = &_t; _lock; return (_t); }
#define	DEFINE_LOCK_GUARD_0(_name, _lock, _unlock, extra...) \
typedef struct { void *lock; extra; } class_##_name##_t; \
static inline void class_##_name##_destructor(class_##_name##_t *_T) \
	{ if (_T->lock) { _unlock; } } \
static inline void *class_##_name##_lock_ptr(class_##_name##_t *_T) \
	{ return ((void *)_T->lock); } \
static inline class_##_name##_t class_##_name##_constructor(void) \
	{ class_##_name##_t _t = { .lock = (void *)1 }, *_T = &_t; \
	(void)_T; _lock; return (_t); }
#define	DEFINE_LOCK_GUARD_1_COND(_name, _ext, _condlock, ...) \
typedef class_##_name##_t class_##_name##_ext##_t; \
static inline void class_##_name##_ext##_destructor(class_##_name##_t *_T) \
	{ class_##_name##_destructor(_T); } \
static inline void *class_##_name##_ext##_lock_ptr(class_##_name##_t *_T) \
	{ return (class_##_name##_lock_ptr(_T)); } \
static inline class_##_name##_t class_##_name##_ext##_constructor(typeof (((class_##_name##_t *)0)->lock) l) \
	{ class_##_name##_t _t = { .lock = l }, *_T = &_t; \
	if (_T->lock && !(_condlock)) _T->lock = NULL; return (_t); }

#endif
