/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's read-write semaphores over illumos krwlock_t (include/linux/
 * rwsem.h, GPL-2.0; reimplemented). As with mutexes, the _interruptible
 * and _killable forms wait uninterruptibly and return 0. A zero-filled
 * krwlock_t is a valid unlocked lock.
 */
#ifndef _LINUX_RWSEM_H
#define	_LINUX_RWSEM_H

#include <linux/types.h>
#include <linux/lockdep.h>
#include <linux/cleanup.h>

struct rw_semaphore {
	krwlock_t rw;
};

#define	__RWSEM_INITIALIZER(name)	{ }
#define	DECLARE_RWSEM(name)		struct rw_semaphore name = { }
#define	init_rwsem(s)		rw_init(&(s)->rw, NULL, RW_DRIVER, NULL)
#define	down_read(s)		rw_enter(&(s)->rw, RW_READER)
#define	down_read_nested(s, n)	rw_enter(&(s)->rw, RW_READER)
#define	down_read_non_owner(s)	rw_enter(&(s)->rw, RW_READER)
#define	down_read_interruptible(s)	({ rw_enter(&(s)->rw, RW_READER); 0; })
#define	down_read_killable(s)	({ rw_enter(&(s)->rw, RW_READER); 0; })
#define	down_read_trylock(s)	(rw_tryenter(&(s)->rw, RW_READER) != 0)
#define	up_read(s)		rw_exit(&(s)->rw)
#define	up_read_non_owner(s)	rw_exit(&(s)->rw)
#define	down_write(s)		rw_enter(&(s)->rw, RW_WRITER)
#define	down_write_nested(s, n)	rw_enter(&(s)->rw, RW_WRITER)
#define	down_write_killable(s)	({ rw_enter(&(s)->rw, RW_WRITER); 0; })
#define	down_write_trylock(s)	(rw_tryenter(&(s)->rw, RW_WRITER) != 0)
#define	up_write(s)		rw_exit(&(s)->rw)
#define	downgrade_write(s)	rw_downgrade(&(s)->rw)
#define	rwsem_is_locked(s)	(rw_lock_held(&(s)->rw) != 0)
#define	rwsem_is_contended(s)	0
#define	rwsem_assert_held(s)	((void)(s))
#define	rwsem_assert_held_write(s)	ASSERT(RW_WRITE_HELD(&(s)->rw))

DEFINE_GUARD(rwsem_read, struct rw_semaphore *, down_read(_T), up_read(_T))
DEFINE_GUARD(rwsem_write, struct rw_semaphore *, down_write(_T), up_write(_T))

#endif
