/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's IRQ domain/chip interface: not supported (no child interrupt controllers). */
#ifndef _LINUX_IRQ_H
#define	_LINUX_IRQ_H
#include <linux/interrupt.h>
struct irq_data;
struct irq_chip;
struct irq_domain;
#define	irq_set_status_flags(irq, f)	((void)(irq))
#define	irq_clear_status_flags(irq, f)	((void)(irq))
#define	IRQ_NOAUTOEN			0
#endif
