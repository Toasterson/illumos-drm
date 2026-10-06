/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Host implementations of the illumos kernel interfaces declared in
 * include/lkpi_host.h, and of the layer functions other parts of the
 * layer provide in the kernel (lkpi_kmalloc() and friends). Compiled
 * without the layer's prologue: plain host C.
 */

#include <lkpi_host.h>

int lkpi_host_cmn_level = -1;
char lkpi_host_cmn_msg[1024];
unsigned long lkpi_host_cmn_count;
int lkpi_host_cmn_quiet;
int lkpi_host_kmem_fail_nosleep;
unsigned long lkpi_host_kmem_live;

void
lkpi_host_assfail(const char *expr, const char *file, int line)
{
	fprintf(stderr, "assertion failed: %s, file: %s, line: %d\n", expr,
	    file, line);
	abort();
}

void
vcmn_err(int level, const char *fmt, va_list ap)
{
	static const char *const pfx[] = { "", "NOTICE: ", "WARNING: ",
	    "PANIC: ", "" };

	(void) vsnprintf(lkpi_host_cmn_msg, sizeof (lkpi_host_cmn_msg), fmt,
	    ap);
	lkpi_host_cmn_level = level;
	lkpi_host_cmn_count++;
	if (!lkpi_host_cmn_quiet) {
		fprintf(stderr, "%s%s%s", level >= 0 && level <= 4 ?
		    pfx[level] : "", lkpi_host_cmn_msg,
		    level == CE_CONT ? "" : "\n");
	}
}

void
cmn_err(int level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vcmn_err(level, fmt, ap);
	va_end(ap);
}

void
panic(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "panic: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	abort();
}

/*
 * kmem: malloc() with a live count; KM_NOSLEEP may be made to fail. Each
 * allocation keeps its size in a 16-byte header, and kmem_free() aborts
 * when given another size, which illumos' kmem_free(9F) would not survive.
 */
#define	HOST_KMEM_HDR	16

void *
kmem_alloc(size_t size, int flag)
{
	char *p;

	if ((flag & KM_NOSLEEP) && lkpi_host_kmem_fail_nosleep)
		return (NULL);
	if (size == 0)
		return (NULL);
	p = malloc(size + HOST_KMEM_HDR);
	if (p == NULL)
		abort();
	memcpy(p, &size, sizeof (size));
	lkpi_host_kmem_live++;
	return (p + HOST_KMEM_HDR);
}

void *
kmem_zalloc(size_t size, int flag)
{
	void *p = kmem_alloc(size, flag);

	if (p != NULL)
		memset(p, 0, size);
	return (p);
}

void
kmem_free(void *p, size_t size)
{
	char *h;
	size_t had;

	if (p == NULL)
		return;
	h = (char *)p - HOST_KMEM_HDR;
	memcpy(&had, h, sizeof (had));
	if (had != size) {
		fprintf(stderr, "kmem_free(%p, %zu): allocated with %zu\n", p,
		    size, had);
		abort();
	}
	lkpi_host_kmem_live--;
	free(h);
}

/* The layer's kmalloc() (src/lkpi_slab.c in the kernel). */
#define	HOST_GFP_ZERO		0x100u
#define	HOST_GFP_RECLAIM	0x400u

void *
lkpi_kmalloc(size_t size, unsigned int gfp)
{
	void *p;

	if (!(gfp & HOST_GFP_RECLAIM) && lkpi_host_kmem_fail_nosleep)
		return (NULL);
	p = (gfp & HOST_GFP_ZERO) ? calloc(1, size ? size : 1) :
	    malloc(size ? size : 1);
	if (p == NULL)
		abort();
	return (p);
}

void
lkpi_kfree(const void *p)
{
	free((void *)p);
}

/* Locks: a zero-filled lock is initialised on first use, as in illumos. */
static pthread_mutex_t lkpi_host_init_lock = PTHREAD_MUTEX_INITIALIZER;

static void
host_mutex_lazy(kmutex_t *m)
{
	if (__atomic_load_n(&m->hm_init, __ATOMIC_ACQUIRE))
		return;
	pthread_mutex_lock(&lkpi_host_init_lock);
	if (!m->hm_init) {
		pthread_mutex_init(&m->hm_mutex, NULL);
		__atomic_store_n(&m->hm_init, 1, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&lkpi_host_init_lock);
}

void
mutex_init(kmutex_t *m, char *name, kmutex_type_t type, void *arg)
{
	memset(m, 0, sizeof (*m));
	host_mutex_lazy(m);
}

void
mutex_destroy(kmutex_t *m)
{
	if (m->hm_held)
		lkpi_host_assfail("mutex_destroy of a held mutex", __FILE__,
		    __LINE__);
	if (m->hm_init)
		pthread_mutex_destroy(&m->hm_mutex);
	m->hm_init = 0;
}

void
mutex_enter(kmutex_t *m)
{
	host_mutex_lazy(m);
	if (m->hm_held && pthread_equal(m->hm_owner, pthread_self()))
		lkpi_host_assfail("recursive mutex_enter", __FILE__, __LINE__);
	pthread_mutex_lock(&m->hm_mutex);
	m->hm_owner = pthread_self();
	m->hm_held = 1;
}

void
mutex_exit(kmutex_t *m)
{
	if (!m->hm_held || !pthread_equal(m->hm_owner, pthread_self()))
		lkpi_host_assfail("mutex_exit of a mutex not held", __FILE__,
		    __LINE__);
	m->hm_held = 0;
	pthread_mutex_unlock(&m->hm_mutex);
}

int
mutex_tryenter(kmutex_t *m)
{
	host_mutex_lazy(m);
	if (pthread_mutex_trylock(&m->hm_mutex) != 0)
		return (0);
	m->hm_owner = pthread_self();
	m->hm_held = 1;
	return (1);
}

int
mutex_owned(const kmutex_t *m)
{
	return (m->hm_held && pthread_equal(m->hm_owner, pthread_self()));
}

/* Condition variables over pthreads, keeping the mutex's owner record. */
__thread int lkpi_host_cv_signal_pending;

static void
host_cv_lazy(kcondvar_t *cv)
{
	if (__atomic_load_n(&cv->hc_init, __ATOMIC_ACQUIRE))
		return;
	pthread_mutex_lock(&lkpi_host_init_lock);
	if (!cv->hc_init) {
		pthread_cond_init(&cv->hc_cv, NULL);
		__atomic_store_n(&cv->hc_init, 1, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&lkpi_host_init_lock);
}

void
cv_init(kcondvar_t *cv, char *name, kcv_type_t type, void *arg)
{
	memset(cv, 0, sizeof (*cv));
	host_cv_lazy(cv);
}

void
cv_destroy(kcondvar_t *cv)
{
	if (cv->hc_init)
		pthread_cond_destroy(&cv->hc_cv);
	cv->hc_init = 0;
}

void
cv_wait(kcondvar_t *cv, kmutex_t *m)
{
	if (!mutex_owned(m))
		lkpi_host_assfail("cv_wait without the mutex", __FILE__,
		    __LINE__);
	host_cv_lazy(cv);
	m->hm_held = 0;
	pthread_cond_wait(&cv->hc_cv, &m->hm_mutex);
	m->hm_owner = pthread_self();
	m->hm_held = 1;
}

int
cv_wait_sig(kcondvar_t *cv, kmutex_t *m)
{
	if (lkpi_host_cv_signal_pending) {
		lkpi_host_cv_signal_pending = 0;
		return (0);
	}
	cv_wait(cv, m);
	return (1);
}

void
cv_signal(kcondvar_t *cv)
{
	host_cv_lazy(cv);
	pthread_cond_signal(&cv->hc_cv);
}

void
cv_broadcast(kcondvar_t *cv)
{
	host_cv_lazy(cv);
	pthread_cond_broadcast(&cv->hc_cv);
}

static void
host_rw_lazy(krwlock_t *l)
{
	if (__atomic_load_n(&l->hr_init, __ATOMIC_ACQUIRE))
		return;
	pthread_mutex_lock(&lkpi_host_init_lock);
	if (!l->hr_init) {
		pthread_rwlock_init(&l->hr_lock, NULL);
		__atomic_store_n(&l->hr_init, 1, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&lkpi_host_init_lock);
}

void
rw_init(krwlock_t *l, char *name, krw_type_t type, void *arg)
{
	memset(l, 0, sizeof (*l));
	host_rw_lazy(l);
}

void
rw_destroy(krwlock_t *l)
{
	if (l->hr_init)
		pthread_rwlock_destroy(&l->hr_lock);
	l->hr_init = 0;
}

void
rw_enter(krwlock_t *l, krw_t how)
{
	host_rw_lazy(l);
	if (how == RW_WRITER) {
		pthread_rwlock_wrlock(&l->hr_lock);
		l->hr_writer = 1;
	} else {
		pthread_rwlock_rdlock(&l->hr_lock);
	}
}

void
rw_exit(krwlock_t *l)
{
	l->hr_writer = 0;
	pthread_rwlock_unlock(&l->hr_lock);
}

int
rw_tryenter(krwlock_t *l, krw_t how)
{
	host_rw_lazy(l);
	if (how == RW_WRITER) {
		if (pthread_rwlock_trywrlock(&l->hr_lock) != 0)
			return (0);
		l->hr_writer = 1;
		return (1);
	}
	return (pthread_rwlock_tryrdlock(&l->hr_lock) == 0);
}

hrtime_t
gethrtime(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((hrtime_t)ts.tv_sec * NANOSEC + ts.tv_nsec);
}

void
drv_usecwait(clock_t us)
{
	usleep((useconds_t)us);
}

clock_t
drv_usectohz(clock_t us)
{
	return (us / 1000);
}

clock_t
drv_hztousec(clock_t hz)
{
	return (hz * 1000);
}

int
servicing_interrupt(void)
{
	return (0);
}

/* Set by the tests to exercise the fallback path of get_random_bytes(). */
int lkpi_host_random_fail;

int
random_get_pseudo_bytes(uint8_t *p, size_t len)
{
	static uint64_t state = 0x853c49e6748fea9bULL;
	size_t i;

	if (lkpi_host_random_fail)
		return (EIO);
	if (state == 0x853c49e6748fea9bULL)
		state ^= (uint64_t)gethrtime();
	for (i = 0; i < len; i++) {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		p[i] = (uint8_t)(state >> 24);
	}
	return (0);
}

int
random_get_bytes(uint8_t *p, size_t len)
{
	return (random_get_pseudo_bytes(p, len));
}
