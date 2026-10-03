/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's sequence counters and sequential locks (include/linux/
 * seqlock.h, GPL-2.0; reimplemented). A writer makes the count odd while
 * it writes; a reader retries when the count was odd or changed. Every
 * seqcount variant starts with its seqcount_t, so the accessors take any
 * of them. Writers must be serialised by their own lock, as on Linux.
 */
#ifndef _LINUX_SEQLOCK_H
#define	_LINUX_SEQLOCK_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <asm/barrier.h>

typedef struct seqcount {
	unsigned int sequence;
} seqcount_t;

typedef struct {
	seqcount_t seqcount;
	spinlock_t *lock;
} seqcount_spinlock_t;

typedef struct {
	seqcount_t seqcount;
	raw_spinlock_t *lock;
} seqcount_raw_spinlock_t;

typedef struct {
	seqcount_t seqcount;
	struct mutex *lock;
} seqcount_mutex_t;

typedef struct {
	seqcount_t seqcount;
	rwlock_t *lock;
} seqcount_rwlock_t;

typedef struct {
	seqcount_t seqcount;
	void *lock;
} seqcount_latch_t;

typedef struct {
	seqcount_spinlock_t seqcount;
	spinlock_t lock;
} seqlock_t;

#define	__lkpi_seq(s)	((seqcount_t *)(void *)(s))
#define	SEQCNT_ZERO(name)	{ .sequence = 0 }
#define	SEQCNT_SPINLOCK_ZERO(name, l)	{ .seqcount = SEQCNT_ZERO(name), .lock = (l) }
#define	SEQCNT_MUTEX_ZERO(name, l)	{ .seqcount = SEQCNT_ZERO(name), .lock = (l) }
#define	seqcount_init(s)		(__lkpi_seq(s)->sequence = 0)
#define	seqcount_spinlock_init(s, l)	do { seqcount_init(s); (s)->lock = (l); } while (0)
#define	seqcount_raw_spinlock_init(s, l)	seqcount_spinlock_init(s, l)
#define	seqcount_mutex_init(s, l)	seqcount_spinlock_init(s, l)
#define	seqcount_rwlock_init(s, l)	seqcount_spinlock_init(s, l)
#define	seqcount_latch_init(s)		seqcount_init(s)

static inline unsigned int
lkpi_read_seqcount_begin(const seqcount_t *s)
{
	unsigned int seq;

	while ((seq = READ_ONCE(s->sequence)) & 1)
		cpu_relax();
	smp_rmb();
	return (seq);
}

static inline unsigned int
lkpi_raw_read_seqcount(const seqcount_t *s)
{
	unsigned int seq = READ_ONCE(s->sequence);

	smp_rmb();
	return (seq);
}

static inline int
lkpi_read_seqcount_retry(const seqcount_t *s, unsigned int start)
{
	smp_rmb();
	return (READ_ONCE(s->sequence) != start);
}

static inline void
lkpi_write_seqcount_begin(seqcount_t *s)
{
	WRITE_ONCE(s->sequence, s->sequence + 1);
	smp_wmb();
}

static inline void
lkpi_write_seqcount_end(seqcount_t *s)
{
	smp_wmb();
	WRITE_ONCE(s->sequence, s->sequence + 1);
}

#define	read_seqcount_begin(s)		lkpi_read_seqcount_begin(__lkpi_seq(s))
#define	raw_read_seqcount_begin(s)	lkpi_read_seqcount_begin(__lkpi_seq(s))
#define	__read_seqcount_begin(s)	lkpi_read_seqcount_begin(__lkpi_seq(s))
#define	raw_read_seqcount(s)		lkpi_raw_read_seqcount(__lkpi_seq(s))
#define	read_seqcount_retry(s, start)	lkpi_read_seqcount_retry(__lkpi_seq(s), start)
#define	__read_seqcount_retry(s, start)	lkpi_read_seqcount_retry(__lkpi_seq(s), start)
#define	raw_read_seqcount_retry(s, st)	lkpi_read_seqcount_retry(__lkpi_seq(s), st)
#define	write_seqcount_begin(s)		lkpi_write_seqcount_begin(__lkpi_seq(s))
#define	raw_write_seqcount_begin(s)	lkpi_write_seqcount_begin(__lkpi_seq(s))
#define	write_seqcount_begin_nested(s, n)	lkpi_write_seqcount_begin(__lkpi_seq(s))
#define	write_seqcount_end(s)		lkpi_write_seqcount_end(__lkpi_seq(s))
#define	raw_write_seqcount_end(s)	lkpi_write_seqcount_end(__lkpi_seq(s))
#define	write_seqcount_invalidate(s) \
	do { smp_wmb(); __lkpi_seq(s)->sequence += 2; } while (0)
#define	raw_write_seqcount_barrier(s)	write_seqcount_invalidate(s)
#define	raw_read_seqcount_latch(s)	lkpi_raw_read_seqcount(__lkpi_seq(s))
#define	raw_read_seqcount_latch_retry(s, st)	lkpi_read_seqcount_retry(__lkpi_seq(s), st)
#define	raw_write_seqcount_latch(s) \
	do { smp_wmb(); __lkpi_seq(s)->sequence++; smp_wmb(); } while (0)

#define	seqlock_init(sl) \
	do { seqcount_init(&(sl)->seqcount); spin_lock_init(&(sl)->lock); } while (0)
#define	DEFINE_SEQLOCK(name)	seqlock_t name = { }
#define	read_seqbegin(sl)	read_seqcount_begin(&(sl)->seqcount)
#define	read_seqretry(sl, s)	read_seqcount_retry(&(sl)->seqcount, s)
#define	write_seqlock(sl) \
	do { spin_lock(&(sl)->lock); write_seqcount_begin(&(sl)->seqcount); } while (0)
#define	write_sequnlock(sl) \
	do { write_seqcount_end(&(sl)->seqcount); spin_unlock(&(sl)->lock); } while (0)
#define	write_seqlock_irq(sl)	write_seqlock(sl)
#define	write_sequnlock_irq(sl)	write_sequnlock(sl)
#define	write_seqlock_irqsave(sl, f) \
	do { (f) = 0; write_seqlock(sl); } while (0)
#define	write_sequnlock_irqrestore(sl, f) \
	do { (void)(f); write_sequnlock(sl); } while (0)
#define	read_seqlock_excl(sl)	spin_lock(&(sl)->lock)
#define	read_sequnlock_excl(sl)	spin_unlock(&(sl)->lock)

#endif
