/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's local interrupt masking. Not supported: the layer's spinlocks
 * need no masking (see <linux/spinlock.h>), so these are no-ops and
 * irqs_disabled() is false. Code that masks interrupts for timing (a
 * busy wait that must not be interrupted) runs with interrupts enabled.
 */
#ifndef _LINUX_IRQFLAGS_H
#define	_LINUX_IRQFLAGS_H
#define	local_irq_disable()		((void)0)
#define	local_irq_enable()		((void)0)
#define	local_irq_save(flags)		do { (flags) = 0; } while (0)
#define	local_irq_restore(flags)	do { (void)(flags); } while (0)
#define	raw_local_irq_save(flags)	local_irq_save(flags)
#define	raw_local_irq_restore(flags)	local_irq_restore(flags)
#define	irqs_disabled()			0
#define	irqs_disabled_flags(flags)	((void)(flags), 0)
#define	local_save_flags(flags)		do { (flags) = 0; } while (0)
#define	hard_irq_disable()		((void)0)
#endif
