/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's _RET_IP_ (see kernel.h). */
#ifndef _LINUX_INSTRUCTION_POINTER_H
#define	_LINUX_INSTRUCTION_POINTER_H
#ifndef	_RET_IP_
#define	_RET_IP_	((unsigned long)__builtin_return_address(0))
#endif
#endif
