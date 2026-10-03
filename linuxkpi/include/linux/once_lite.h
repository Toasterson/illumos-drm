/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's DO_ONCE_LITE(): run a statement once per call site. */
#ifndef _LINUX_ONCE_LITE_H
#define	_LINUX_ONCE_LITE_H
#define	DO_ONCE_LITE(func, ...)	DO_ONCE_LITE_IF(true, func, ##__VA_ARGS__)
#define	DO_ONCE_LITE_IF(condition, func, ...) ({ \
	static volatile uint32_t __already_done; \
	bool __ret_do_once = !!(condition); \
	if (__ret_do_once && __already_done == 0 && \
	    __atomic_exchange_n(&__already_done, 1, __ATOMIC_SEQ_CST) == 0) \
		func(__VA_ARGS__); \
	__ret_do_once; })
#endif
