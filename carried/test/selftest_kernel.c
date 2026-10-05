/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The kernel environment of the carried helpers' self-tests
 * (carried/test/selftest.h), for the test driver drm_helpers_test:
 * failures are logged with cmn_err(9F) (CE_WARN, console and system log),
 * memory comes from kmem_zalloc(9F) and parallel runs use a DDI task
 * queue of the test driver's node.
 */

#include <sys/types.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/kmem.h>
#include <sys/varargs.h>

#include "selftest.h"

/* The node the parallel runs' task queues belong to (set by attach). */
dev_info_t *st_kernel_dip;

void
st_fail(struct st_ctx *ctx, int line, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	ctx->checks_failed++;
	if (ctx->logged++ >= ST_MAX_LOGGED)
		return;
	va_start(ap, fmt);
	(void) vsnprintf(msg, sizeof (msg), fmt, ap);
	va_end(ap);
	cmn_err(CE_WARN, "drm_helpers_test: %s FAILED (line %d): %s",
	    ctx->test, line, msg);
}

void *
st_zalloc(size_t size)
{
	return (kmem_zalloc(size, KM_SLEEP));
}

void
st_free(void *ptr, size_t size)
{
	kmem_free(ptr, size);
}

/*
 * One task queue with n threads; ddi_taskq_destroy(9F) waits for every
 * dispatched task to finish. Dispatching with DDI_SLEEP cannot fail.
 */
int
st_run_parallel(void (*fn)(void *), void **args, unsigned int n)
{
	ddi_taskq_t *tq;
	unsigned int i;

	if (st_kernel_dip == NULL)
		return (-1);
	tq = ddi_taskq_create(st_kernel_dip, "selftest", (int)n,
	    TASKQ_DEFAULTPRI, 0);
	if (tq == NULL)
		return (-1);
	for (i = 0; i < n; i++)
		(void) ddi_taskq_dispatch(tq, fn, args[i], DDI_SLEEP);
	ddi_taskq_destroy(tq);
	return (0);
}
