/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Interfaces between the parts of the LinuxKPI layer (not for drivers).
 * Every part with global state has an init and a fini function, called
 * by the module glue (src/lkpi_module.c) in this order at _init(9E) and
 * in reverse at _fini(9E); an init failure unwinds the parts before it.
 */
#ifndef _LKPI_INTERNAL_H
#define	_LKPI_INTERNAL_H

#include <linux/types.h>
#include <linux/pci.h>

/* src/lkpi_cpu.c: boot_cpu_data (CPUID). */
extern int lkpi_cpu_init(void);

/* src/lkpi_slab.c, src/lkpi_page.c: allocation, the page cache. */
extern int lkpi_slab_init(void);
extern void lkpi_slab_fini(void);
extern int lkpi_page_init(void);
extern void lkpi_page_fini(void);

/* src/lkpi_sched.c: tasks (tsd key), src/lkpi_rcu.c: RCU state. */
extern int lkpi_sched_init(void);
extern void lkpi_sched_fini(void);
extern int lkpi_rcu_init(void);
extern void lkpi_rcu_fini(void);

/*
 * Within the scheduling part (src/lkpi_sched.c, lkpi_wait.c,
 * lkpi_kthread.c): a task's fields for a thread named comm; binding a
 * task to the calling thread (`current` returns it from then on) and
 * unbinding it; the shared wait queues of wait_var_event(); stopping the
 * kthreads left at unload.
 */
struct task_struct;
extern void lkpi_task_init(struct task_struct *t, const char *comm);
extern void lkpi_task_fini(struct task_struct *t);
extern void lkpi_task_bind(struct task_struct *t);
extern void lkpi_task_unbind(void);
extern void lkpi_wait_init(void);
extern void lkpi_kthread_fini(void);

/*
 * illumos clock ticks for a sleep of j jiffies, at least one tick and at
 * most one day (longer sleeps wake up and sleep again), for
 * cv_reltimedwait(9F).
 */
extern clock_t lkpi_sleep_ticks(unsigned long j);

/* src/lkpi_timer.c: timers; src/lkpi_work.c: system workqueues, irq_work. */
extern int lkpi_timer_init(void);
extern void lkpi_timer_fini(void);
extern int lkpi_work_init(void);
extern void lkpi_work_fini(void);

/*
 * Waits until t's function is not running (without disarming t); returns
 * at once on the timer thread itself. Thread context.
 */
struct timer_list;
extern void lkpi_timer_wait_idle(struct timer_list *t);

/* Thread priority of the layer's high-priority threads (SYS class). */
#define	LKPI_PRI_HIGH	98

/* src/lkpi_shrinker.c: the kmem reclaim hook. */
extern int lkpi_shrinker_init(void);
extern void lkpi_shrinker_fini(void);

/* src/lkpi_drm.c: the DRM driver-table copies, the unplug SRCU. */
extern int lkpi_drm_init(void);
extern void lkpi_drm_fini(void);

/* src/lkpi_pci.c: attached PCI devices. */
extern int lkpi_pci_init(void);
extern void lkpi_pci_fini(void);

/*
 * src/lkpi_irq.c: the table of allocated interrupt vectors (irq numbers);
 * called by lkpi_pci_init()/lkpi_pci_fini().
 */
extern int lkpi_irq_init(void);
extern void lkpi_irq_fini(void);

/*
 * The BAR of an attached layer PCI device that contains the physical range
 * [phys, phys + size): its node, its "reg" index (for
 * ddi_regs_map_setup(9F)) and the offset of phys in it. 0 or ENXIO.
 */
extern int lkpi_pci_phys_lookup(resource_size_t phys, size_t size,
    dev_info_t **dipp, uint_t *regnump, off_t *offp);

/*
 * The DMA attributes of a layer device (dma_set_mask() changes the
 * address limit); NULL for a device without DMA.
 */
extern ddi_dma_attr_t *lkpi_dev_dma_attr(struct device *dev);

/* src/lkpi_device.c: devres of a device at detach. */
extern void lkpi_device_init(struct device *dev, dev_info_t *dip,
    const char *name);
extern void lkpi_device_fini(struct device *dev);

#endif
