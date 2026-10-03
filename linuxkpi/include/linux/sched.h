/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's task model, reduced to what drivers use (include/linux/sched.h,
 * GPL-2.0; reimplemented in src/lkpi_sched.c).
 *
 * A struct task_struct exists for each illumos thread that uses
 * `current` in thread context: created on first use, kept in thread-
 * specific data (tsd_create/tsd_set), freed when the thread exits. In an
 * interrupt handler `current` is a shared placeholder that cannot sleep.
 * The task holds the sleep state of the set_current_state() /
 * schedule_timeout() / wake_up_process() protocol, a mutex and a
 * condition variable. TASK_INTERRUPTIBLE sleeps end early on a signal
 * (cv_reltimedwait_sig(9F)); signal_pending() then reports it until the
 * task's next interruptible sleep. Other task fields are fixed: comm is
 * "illumos", mm is NULL (no Linux address spaces), flags 0.
 */
#ifndef _LINUX_SCHED_H
#define	_LINUX_SCHED_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/preempt.h>
#include <linux/smp.h>
#include <linux/sched/clock.h>

#define	TASK_RUNNING		0x00000000
#define	TASK_INTERRUPTIBLE	0x00000001
#define	TASK_UNINTERRUPTIBLE	0x00000002
#define	__TASK_STOPPED		0x00000004
#define	__TASK_TRACED		0x00000008
#define	TASK_PARKED		0x00000040
#define	TASK_DEAD		0x00000080
#define	TASK_WAKEKILL		0x00000100
#define	TASK_WAKING		0x00000200
#define	TASK_NOLOAD		0x00000400
#define	TASK_NEW		0x00000800
#define	TASK_KILLABLE		(TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)
#define	TASK_IDLE		(TASK_UNINTERRUPTIBLE | TASK_NOLOAD)
#define	TASK_NORMAL		(TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)
#define	TASK_COMM_LEN		16
#define	PF_KTHREAD		0x00200000
#define	PF_MEMALLOC		0x00000800
#define	PF_EXITING		0x00000004
#define	MAX_RT_PRIO		100
#define	MAX_PRIO		140
#define	DEFAULT_PRIO		120

struct mm_struct;

struct task_struct {
	kthread_t *thread;
	pid_t pid;
	int prio;
	unsigned int flags;
	volatile long __state;
	volatile int sigpending;
	struct mm_struct *mm;
	char comm[TASK_COMM_LEN];
	kmutex_t lock;
	kcondvar_t cv;
	void *kthread_data;
};

extern struct task_struct *lkpi_current(void);
#define	current		lkpi_current()
#define	get_current()	lkpi_current()

extern void lkpi_set_current_state(long state);
extern long schedule_timeout(long timeout);
extern int wake_up_process(struct task_struct *t);
extern int wake_up_state(struct task_struct *t, unsigned int state);

#define	set_current_state(s)		lkpi_set_current_state(s)
#define	__set_current_state(s)		lkpi_set_current_state(s)
#define	set_special_state(s)		lkpi_set_current_state(s)
#define	task_is_running(t)		(READ_ONCE((t)->__state) == TASK_RUNNING)
#define	schedule()		((void)schedule_timeout(MAX_SCHEDULE_TIMEOUT))
#define	io_schedule()		schedule()
#define	io_schedule_timeout(t)	schedule_timeout(t)
#define	schedule_timeout_interruptible(t) \
	({ set_current_state(TASK_INTERRUPTIBLE); schedule_timeout(t); })
#define	schedule_timeout_uninterruptible(t) \
	({ set_current_state(TASK_UNINTERRUPTIBLE); schedule_timeout(t); })
#define	schedule_timeout_killable(t) \
	({ set_current_state(TASK_KILLABLE); schedule_timeout(t); })
#define	schedule_timeout_idle(t)	schedule_timeout_uninterruptible(t)
#define	schedule_hrtimeout(kt, mode) \
	schedule_timeout(nsecs_to_jiffies(*(kt)) + 1)

extern void lkpi_cond_resched(void);
#define	cond_resched()			({ lkpi_cond_resched(); 0; })
#define	cond_resched_lock(l)		0
#define	need_resched()			0
#define	yield()				lkpi_cond_resched()
#define	cpu_relax_yield()		lkpi_cond_resched()

static inline int
signal_pending(struct task_struct *t)
{
	return (t->sigpending);
}

static inline int
fatal_signal_pending(struct task_struct *t)
{
	return (t->sigpending);
}

static inline int
signal_pending_state(unsigned int state, struct task_struct *t)
{
	if ((state & (TASK_INTERRUPTIBLE | TASK_WAKEKILL)) == 0)
		return (0);
	return (signal_pending(t));
}

static inline pid_t
task_pid_nr(struct task_struct *t)
{
	return (t->pid);
}

#define	task_pid_vnr(t)		task_pid_nr(t)
#define	task_tgid_nr(t)		task_pid_nr(t)
#define	task_tgid_vnr(t)	task_pid_nr(t)
#define	task_pid(t)		((struct pid *)(uintptr_t)(t)->pid)
#define	get_task_comm(buf, t)	strscpy(buf, (t)->comm, sizeof (buf))
#define	__get_task_comm(buf, n, t)	strscpy(buf, (t)->comm, n)
#define	set_user_nice(t, n)	((void)(t), (void)(n))
#define	task_nice(t)		0
#define	sched_setscheduler_nocheck(t, p, s)	0
#define	sched_set_fifo(t)	((void)(t))
#define	sched_set_fifo_low(t)	((void)(t))
#define	sched_set_normal(t, n)	((void)(t))
#define	get_task_struct(t)	(t)
#define	put_task_struct(t)	((void)(t))
#define	task_cpu(t)		0
#define	is_global_init(t)	0
#define	same_thread_group(a, b)	((a) == (b))

#endif
