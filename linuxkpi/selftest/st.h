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

#include <linux/completion.h>

struct pci_dev;
struct task_struct;

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

/*
 * st_sched.c's helpers, shared with st_kthread.c and st_ww.c:
 * st_wait() waits for a completion at most `ms` (true if it completed);
 * st_hold_sleep() sleeps while *flag is set, at most 2 s; st_call_start()
 * runs fn(arg) on a helper kthread whose `done` completes when fn
 * returned, and st_call_finish() waits for that at most `ms` and reaps
 * the kthread only if it returned (a helper left behind must only touch
 * static state).
 */
struct st_call {
	void (*fn)(void *);
	void *arg;
	struct completion done;
	struct task_struct *k;
};
extern bool st_wait(struct completion *c, unsigned int ms);
extern void st_hold_sleep(volatile int *flag);
extern bool st_call_start(struct st_call *c, void (*fn)(void *), void *arg);
extern bool st_call_finish(struct st_call *c, unsigned int ms);

/* st_kthread.c: kthread workers (ordering, flush, cancel, destroy). */
extern void st_kthread_tests(void);
/*
 * st_ww.c: stand-alone wound/wait mutexes, alone and in transactions
 * with reservation locks.
 */
extern void st_ww_tests(void);
/* st_mem.c: shmem, vmap, DMA, slab and page index under contention. */
extern void st_mem_tests(struct pci_dev *pdev);
/* st_resv.c: reservations, ww acquire contexts, fence arrays. */
extern void st_resv_tests(void);
/* st_i2c.c: I2C core and bit-banging algorithm on a simulated bus. */
extern void st_i2c_tests(struct pci_dev *pdev);

#endif /* _LKPI_ST_H */
