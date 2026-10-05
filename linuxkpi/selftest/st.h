/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Shared by the files of the linuxkpi_selftest driver: the check macro and
 * counters (linuxkpi_selftest.c) and the test entry points of each file,
 * run in order from probe(), in thread context.
 */
#ifndef _LKPI_ST_H
#define	_LKPI_ST_H

#include <linux/kernel.h>
#include <linux/printk.h>

struct pci_dev;

extern int st_passed, st_failed;

#define	CHECK(name, cond) do { \
	if (cond) { \
		st_passed++; \
		pr_info("linuxkpi_selftest: ok %s\n", name); \
	} else { \
		st_failed++; \
		pr_err("linuxkpi_selftest: FAIL %s (%s:%d)\n", name, \
		    __FILE__, __LINE__); \
	} \
	} while (0)

/* st_sched.c: workqueues, timers, RCU/SRCU, wait queues under contention. */
extern void st_sched_tests(void);
/* st_mem.c: shmem, vmap, DMA, slab and page index under contention. */
extern void st_mem_tests(struct pci_dev *pdev);
/* st_resv.c: reservations, ww acquire contexts, fence arrays. */
extern void st_resv_tests(void);

#endif /* _LKPI_ST_H */
