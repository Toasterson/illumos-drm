/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Host (macOS/Linux user space) stand-ins for the illumos kernel
 * interfaces the pure-logic parts of the LinuxKPI layer use, so that the
 * real src/ files build and run in unit tests: kmutex_t and krwlock_t
 * over pthreads (a zero-filled one is valid, as in illumos), kmem_*()
 * over malloc(), cmn_err() and panic() to stderr (the last message is
 * kept for the tests), gethrtime(), the illumos base types and the
 * atomic_*() subset of <sys/atomic.h>. Every <sys/...> header the layer
 * includes that the host lacks is a one-line file including this one.
 */
#ifndef _LKPI_HOST_H
#define	_LKPI_HOST_H

/* The host's <strings.h> declares fls()/ffs(), which the layer defines. */
#define	fls	lkpi_host_libc_fls
#define	ffs	lkpi_host_libc_ffs
#define	flsl	lkpi_host_libc_flsl
#define	flsll	lkpi_host_libc_flsll
#define	ffsl	lkpi_host_libc_ffsl
#define	ffsll	lkpi_host_libc_ffsll
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#undef	fls
#undef	ffs
#undef	flsl
#undef	flsll
#undef	ffsl
#undef	ffsll

/* --- base types ----------------------------------------------------------- */

typedef unsigned char	uchar_t;
typedef unsigned short	ushort_t;
typedef unsigned int	uint_t;
typedef unsigned long	ulong_t;
typedef long long	longlong_t;
typedef unsigned long long	u_longlong_t;
typedef long long	hrtime_t;
typedef int		processorid_t;
typedef int		zoneid_t;
typedef unsigned int	major_t;
typedef unsigned int	minor_t;
typedef enum { B_FALSE = 0, B_TRUE = 1 } boolean_t;
typedef struct dev_info	dev_info_t;
typedef struct lkpi_host_cred	cred_t;
typedef void		*ddi_acc_handle_t;
typedef void		*ddi_dma_handle_t;
typedef struct { int dummy; } ddi_dma_attr_t;
typedef struct { int dummy; } ddi_device_acc_attr_t;
typedef va_list		__va_list;

#define	ASSERT(x)	do { if (!(x)) lkpi_host_assfail(#x, __FILE__, \
	__LINE__); } while (0)
#define	ASSERT3U(a, op, b)	ASSERT((a) op (b))
#define	ASSERT3S(a, op, b)	ASSERT((a) op (b))
#define	ASSERT3P(a, op, b)	ASSERT((a) op (b))
#define	VERIFY(x)	ASSERT(x)
#define	VERIFY0(x)	ASSERT((x) == 0)
#define	IMPLY(a, b)	ASSERT(!(a) || (b))

extern void lkpi_host_assfail(const char *, const char *, int)
    __attribute__((__noreturn__));

/* --- cmn_err(), panic() --------------------------------------------------- */

#define	CE_CONT		0
#define	CE_NOTE		1
#define	CE_WARN		2
#define	CE_PANIC	3
#define	CE_IGNORE	4

/* The last cmn_err() message (level, text) and the number of messages. */
extern int lkpi_host_cmn_level;
extern char lkpi_host_cmn_msg[1024];
extern unsigned long lkpi_host_cmn_count;
/* Non-zero: cmn_err() does not print (tests that expect warnings). */
extern int lkpi_host_cmn_quiet;

extern void cmn_err(int, const char *, ...)
    __attribute__((__format__(__printf__, 2, 3)));
extern void vcmn_err(int, const char *, va_list);
extern void panic(const char *, ...) __attribute__((__noreturn__));

/* --- memory ---------------------------------------------------------------- */

#define	KM_SLEEP	0x0000
#define	KM_NOSLEEP	0x0001
#define	KM_PANIC	0x0002
#define	KM_PUSHPAGE	0x0004
#define	KM_NORMALPRI	0x0008

/* Set to make the next KM_NOSLEEP allocations fail (tests of -ENOMEM). */
extern int lkpi_host_kmem_fail_nosleep;
extern unsigned long lkpi_host_kmem_live;

extern void *kmem_alloc(size_t, int);
extern void *kmem_zalloc(size_t, int);
extern void kmem_free(void *, size_t);

/* --- locks ----------------------------------------------------------------- */

typedef enum { MUTEX_ADAPTIVE = 0, MUTEX_SPIN = 1, MUTEX_DRIVER = 4,
	MUTEX_DEFAULT = 6 } kmutex_type_t;
typedef enum { RW_DRIVER = 2, RW_DEFAULT = 4 } krw_type_t;
typedef enum { RW_WRITER, RW_READER, RW_READER_STARVEWRITER } krw_t;

typedef struct mutex {
	pthread_mutex_t	hm_mutex;
	pthread_t	hm_owner;
	volatile int	hm_init;
	volatile int	hm_held;
} kmutex_t;

typedef struct krwlock {
	pthread_rwlock_t hr_lock;
	volatile int	hr_init;
	volatile int	hr_writer;
} krwlock_t;

typedef struct kcondvar {
	pthread_cond_t	hc_cv;
	volatile int	hc_init;
} kcondvar_t;

extern void mutex_init(kmutex_t *, char *, kmutex_type_t, void *);
extern void mutex_destroy(kmutex_t *);
extern void mutex_enter(kmutex_t *);
extern void mutex_exit(kmutex_t *);
extern int mutex_tryenter(kmutex_t *);
extern int mutex_owned(const kmutex_t *);
#define	MUTEX_HELD(m)		mutex_owned(m)
#define	MUTEX_NOT_HELD(m)	(!mutex_owned(m))

extern void rw_init(krwlock_t *, char *, krw_type_t, void *);
extern void rw_destroy(krwlock_t *);
extern void rw_enter(krwlock_t *, krw_t);
extern void rw_exit(krwlock_t *);
extern int rw_tryenter(krwlock_t *, krw_t);
#define	RW_WRITE_HELD(l)	((l)->hr_writer != 0)
#define	RW_LOCK_HELD(l)		1

/* --- time, CPU ------------------------------------------------------------- */

#define	NANOSEC		1000000000LL
#define	MICROSEC	1000000LL
#define	MILLISEC	1000LL

typedef struct _kthread kthread_t;
typedef struct cpu cpu_t;

extern hrtime_t gethrtime(void);
extern clock_t drv_usectohz(clock_t);
extern clock_t drv_hztousec(clock_t);
#define	kpreempt_disable()	((void)0)
#define	kpreempt_enable()	((void)0)
extern void drv_usecwait(clock_t);
extern int servicing_interrupt(void);

/* --- <sys/atomic.h> -------------------------------------------------------- */

#define	atomic_inc_32(p)	((void)__atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST))
#define	atomic_dec_32(p)	((void)__atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST))
#define	atomic_inc_32_nv(p)	__atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define	atomic_dec_32_nv(p)	__atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define	atomic_add_32(p, v)	((void)__atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST))
#define	atomic_add_32_nv(p, v)	__atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define	atomic_inc_64(p)	((void)__atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST))
#define	atomic_dec_64(p)	((void)__atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST))
#define	atomic_inc_64_nv(p)	__atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define	atomic_dec_64_nv(p)	__atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define	atomic_add_64(p, v)	((void)__atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST))
#define	atomic_add_64_nv(p, v)	__atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define	atomic_or_32(p, v)	((void)__atomic_or_fetch((p), (v), __ATOMIC_SEQ_CST))
#define	atomic_and_32(p, v)	((void)__atomic_and_fetch((p), (v), __ATOMIC_SEQ_CST))
#define	atomic_or_64(p, v)	((void)__atomic_or_fetch((p), (v), __ATOMIC_SEQ_CST))
#define	atomic_and_64(p, v)	((void)__atomic_and_fetch((p), (v), __ATOMIC_SEQ_CST))
#define	atomic_swap_32(p, v)	__atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define	atomic_swap_64(p, v)	__atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define	atomic_swap_ptr(p, v)	__atomic_exchange_n((void **)(p), (v), __ATOMIC_SEQ_CST)
#define	membar_producer()	__atomic_thread_fence(__ATOMIC_RELEASE)
#define	membar_consumer()	__atomic_thread_fence(__ATOMIC_ACQUIRE)
#define	membar_enter()		__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define	membar_exit()		__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define	membar_sync()		__atomic_thread_fence(__ATOMIC_SEQ_CST)

static inline uint32_t
atomic_cas_32(volatile uint32_t *p, uint32_t cmp, uint32_t nv)
{
	(void) __atomic_compare_exchange_n(p, &cmp, nv, false,
	    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return (cmp);
}

static inline uint64_t
atomic_cas_64(volatile uint64_t *p, uint64_t cmp, uint64_t nv)
{
	(void) __atomic_compare_exchange_n(p, &cmp, nv, false,
	    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return (cmp);
}

static inline void *
atomic_cas_ptr(volatile void *p, void *cmp, void *nv)
{
	(void) __atomic_compare_exchange_n((void *volatile *)p, &cmp, nv,
	    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return (cmp);
}

/* --- misc ------------------------------------------------------------------ */

extern int random_get_pseudo_bytes(uint8_t *, size_t);
extern int random_get_bytes(uint8_t *, size_t);

#ifndef	MAXNAMELEN
#define	MAXNAMELEN	256
#endif

#endif	/* _LKPI_HOST_H */
