/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's Intel On-chip System Fabric mailbox (Valleyview/Cherryview
 * sideband). Not supported: illumos has no IOSF driver. iosf_mbi_available()
 * is false; the read/write functions fail with -ENODEV; the PMIC bus
 * lock calls do nothing. Gen7 Ivy Bridge and gen9.5 Coffee Lake do not
 * use the sideband.
 */
#ifndef _ASM_IOSF_MBI_H
#define	_ASM_IOSF_MBI_H
#include <linux/types.h>
#include <linux/notifier.h>
#define	MBI_PMIC_BUS_ACCESS_BEGIN	1
#define	MBI_PMIC_BUS_ACCESS_END		2
#define	iosf_mbi_available()		false
#define	iosf_mbi_read(p, o, a, d)	(-ENODEV)
#define	iosf_mbi_write(p, o, a, d)	(-ENODEV)
#define	iosf_mbi_modify(p, o, a, d, m)	(-ENODEV)
#define	iosf_mbi_punit_acquire()	((void)0)
#define	iosf_mbi_punit_release()	((void)0)
#define	iosf_mbi_assert_punit_acquired()	((void)0)
#define	iosf_mbi_register_pmic_bus_access_notifier(nb)	0
#define	iosf_mbi_unregister_pmic_bus_access_notifier_unlocked(nb)	0
#define	iosf_mbi_unregister_pmic_bus_access_notifier(nb)	0
#endif
