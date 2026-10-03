/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's completions (include/linux/completion.h, GPL-2.0;
 * reimplemented in src/lkpi_wait.c over the layer's wait queues).
 */
#ifndef _LINUX_COMPLETION_H
#define	_LINUX_COMPLETION_H

#include <linux/wait.h>

struct completion {
	unsigned int done;
	struct wait_queue_head wait;
};

#define	COMPLETION_INITIALIZER(work) \
	{ 0, __WAIT_QUEUE_HEAD_INITIALIZER((work).wait) }
#define	DECLARE_COMPLETION(work) \
	struct completion work = COMPLETION_INITIALIZER(work)
#define	DECLARE_COMPLETION_ONSTACK(work)	DECLARE_COMPLETION(work)
#define	COMPLETION_INITIALIZER_ONSTACK(work)	COMPLETION_INITIALIZER(work)

static inline void
init_completion(struct completion *x)
{
	x->done = 0;
	init_waitqueue_head(&x->wait);
}

static inline void
reinit_completion(struct completion *x)
{
	x->done = 0;
}

extern void complete(struct completion *x);
extern void complete_all(struct completion *x);
extern void wait_for_completion(struct completion *x);
extern unsigned long wait_for_completion_timeout(struct completion *x,
    unsigned long timeout);
extern int wait_for_completion_interruptible(struct completion *x);
extern long wait_for_completion_interruptible_timeout(struct completion *x,
    unsigned long timeout);
extern int wait_for_completion_killable(struct completion *x);
extern long wait_for_completion_killable_timeout(struct completion *x,
    unsigned long timeout);
extern bool try_wait_for_completion(struct completion *x);
extern bool completion_done(struct completion *x);
#define	wait_for_completion_io(x)	wait_for_completion(x)
#define	wait_for_completion_io_timeout(x, t)	wait_for_completion_timeout(x, t)
#define	complete_release(x)		complete(x)

#endif
