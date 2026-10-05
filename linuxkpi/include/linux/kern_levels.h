/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's KERN_* levels (the facade's drm_print.h has the same values). */
#ifndef _LINUX_KERN_LEVELS_H
#define	_LINUX_KERN_LEVELS_H
#ifndef	KERN_SOH
#define	KERN_SOH	"\001"
#define	KERN_EMERG	KERN_SOH "0"
#define	KERN_ALERT	KERN_SOH "1"
#define	KERN_CRIT	KERN_SOH "2"
#define	KERN_ERR	KERN_SOH "3"
#define	KERN_WARNING	KERN_SOH "4"
#define	KERN_NOTICE	KERN_SOH "5"
#define	KERN_INFO	KERN_SOH "6"
#define	KERN_DEBUG	KERN_SOH "7"
#endif
#define	KERN_SOH_ASCII	'\001'
#define	KERN_DEFAULT	""
#define	KERN_CONT	KERN_SOH "c"
#define	LOGLEVEL_ERR	3
#define	LOGLEVEL_WARNING	4
#define	LOGLEVEL_INFO	6
#define	LOGLEVEL_DEBUG	7
#endif
