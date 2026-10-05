/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * ioctl command encoding. DRM uAPI headers build their commands with
 * _IOW()/_IOR()/_IOWR(), which must be illumos' (<sys/ioccom.h>, included
 * by the prologue): the DRM core and libdrm on illumos use that encoding.
 * The _IOC_*() accessors decode it.
 */
#ifndef _ASM_IOCTL_H
#define	_ASM_IOCTL_H
#define	_IOC_NR(cmd)		((cmd) & 0xff)
#define	_IOC_TYPE(cmd)		(((cmd) >> 8) & 0xff)
#define	_IOC_SIZE(cmd)		(((cmd) >> 16) & IOCPARM_MASK)
#define	_IOC_DIR(cmd)		((cmd) & (IOC_VOID | IOC_IN | IOC_OUT))
#define	_IOC_NONE		IOC_VOID
#define	_IOC_READ		IOC_OUT
#define	_IOC_WRITE		IOC_IN
#endif
