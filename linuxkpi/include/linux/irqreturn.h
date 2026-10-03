/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's interrupt handler results. */
#ifndef _LINUX_IRQRETURN_H
#define	_LINUX_IRQRETURN_H
enum irqreturn {
	IRQ_NONE = 0,
	IRQ_HANDLED = 1,
	IRQ_WAKE_THREAD = 2
};
typedef enum irqreturn irqreturn_t;
#define	IRQ_RETVAL(x)	((x) ? IRQ_HANDLED : IRQ_NONE)
#endif
