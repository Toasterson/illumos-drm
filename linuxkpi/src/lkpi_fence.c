/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's fence containers and helpers on top of the DRM facade's fences
 * (<drm/dma_fence.h>, misc/drm_core):
 *
 * - fence arrays: a driver-side fence (dma_fence_init() with the array's
 *   own ops) that signals once all its fences have signalled, with the
 *   first error; the last child callback signals it from the layer's
 *   irq_work thread so that no fence lock is held while it signals;
 * - fence chains are the core's (timeline points of sync objects): no C
 *   fence is a chain, so the chain helpers behave as Linux's do for a fence
 *   that is not a chain;
 * - dma_fence_free(), dma_fence_describe(), dma_fence_wait_any_timeout().
 */

#include <linux/dma-fence.h>
#include <linux/dma-fence-array.h>
#include <linux/dma-fence-chain.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/wait.h>

/* --- arrays ------------------------------------------------------------ */

static const char *
array_driver_name(struct dma_fence *fence)
{
	return ("dma_fence_array");
}

static const char *
array_timeline_name(struct dma_fence *fence)
{
	return ("unbound");
}

/* The last child signalled: signal the array outside the child's lock. */
static void
array_signal_work(struct irq_work *work)
{
	struct dma_fence_array *array =
	    container_of(work, struct dma_fence_array, work);

	dma_fence_signal(&array->base);
	dma_fence_put(&array->base);
}

static void
array_set_error(struct dma_fence_array *array, int error)
{
	/* The first error wins; set it before the array signals. */
	if (error < 0 && array->base.error == 0)
		(void) cmpxchg(&array->base.error, 0, error);
}

static void
array_cb_func(struct dma_fence *f, struct dma_fence_cb *cb)
{
	struct dma_fence_array_cb *array_cb =
	    container_of(cb, struct dma_fence_array_cb, cb);
	struct dma_fence_array *array = array_cb->array;

	array_set_error(array, f->error);
	if (atomic_dec_and_test(&array->num_pending))
		(void) irq_work_queue(&array->work);
	else
		dma_fence_put(&array->base);
}

static bool
array_enable_signaling(struct dma_fence *fence)
{
	struct dma_fence_array *array = to_dma_fence_array(fence);
	struct dma_fence_array_cb *cb = array->callbacks;
	unsigned int i;

	for (i = 0; i < array->num_fences; i++) {
		cb[i].array = array;
		/* Each pending callback holds a reference to the array. */
		dma_fence_get(&array->base);
		if (dma_fence_add_callback(array->fences[i], &cb[i].cb,
		    array_cb_func) != 0) {
			/* Already signalled. */
			array_set_error(array, array->fences[i]->error);
			dma_fence_put(&array->base);
			if (atomic_dec_and_test(&array->num_pending))
				return (false);
		}
	}
	return (true);
}

static bool
array_signaled(struct dma_fence *fence)
{
	struct dma_fence_array *array = to_dma_fence_array(fence);

	return (atomic_read(&array->num_pending) <= 0);
}

static void
array_release(struct dma_fence *fence)
{
	struct dma_fence_array *array = to_dma_fence_array(fence);
	unsigned int i;

	for (i = 0; i < array->num_fences; i++)
		dma_fence_put(array->fences[i]);
	kfree(array->fences);
	dma_fence_free(fence);
}

const struct dma_fence_ops dma_fence_array_ops = {
	.get_driver_name = array_driver_name,
	.get_timeline_name = array_timeline_name,
	.enable_signaling = array_enable_signaling,
	.signaled = array_signaled,
	.release = array_release,
};

struct dma_fence_array *
dma_fence_array_alloc(int num_fences)
{
	struct dma_fence_array *array;

	if (num_fences <= 0)
		return (NULL);
	array = kzalloc(struct_size(array, callbacks, num_fences), GFP_KERNEL);
	return (array);
}

void
dma_fence_array_init(struct dma_fence_array *array, int num_fences,
    struct dma_fence **fences, u64 context, unsigned int seqno)
{
	spin_lock_init(&array->lock);
	array->num_fences = num_fences;
	atomic_set(&array->num_pending, num_fences);
	array->fences = fences;
	init_irq_work(&array->work, array_signal_work);
	dma_fence_init(&array->base, &dma_fence_array_ops, &array->lock,
	    context, seqno);
}

struct dma_fence_array *
dma_fence_array_create(int num_fences, struct dma_fence **fences,
    u64 context, unsigned int seqno)
{
	struct dma_fence_array *array = dma_fence_array_alloc(num_fences);

	if (array == NULL)
		return (NULL);
	dma_fence_array_init(array, num_fences, fences, context, seqno);
	return (array);
}

bool
dma_fence_match_context(struct dma_fence *fence, u64 context)
{
	struct dma_fence_array *array = to_dma_fence_array(fence);
	unsigned int i;

	if (array == NULL)
		return (fence->context == context);
	for (i = 0; i < array->num_fences; i++)
		if (array->fences[i]->context != context)
			return (false);
	return (true);
}

struct dma_fence *
dma_fence_array_first(struct dma_fence *head)
{
	struct dma_fence_array *array;

	if (head == NULL)
		return (NULL);
	array = to_dma_fence_array(head);
	if (array == NULL)
		return (head);
	if (array->num_fences == 0)
		return (NULL);
	return (array->fences[0]);
}

struct dma_fence *
dma_fence_array_next(struct dma_fence *head, unsigned int index)
{
	struct dma_fence_array *array = to_dma_fence_array(head);

	if (array == NULL || index >= array->num_fences)
		return (NULL);
	return (array->fences[index]);
}

/* --- chains: the core's ------------------------------------------------ */

static const char *
chain_name(struct dma_fence *fence)
{
	return ("dma_fence_chain");
}

/* No fence carries these ops: chains are the core's (see the header). */
const struct dma_fence_ops dma_fence_chain_ops = {
	.get_driver_name = chain_name,
	.get_timeline_name = chain_name,
};

struct dma_fence *
dma_fence_chain_walk(struct dma_fence *fence)
{
	/* Not a chain: the walk ends after the fence itself. */
	dma_fence_put(fence);
	return (NULL);
}

int
dma_fence_chain_find_seqno(struct dma_fence **pfence, uint64_t seqno)
{
	/* Linux's answer for a fence that is not a chain. */
	if (seqno == 0)
		return (0);
	return (-EINVAL);
}

void
dma_fence_chain_init(struct dma_fence_chain *chain, struct dma_fence *prev,
    struct dma_fence *fence, uint64_t seqno)
{
	/*
	 * Chains are made by the core (drm_syncobj_add_point()); a C chain
	 * would not be a timeline point anyone resolves.
	 */
	WARN(1, "dma_fence_chain_init is not supported; use "
	    "drm_syncobj_add_point()");
	(void) prev;
	(void) fence;
	(void) seqno;
	memset(chain, 0, sizeof (*chain));
}

/* --- helpers ------------------------------------------------------------ */

/*
 * The driver's release frees its fence structure; Linux delays the free
 * by an RCU grace period, as here.
 */
void
dma_fence_free(struct dma_fence *fence)
{
	lkpi_kvfree_rcu(fence, &fence->rcu);
}

/*
 * Linux's kref release of a fence; the facade's dma_fence_put() releases
 * fences, so nothing calls this (a WARN if something does).
 */
void
dma_fence_release(struct kref *kref)
{
	WARN(1, "dma_fence_release: fences are released by dma_fence_put()");
}

void
dma_fence_describe(struct dma_fence *fence, struct seq_file *seq)
{
	seq_printf(seq, "%s %s seq %llu %ssignalled\n",
	    dma_fence_driver_name(fence), dma_fence_timeline_name(fence),
	    (unsigned long long)fence->seqno,
	    dma_fence_is_signaled(fence) ? "" : "un");
}

struct dma_fence *
dma_fence_allocate_private_stub(ktime_t timestamp)
{
	/* A signalled fence; the facade's shared stub serves the purpose. */
	(void) timestamp;
	return (dma_fence_get_stub());
}

struct wait_any {
	struct dma_fence_cb cb;
	struct wait_queue_head *wq;
	volatile int *done;
};

static void
wait_any_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct wait_any *w = container_of(cb, struct wait_any, cb);

	*w->done = 1;
	wake_up_all(w->wq);
}

signed long
dma_fence_wait_any_timeout(struct dma_fence **fences, uint32_t count,
    bool intr, signed long timeout, uint32_t *idx)
{
	DECLARE_WAIT_QUEUE_HEAD(wq);
	struct wait_any *cbs;
	volatile int done = 0;
	signed long ret = timeout ? timeout : 1;
	uint32_t i, j;

	for (i = 0; i < count; i++) {
		if (dma_fence_is_signaled(fences[i])) {
			if (idx != NULL)
				*idx = i;
			return (ret);
		}
	}
	if (timeout == 0)
		return (0);
	cbs = kcalloc(count, sizeof (*cbs), GFP_KERNEL);
	if (cbs == NULL)
		return (-ENOMEM);
	init_waitqueue_head(&wq);
	for (i = 0; i < count; i++) {
		cbs[i].wq = &wq;
		cbs[i].done = &done;
		if (dma_fence_add_callback(fences[i], &cbs[i].cb,
		    wait_any_cb) != 0) {
			if (idx != NULL)
				*idx = i;
			goto out;
		}
	}
	if (intr)
		ret = wait_event_interruptible_timeout(wq, done != 0, timeout);
	else
		ret = wait_event_timeout(wq, done != 0, timeout);
	if (ret > 0 && idx != NULL) {
		for (j = 0; j < count; j++) {
			if (dma_fence_is_signaled(fences[j])) {
				*idx = j;
				break;
			}
		}
	}
out:
	for (j = 0; j < i && j < count; j++)
		(void) dma_fence_remove_callback(fences[j], &cbs[j].cb);
	kfree(cbs);
	return (ret);
}
