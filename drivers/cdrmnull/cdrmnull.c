/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * cdrmnull: a DRM driver without hardware, in C, on the illumos-drm facade.
 *
 * cdrmnull is the C port of the Rust test driver rdrmnull and the first C
 * driver of misc/drm_core (work item R5a): it uses nothing of the DRM core
 * but the facade's C functions (include/drm), and it is linked against
 * misc/drm_core (ld -N misc/drm_core) instead of carrying a copy of the
 * core. It registers one DRM device per instance of its pseudo node
 * (cdrmnull.conf asks for instance 0), with the primary node card<N> and
 * the render node renderD<128+N>, and declares mode setting and atomic
 * support, as rdrmnull does, so that every capability path of the core
 * answers. It has no display: rdrmnull's virtual display (milestone R3)
 * waits for the C mode-setting core (R5i). Nor does it port rdrmnull's
 * DMA test properties (dma-mode, dma-max-transfer), which exercise the
 * core's DMA binding on the device's own node; the Rust driver keeps
 * them.
 *
 * GEM objects and dumb buffers live in system memory. As a pseudo device
 * it does no DMA, so its objects have no scatter/gather table.
 *
 * It has driver-private ioctls that exist only for the kernel tests
 * (tools/test/drm_r1_test.c, drm_r2_test.c), with rdrmnull's numbers,
 * structures and errors (cdrmnull_ioctl.h):
 *
 * - CREATE_MOVABLE makes an object with two backings, the stand-in for an
 *   object a GPU's memory manager moves between system memory and VRAM.
 * - MOVE invalidates every user mapping of such an object with
 *   devmap_unload(9F) and, unless asked not to, switches the object to its
 *   other backing with devmap_umem_remap first. The next access through
 *   an existing mapping faults into devmap_access(9E), which loads the
 *   translation with devmap_load(9F).
 * - PUT_LATER takes a reference to an object and drops it from a
 *   timeout(9F) callback with drm_illumos_gem_object_put_deferred(), which
 *   exercises the deferred release of the core (timeout(9F) callbacks run
 *   on callout threads, which drm_gem_object_put() cannot tell from thread
 *   context).
 * - HOLD keeps a reference to an object until it is asked to drop it, also
 *   across the close of every file. detach(9E) is then refused, and the
 *   test checks that the device still works.
 * - FENCE creates a fence on the device's test timeline that signals after
 *   delay_ms milliseconds (optionally with an error), and puts it into a
 *   sync object at a point, into a buffer's reservation (as a writer or a
 *   reader), and/or into a new sync file. A timeout(9F) callback triggers
 *   a soft interrupt, whose handler signals every fence that is due, as a
 *   GPU driver signals fences from its completion interrupt; the fences'
 *   callbacks run there, in interrupt context.
 * - BO_WAIT waits for a buffer's reservation, as a driver's wait ioctl
 *   does.
 * - PRIME_SELFTEST (new in cdrmnull) imports a dma-buf or exports a
 *   buffer through the facade's dma-buf functions.
 *
 * A detach attempt cancels the timers and signals the fences that are left
 * with ECANCELED (illumos_before_detach), because every fence must signal.
 * The soft interrupt stays until the detach is sure to complete
 * (illumos_before_teardown), so a refused detach leaves the timeline
 * working.
 *
 * The test fences are the driver's own structures (struct cdrmnull_tlf,
 * dma_fence_init()), so they belong to the fence domain of misc/drm_core
 * (kstat drm:0:sync), not to the device's (kstat cdrmnull:<N>:sync).
 *
 * One symbol outside the DDI is used: devmap_umem_remap (see below).
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/debug.h>
#include <sys/sysmacros.h>
#include <sys/kmem.h>
#include <sys/ksynch.h>
#include <sys/list.h>
#include <sys/atomic.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/fcntl.h>
#include <sys/cmn_err.h>
#include <sys/conf.h>
#include <sys/modctl.h>
#include <sys/stat.h>
#include <sys/ddidevmap.h>
#include <sys/ddi_intr.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>

#include <drm/drm_facade.h>

#include "cdrmnull_ioctl.h"

#define	CDRMNULL_NAME		"cdrmnull"
/* Small limits, as rdrmnull's: 16 MiB per object, 64 MiB per device. */
#define	CDRMNULL_MAX_OBJECT	(16ULL << 20)
#define	CDRMNULL_MAX_MEMORY	(64ULL << 20)
#define	CDRMNULL_MAX_DELAY_MS	10000
/*
 * The page of GEM object sizes: the core's (drm_gem_object_init() takes
 * whole 4 KiB pages; misc/drm_core refuses to attach a device on a system with
 * another page size). PAGESIZE would be genunix's variable _pagesize,
 * which is not part of the DDI.
 */
#define	CDRMNULL_PAGE_SIZE	4096ULL

CTASSERT(sizeof (struct cdrmnull_create_movable) == 16);
CTASSERT(sizeof (struct cdrmnull_move) == 16);
CTASSERT(sizeof (struct cdrmnull_put_later) == 8);
CTASSERT(sizeof (struct cdrmnull_hold) == 8);
CTASSERT(sizeof (struct cdrmnull_fence) == 40);
CTASSERT(sizeof (struct cdrmnull_bo_wait) == 16);
CTASSERT(sizeof (struct cdrmnull_prime_selftest) == 16);

#define	CDRMNULL_MOVE_FLAGS \
	(CDRMNULL_MOVE_FILL | CDRMNULL_MOVE_UNLOAD_ONLY | 0xffu)

/*
 * devmap_umem_remap: points an existing mapping at other kernel memory
 * and unloads its translations.
 *
 * It is prototyped in sys/sunddi.h, implemented in vm/seg_dev.c and
 * exported by genunix (the rsm and InfiniBand drivers use it), but it has
 * no manual page and is not in the gate's ddi.mapfile, so private.mapfile
 * declares it for the link. It is declared here, as rdrmnull declares it,
 * for the test-only movable objects; the facade does not offer it. The
 * movable-object layer of milestone R5 needs it bound, in the private tier
 * or after a manual page upstream.
 */
extern int devmap_umem_remap(devmap_cookie_t dhp, dev_info_t *dip,
    ddi_umem_cookie_t cookie, offset_t off, size_t len, uint_t maxprot,
    uint_t flags, const ddi_device_acc_attr_t *accattrp);

/* A buffer object: a dumb buffer or a movable test object. */
struct cdrmnull_bo {
	struct drm_gem_object base;
	/* Handles to the object (funcs->open and funcs->close). */
	volatile uint32_t handles;
	/*
	 * Movable objects: the backing in use, 0 or 1. Changed only by MOVE
	 * (serialised by cdrmnull.move_lock); read by the mapping callbacks.
	 */
	volatile uint_t cur;
};

/* The per-file state (drm_file.driver_priv). */
struct cdrmnull_file {
	/* Handles of this driver's objects on the file. */
	volatile uint32_t handles;
	/* Test fences made through the file. */
	volatile uint32_t fences;
};

/* A fence of the test timeline and when it is due. */
struct cdrmnull_tlf {
	struct dma_fence base;
	list_node_t link;	/* on cdrmnull.tl_due while pending */
	hrtime_t due;		/* gethrtime(9F) */
};

/*
 * The timer of one test fence. It only triggers the soft interrupt, whose
 * handler finds the fences that are due, so it may outlive its fence; the
 * record is freed after untimeout(9F), which waits for a running callback.
 */
struct cdrmnull_timer {
	list_node_t link;	/* on cdrmnull.tl_timers */
	timeout_id_t tid;
	volatile uint_t fired;	/* set by the callback, a hint for reaping */
	struct cdrmnull *cn;
};

/* One device: the driver's structure around its struct drm_device. */
struct cdrmnull {
	struct drm_device drm;
	int instance;
	boolean_t locks_ready;	/* lock, move_lock, later_lock, timers */
	boolean_t tl_ready;	/* tl_lock and tl_due */

	/* held, later_tid, tl_seqno; serialises PUT_LATER. */
	kmutex_t lock;
	/* The reference HOLD keeps; not dropped by before_detach. */
	struct drm_gem_object *held;
	timeout_id_t later_tid;

	/* Serialises MOVE. */
	kmutex_t move_lock;

	/* The reference PUT_LATER hands to its timer; the timer takes it. */
	kmutex_t later_lock;
	struct drm_gem_object *later;

	/* The test timeline. */
	ddi_softint_handle_t softint;
	boolean_t softint_added;
	uint_t softint_pri;
	uint64_t tl_context;
	uint64_t tl_seqno;
	/* Fences waiting for their time; at the soft interrupt's priority. */
	kmutex_t tl_lock;
	list_t tl_due;
	uint64_t tl_signaled;
	/* The timers of pending fences; thread context only. */
	kmutex_t timers_lock;
	/* The calls of the unregistration test (cdrmnull_unregister_race). */
	volatile uint64_t race_calls;
	list_t tl_timers;
};

static void *cdrmnull_state;

static const struct drm_gem_object_funcs cdrmnull_bo_funcs;
static const struct drm_gem_object_funcs cdrmnull_movable_funcs;

static struct cdrmnull *
to_cn(struct drm_device *dev)
{
	return (container_of(dev, struct cdrmnull, drm));
}

static struct cdrmnull_bo *
to_bo(struct drm_gem_object *obj)
{
	return (container_of(obj, struct cdrmnull_bo, base));
}

static boolean_t
cdrmnull_is_movable(const struct drm_gem_object *obj)
{
	return (obj->funcs == &cdrmnull_movable_funcs);
}

/* --- objects -------------------------------------------------------------- */

static void
cdrmnull_bo_free(struct drm_gem_object *obj)
{
	struct cdrmnull_bo *bo = to_bo(obj);

	drm_gem_object_release(obj);
	kmem_free(bo, sizeof (*bo));
}

static int
cdrmnull_bo_open(struct drm_gem_object *obj, struct drm_file *file)
{
	struct cdrmnull_file *fp = file->driver_priv;

	atomic_inc_32_nv(&to_bo(obj)->handles);
	if (fp != NULL)
		atomic_inc_32_nv(&fp->handles);
	return (0);
}

static void
cdrmnull_bo_close(struct drm_gem_object *obj, struct drm_file *file)
{
	struct cdrmnull_file *fp = file->driver_priv;

	atomic_dec_32_nv(&to_bo(obj)->handles);
	if (fp != NULL)
		atomic_dec_32_nv(&fp->handles);
}

/* The memory user mappings of a movable object map now. */
static ddi_umem_cookie_t
cdrmnull_movable_umem(struct drm_gem_object *obj)
{
	return (drm_illumos_gem_umem(obj, to_bo(obj)->cur & 1));
}

/* Points a mapping piece of a movable object at its current backing. */
static int
cdrmnull_remap(struct drm_gem_object *obj, struct drm_illumos_mapping *map)
{
	ddi_device_acc_attr_t acc = {
		.devacc_attr_version = DDI_DEVICE_ATTR_V0,
		.devacc_attr_endian_flags = DDI_NEVERSWAP_ACC,
		.devacc_attr_dataorder = DDI_STRICTORDER_ACC,
	};
	ddi_umem_cookie_t c = cdrmnull_movable_umem(obj);

	if (c == NULL)
		return (-EIO);
	/*
	 * A live piece of this object (the caller is inside a mapping
	 * callback); the cookie belongs to the object, which the mapping
	 * keeps alive; the range is the piece's own range within the object;
	 * PROT_ALL as at setup; flags must be 0.
	 */
	if (devmap_umem_remap(drm_illumos_mapping_cookie(map), obj->dev->dev,
	    c, (offset_t)drm_illumos_mapping_object_offset(map),
	    drm_illumos_mapping_len(map), PROT_ALL, 0, &acc) != DDI_SUCCESS)
		return (-EIO);
	return (0);
}

/*
 * A mapping piece was set up (or copied, on fork and split) from the
 * backing current at the time; follow a move since then.
 */
static int
cdrmnull_movable_mapped(struct drm_gem_object *obj,
    struct drm_illumos_mapping *map)
{
	return (cdrmnull_remap(obj, map));
}

/*
 * The refault after a move: load the translation of the piece's current
 * backing explicitly, as a movable-object layer would after waiting for
 * the move to finish.
 */
static int
cdrmnull_movable_access(struct drm_gem_object *obj,
    struct drm_illumos_mapping *map, struct drm_illumos_fault *fault)
{
	(void) obj;
	(void) map;
	return (drm_illumos_fault_load(fault));
}

static const struct drm_gem_object_funcs cdrmnull_bo_funcs = {
	.free = cdrmnull_bo_free,
	.open = cdrmnull_bo_open,
	.close = cdrmnull_bo_close,
};

static const struct drm_gem_object_funcs cdrmnull_movable_funcs = {
	.free = cdrmnull_bo_free,
	.open = cdrmnull_bo_open,
	.close = cdrmnull_bo_close,
	.illumos_umem = cdrmnull_movable_umem,
	.illumos_mapped = cdrmnull_movable_mapped,
	.illumos_access = cdrmnull_movable_access,
	.illumos_movable = true,
};

/*
 * MODE_CREATE_DUMB: an object of system memory. The core has checked the
 * caller's permission; drm_mode_size_dumb() and drm_gem_object_init()
 * give the core's errors (EINVAL for the arguments, ENOMEM beyond the
 * object and device limits).
 */
static int
cdrmnull_dumb_create(struct drm_file *file, struct drm_device *dev,
    struct drm_mode_create_dumb *args)
{
	struct cdrmnull_bo *bo;
	int r;

	if ((r = drm_mode_size_dumb(dev, args, 0, 0)) != 0)
		return (r);
	bo = kmem_zalloc(sizeof (*bo), KM_SLEEP);
	bo->base.funcs = &cdrmnull_bo_funcs;
	if ((r = drm_gem_object_init(dev, &bo->base, args->size)) != 0) {
		/* Nothing refers to the object. */
		kmem_free(bo, sizeof (*bo));
		return (r);
	}
	r = drm_gem_handle_create(file, &bo->base, &args->handle);
	/* The handle holds its own reference; this one goes. */
	drm_gem_object_put(&bo->base);
	return (r);
}

/* --- the test timeline ---------------------------------------------------- */

static const char *
cdrmnull_fence_driver_name(struct dma_fence *fence)
{
	(void) fence;
	return (CDRMNULL_NAME);
}

static const char *
cdrmnull_fence_timeline_name(struct dma_fence *fence)
{
	(void) fence;
	return ("cdrmnull-test");
}

/*
 * The last reference is gone. The soft interrupt handler drops it as well
 * as threads do, so this only frees memory, which interrupt handlers may.
 */
static void
cdrmnull_fence_release(struct dma_fence *fence)
{
	kmem_free(container_of(fence, struct cdrmnull_tlf, base),
	    sizeof (struct cdrmnull_tlf));
}

static const struct dma_fence_ops cdrmnull_fence_ops = {
	.get_driver_name = cdrmnull_fence_driver_name,
	.get_timeline_name = cdrmnull_fence_timeline_name,
	.release = cdrmnull_fence_release,
};

/*
 * The soft interrupt: signals every fence that is due, one at a time,
 * outside the timeline lock and without allocating; the fences' callbacks
 * run here, in interrupt context.
 */
static uint_t
cdrmnull_softint(caddr_t arg1, caddr_t arg2)
{
	struct cdrmnull *cn = (struct cdrmnull *)(void *)arg1;
	hrtime_t now = gethrtime();
	uint_t claimed = DDI_INTR_UNCLAIMED;
	struct cdrmnull_tlf *f;

	(void) arg2;
	for (;;) {
		mutex_enter(&cn->tl_lock);
		for (f = list_head(&cn->tl_due); f != NULL;
		    f = list_next(&cn->tl_due, f)) {
			if (f->due <= now)
				break;
		}
		if (f != NULL) {
			list_remove(&cn->tl_due, f);
			cn->tl_signaled++;
		}
		mutex_exit(&cn->tl_lock);
		if (f == NULL)
			break;
		dma_fence_signal(&f->base);
		/* The timeline's reference. */
		dma_fence_put(&f->base);
		claimed = DDI_INTR_CLAIMED;
	}
	return (claimed);
}

/*
 * The timer of a test fence (a callout): triggers the soft interrupt of
 * its device only. A trigger while one is pending is merged (DDI_EPENDING).
 */
static void
cdrmnull_fence_timer(void *arg)
{
	struct cdrmnull_timer *t = arg;

	(void) ddi_intr_trigger_softint(t->cn->softint, NULL);
	t->fired = 1;
}

/*
 * Starts the timer `t` and frees the timers that have fired. Thread
 * context. untimeout(9F) of a fired timer returns at once, or waits while
 * its callback still runs; the callback takes no lock of the driver.
 */
static void
cdrmnull_arm_timer(struct cdrmnull *cn, struct cdrmnull_timer *t,
    clock_t ticks)
{
	struct cdrmnull_timer *o, *next;

	mutex_enter(&cn->timers_lock);
	for (o = list_head(&cn->tl_timers); o != NULL; o = next) {
		next = list_next(&cn->tl_timers, o);
		if (o->fired) {
			list_remove(&cn->tl_timers, o);
			(void) untimeout(o->tid);
			kmem_free(o, sizeof (*o));
		}
	}
	t->tid = timeout(cdrmnull_fence_timer, t, ticks);
	list_insert_tail(&cn->tl_timers, t);
	mutex_exit(&cn->timers_lock);
}

/* Cancels (or waits for) every fence timer. Thread context. */
static void
cdrmnull_cancel_timers(struct cdrmnull *cn)
{
	struct cdrmnull_timer *t;

	mutex_enter(&cn->timers_lock);
	while ((t = list_remove_head(&cn->tl_timers)) != NULL) {
		(void) untimeout(t->tid);
		kmem_free(t, sizeof (*t));
	}
	mutex_exit(&cn->timers_lock);
}

/*
 * Signals every pending fence with ECANCELED and returns how many there
 * were. Thread context; the soft interrupt may run concurrently and takes
 * each fence from the list at most once.
 */
static uint_t
cdrmnull_cancel_fences(struct cdrmnull *cn)
{
	struct cdrmnull_tlf *f;
	uint_t n = 0;

	for (;;) {
		mutex_enter(&cn->tl_lock);
		f = list_remove_head(&cn->tl_due);
		mutex_exit(&cn->tl_lock);
		if (f == NULL)
			break;
		dma_fence_set_error(&f->base, -ECANCELED);
		dma_fence_signal(&f->base);
		dma_fence_put(&f->base);
		n++;
	}
	return (n);
}

/* Removes the soft interrupt; no timer may trigger it any more. */
static void
cdrmnull_softint_remove(struct cdrmnull *cn)
{
	if (cn->softint_added) {
		(void) ddi_intr_remove_softint(cn->softint);
		cn->softint_added = B_FALSE;
	}
}

/* --- PUT_LATER ------------------------------------------------------------ */

/*
 * The timer callback, on a callout thread, which must not block: the
 * reference goes with drm_illumos_gem_object_put_deferred(), so that a
 * last reference is parked and released by the core's task queue (what
 * drm_gem_object_put() does in interrupt threads only).
 */
static void
cdrmnull_put_later_timer(void *arg)
{
	struct cdrmnull *cn = arg;
	struct drm_gem_object *obj;

	mutex_enter(&cn->later_lock);
	obj = cn->later;
	cn->later = NULL;
	mutex_exit(&cn->later_lock);
	if (obj != NULL)
		drm_illumos_gem_object_put_deferred(obj);
}

/* Cancels or finishes the timer, then drops a reference it never got to. */
static void
cdrmnull_cancel_put_later(struct cdrmnull *cn)
{
	struct drm_gem_object *obj;
	timeout_id_t tid;

	mutex_enter(&cn->lock);
	tid = cn->later_tid;
	cn->later_tid = 0;
	mutex_exit(&cn->lock);
	if (tid != 0)
		(void) untimeout(tid);
	mutex_enter(&cn->later_lock);
	obj = cn->later;
	cn->later = NULL;
	mutex_exit(&cn->later_lock);
	if (obj != NULL)
		drm_gem_object_put(obj);
}

/* --- the test ioctls ------------------------------------------------------ */

static int
cdrmnull_ioctl_create_movable(struct drm_device *dev, void *data,
    struct drm_file *file)
{
	struct cdrmnull_create_movable *a = data;
	struct cdrmnull_bo *bo;
	size_t size;
	uint32_t handle;
	int r;

	if (a->pad != 0)
		return (-EINVAL);
	if (a->size == 0)
		return (-EINVAL);
	if (a->size > CDRMNULL_MAX_OBJECT / 2)
		return (-ENOMEM);
	size = (size_t)P2ROUNDUP(a->size, CDRMNULL_PAGE_SIZE);

	bo = kmem_zalloc(sizeof (*bo), KM_SLEEP);
	bo->base.funcs = &cdrmnull_movable_funcs;
	if ((r = drm_gem_object_init(dev, &bo->base, size)) != 0) {
		kmem_free(bo, sizeof (*bo));
		return (r);
	}
	/* Backing 1, before the object gets a handle. */
	if ((r = drm_illumos_gem_add_backing(&bo->base)) < 0) {
		/* The last reference: funcs->free frees bo. */
		drm_gem_object_put(&bo->base);
		return (r);
	}
	r = drm_gem_handle_create(file, &bo->base, &handle);
	drm_gem_object_put(&bo->base);
	if (r != 0)
		return (r);
	a->handle = handle;
	return (0);
}

/* The state of one MOVE across the pieces of the object's mappings. */
struct cdrmnull_move_walk {
	struct drm_gem_object *obj;
	uint32_t flags;
	uint_t next;		/* the backing in use afterwards */
	boolean_t switched;	/* the backing switch is done */
	uint32_t pieces;
	int err;
};

/*
 * Fills and switches to the new backing. Called with the object's mapping
 * lock held when the object has mappings: the backing not in use is then
 * unreachable for every process, because every piece points at the other
 * one.
 */
static void
cdrmnull_move_switch(struct cdrmnull_move_walk *w)
{
	struct drm_gem_object *obj = w->obj;
	void *p;

	w->switched = B_TRUE;
	if ((w->flags & CDRMNULL_MOVE_UNLOAD_ONLY) != 0)
		return;
	if ((w->flags & CDRMNULL_MOVE_FILL) != 0) {
		if ((p = drm_illumos_gem_vaddr(obj, w->next)) == NULL) {
			w->err = -EIO;
			return;
		}
		(void) memset(p, (int)(w->flags & 0xff), obj->size);
	}
	membar_producer();
	to_bo(obj)->cur = w->next;
}

static void
cdrmnull_move_piece(struct drm_illumos_mapping *map, void *arg)
{
	struct cdrmnull_move_walk *w = arg;

	if (!w->switched)
		cdrmnull_move_switch(w);
	if (w->err != 0)
		return;
	/*
	 * A piece that a partial munmap(2) split off may still be finished by
	 * the framework and must not be remapped yet; invalidating it is
	 * enough, because its next fault settles it on the current backing.
	 */
	if ((w->flags & CDRMNULL_MOVE_UNLOAD_ONLY) == 0 &&
	    drm_illumos_mapping_is_settled(map)) {
		if (cdrmnull_remap(w->obj, map) != 0) {
			w->err = -EIO;
			return;
		}
	}
	if (drm_illumos_mapping_unload(map) != 0) {
		w->err = -EIO;
		return;
	}
	w->pieces++;
}

static int
cdrmnull_ioctl_move(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct cdrmnull *cn = to_cn(dev);
	struct cdrmnull_move *a = data;
	struct cdrmnull_move_walk w;
	struct drm_gem_object *obj;

	if ((a->flags & ~CDRMNULL_MOVE_FLAGS) != 0 ||
	    ((a->flags & CDRMNULL_MOVE_UNLOAD_ONLY) != 0 &&
	    (a->flags & CDRMNULL_MOVE_FILL) != 0))
		return (-EINVAL);
	if ((obj = drm_gem_object_lookup(file, a->handle)) == NULL)
		return (-ENOENT);
	/* Only this driver's movable objects (not dumb buffers, imports). */
	if (!cdrmnull_is_movable(obj)) {
		drm_gem_object_put(obj);
		return (-EINVAL);
	}

	mutex_enter(&cn->move_lock);
	bzero(&w, sizeof (w));
	w.obj = obj;
	w.flags = a->flags;
	w.next = to_bo(obj)->cur & 1;
	if ((a->flags & CDRMNULL_MOVE_UNLOAD_ONLY) == 0)
		w.next ^= 1;
	drm_illumos_gem_foreach_mapping(obj, cdrmnull_move_piece, &w);
	if (!w.switched && w.err == 0) {
		/*
		 * No mapping: switch now, then catch a piece that was set up
		 * from the old backing in the meantime.
		 */
		cdrmnull_move_switch(&w);
		if (w.err == 0 && (a->flags & CDRMNULL_MOVE_UNLOAD_ONLY) == 0)
			drm_illumos_gem_foreach_mapping(obj,
			    cdrmnull_move_piece, &w);
	}
	mutex_exit(&cn->move_lock);
	drm_gem_object_put(obj);

	if (w.err != 0)
		return (w.err);
	a->pieces = w.pieces;
	a->backing = w.next;
	return (0);
}

static int
cdrmnull_ioctl_put_later(struct drm_device *dev, void *data,
    struct drm_file *file)
{
	struct cdrmnull *cn = to_cn(dev);
	struct cdrmnull_put_later *a = data;
	struct drm_gem_object *obj;
	boolean_t busy;

	if (a->delay_ms < 1 || a->delay_ms > CDRMNULL_MAX_DELAY_MS)
		return (-EINVAL);
	mutex_enter(&cn->lock);
	mutex_enter(&cn->later_lock);
	busy = (cn->later != NULL);
	mutex_exit(&cn->later_lock);
	if (busy) {
		mutex_exit(&cn->lock);
		return (-EBUSY);
	}
	if ((obj = drm_gem_object_lookup(file, a->handle)) == NULL) {
		mutex_exit(&cn->lock);
		return (-ENOENT);
	}
	/*
	 * The previous timer has fired (the slot was empty); untimeout only
	 * waits for its callback to return.
	 */
	if (cn->later_tid != 0)
		(void) untimeout(cn->later_tid);
	mutex_enter(&cn->later_lock);
	cn->later = obj;
	mutex_exit(&cn->later_lock);
	cn->later_tid = timeout(cdrmnull_put_later_timer, cn,
	    drv_usectohz((clock_t)a->delay_ms * 1000));
	mutex_exit(&cn->lock);
	return (0);
}

static int
cdrmnull_ioctl_hold(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct cdrmnull *cn = to_cn(dev);
	struct cdrmnull_hold *a = data;
	struct drm_gem_object *obj;

	switch (a->hold) {
	case 1:
		mutex_enter(&cn->lock);
		if (cn->held != NULL) {
			mutex_exit(&cn->lock);
			return (-EBUSY);
		}
		if ((obj = drm_gem_object_lookup(file, a->handle)) == NULL) {
			mutex_exit(&cn->lock);
			return (-ENOENT);
		}
		cn->held = obj;
		mutex_exit(&cn->lock);
		return (0);
	case 0:
		mutex_enter(&cn->lock);
		obj = cn->held;
		cn->held = NULL;
		mutex_exit(&cn->lock);
		if (obj == NULL)
			return (-ENOENT);
		drm_gem_object_put(obj);
		return (0);
	default:
		return (-EINVAL);
	}
}

static int
cdrmnull_ioctl_fence(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct cdrmnull *cn = to_cn(dev);
	struct cdrmnull_fence *a = data;
	struct cdrmnull_file *fp = file->driver_priv;
	struct drm_syncobj *so = NULL;
	struct dma_fence_chain *chain = NULL;
	struct drm_gem_object *bo = NULL;
	struct cdrmnull_timer *t;
	struct cdrmnull_tlf *f;
	uint64_t seqno;
	uint32_t delay_ms;
	int fd, r = 0;

	if ((a->flags & ~CDRMNULL_FENCE_FLAGS) != 0 ||
	    a->delay_ms > CDRMNULL_MAX_DELAY_MS || a->error < 0 ||
	    ((a->flags & CDRMNULL_FENCE_BO_WRITE) != 0 &&
	    (a->flags & CDRMNULL_FENCE_BO_READ) != 0))
		return (-EINVAL);

	/* Everything that can fail on the caller's input precedes the fence. */
	if ((a->flags & CDRMNULL_FENCE_SYNCOBJ) != 0) {
		if ((so = drm_syncobj_find(file, a->syncobj)) == NULL)
			return (-ENOENT);
		if (a->point != 0 && (chain = dma_fence_chain_alloc()) == NULL) {
			drm_syncobj_put(so);
			return (-ENOMEM);
		}
	}
	if ((a->flags & (CDRMNULL_FENCE_BO_WRITE |
	    CDRMNULL_FENCE_BO_READ)) != 0) {
		if ((bo = drm_gem_object_lookup(file, a->bo)) == NULL) {
			if (chain != NULL)
				dma_fence_chain_free(chain);
			if (so != NULL)
				drm_syncobj_put(so);
			return (-ENOENT);
		}
	}

	t = kmem_zalloc(sizeof (*t), KM_SLEEP);
	t->cn = cn;
	f = kmem_zalloc(sizeof (*f), KM_SLEEP);
	mutex_enter(&cn->lock);
	seqno = ++cn->tl_seqno;
	mutex_exit(&cn->lock);
	dma_fence_init(&f->base, &cdrmnull_fence_ops, NULL, cn->tl_context,
	    seqno);
	if (dma_fence_is_signaled(&f->base)) {
		/*
		 * The core had no memory for the fence and handed it back
		 * signalled with -ENOMEM; nothing refers to it yet.
		 */
		dma_fence_put(&f->base);
		kmem_free(t, sizeof (*t));
		if (bo != NULL)
			drm_gem_object_put(bo);
		if (chain != NULL)
			dma_fence_chain_free(chain);
		if (so != NULL)
			drm_syncobj_put(so);
		return (-ENOMEM);
	}
	if (a->error != 0)
		dma_fence_set_error(&f->base, -a->error);

	if (so != NULL) {
		if (a->point == 0) {
			drm_syncobj_replace_fence(so, &f->base);
		} else {
			drm_syncobj_add_point(so, chain, &f->base, a->point);
			chain = NULL;
		}
		drm_syncobj_put(so);
		so = NULL;
	}
	if (bo != NULL) {
		if ((r = dma_resv_lock(bo->resv, NULL)) == 0) {
			if ((r = dma_resv_reserve_fences(bo->resv, 1)) == 0) {
				dma_resv_add_fence(bo->resv, &f->base,
				    (a->flags & CDRMNULL_FENCE_BO_WRITE) != 0 ?
				    DMA_RESV_USAGE_WRITE : DMA_RESV_USAGE_READ);
			}
			dma_resv_unlock(bo->resv);
		}
		drm_gem_object_put(bo);
		bo = NULL;
	}
	if (r == 0 && (a->flags & CDRMNULL_FENCE_SYNC_FILE) != 0) {
		if ((r = drm_sync_file_install(&f->base, true, &fd)) == 0)
			a->fd = fd;
	}

	/*
	 * Arm the fence whatever happened, so that it signals. From here the
	 * fence and its reference belong to the timeline: the soft interrupt
	 * may signal and free it at once.
	 */
	delay_ms = a->delay_ms;
	f->due = gethrtime() + MSEC2NSEC(delay_ms);
	mutex_enter(&cn->tl_lock);
	list_insert_tail(&cn->tl_due, f);
	mutex_exit(&cn->tl_lock);
	/*
	 * timeout(9F) counts from the current tick, so it can fire up to one
	 * tick before delay_ms has passed on the monotonic clock; one tick
	 * more makes sure the fence is due when the soft interrupt runs.
	 */
	cdrmnull_arm_timer(cn, t, drv_usectohz((clock_t)delay_ms * 1000) + 1);

	if (r != 0)
		return (r);
	a->seqno = seqno;
	if (fp != NULL)
		atomic_inc_32_nv(&fp->fences);
	return (0);
}

static int
cdrmnull_ioctl_bo_wait(struct drm_device *dev, void *data,
    struct drm_file *file)
{
	struct cdrmnull_bo_wait *a = data;
	struct drm_gem_object *obj;
	long ticks, lr;

	(void) dev;
	if ((a->flags & ~CDRMNULL_BO_WAIT_WRITERS) != 0)
		return (-EINVAL);
	if ((obj = drm_gem_object_lookup(file, a->handle)) == NULL)
		return (-ENOENT);
	if (a->timeout_ns < 0) {
		ticks = MAX_SCHEDULE_TIMEOUT;
	} else {
		/* Microseconds rounded up, then ticks rounded up. */
		clock_t us = (clock_t)(a->timeout_ns / 1000) +
		    (a->timeout_ns % 1000 != 0 ? 1 : 0);
		ticks = drv_usectohz(us);
	}
	lr = dma_resv_wait_timeout(obj->resv,
	    (a->flags & CDRMNULL_BO_WAIT_WRITERS) != 0 ?
	    DMA_RESV_USAGE_WRITE : DMA_RESV_USAGE_READ, true,
	    (unsigned long)ticks);
	drm_gem_object_put(obj);
	if (lr > 0)
		return (0);
	if (lr == 0)
		return (-ETIME);
	return ((int)lr);
}

static int
cdrmnull_ioctl_prime_selftest(struct drm_device *dev, void *data,
    struct drm_file *file)
{
	struct cdrmnull_prime_selftest *a = data;
	struct drm_gem_object *obj;
	struct dma_buf *dbuf;
	uint32_t handle;
	int r, fd;

	if (a->pad != 0)
		return (-EINVAL);
	switch (a->flags) {
	case CDRMNULL_PRIME_IMPORT:
		dbuf = dma_buf_get(a->fd);
		if (dbuf == NULL)
			return (-EINVAL);
		if (IS_ERR(dbuf))
			return ((int)PTR_ERR(dbuf));
		obj = drm_gem_prime_import(dev, dbuf);
		if (obj == NULL || IS_ERR(obj)) {
			dma_buf_put(dbuf);
			return (obj == NULL ? -ENOMEM : (int)PTR_ERR(obj));
		}
		r = drm_gem_handle_create(file, obj, &handle);
		/* The handle and the import hold what they need. */
		drm_gem_object_put(obj);
		dma_buf_put(dbuf);
		if (r != 0)
			return (r);
		a->handle = handle;
		return (0);
	case CDRMNULL_PRIME_EXPORT:
		if ((obj = drm_gem_object_lookup(file, a->handle)) == NULL)
			return (-ENOENT);
		dbuf = drm_gem_prime_export(obj, DRM_CLOEXEC | DRM_RDWR);
		if (dbuf == NULL || IS_ERR(dbuf)) {
			drm_gem_object_put(obj);
			return (dbuf == NULL ? -ENOMEM : (int)PTR_ERR(dbuf));
		}
		fd = dma_buf_fd(dbuf, DRM_CLOEXEC | DRM_RDWR);
		drm_gem_object_put(obj);
		if (fd < 0) {
			dma_buf_put(dbuf);
			return (fd);
		}
		/* The descriptor took over our reference, as on Linux. */
		a->fd = fd;
		return (0);
	default:
		return (-EINVAL);
	}
}

/* --- FENCE_SELFTEST: the facade's fence callbacks ----------------------- */

struct cdrmnull_cbt {
	struct dma_fence_cb cb;
	volatile uint32_t ran;
	boolean_t put_fence;
};

static void
cdrmnull_cbt_func(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct cdrmnull_cbt *t = container_of(cb, struct cdrmnull_cbt, cb);

	t->ran++;
	/* The last reference to the structure of this very fence. */
	if (t->put_fence)
		dma_fence_put(fence);
}

/*
 * A new fence of the device with a second structure for it: `*second`
 * from the sync object, which holds the core fence after this.
 */
static struct dma_fence *
cdrmnull_fence_pair(struct drm_device *dev, struct drm_file *file,
    struct drm_syncobj *so, uint32_t handle, struct dma_fence **second)
{
	struct dma_fence *f;

	f = drm_illumos_fence_create(dev, dma_fence_context_alloc(1), 1);
	if (IS_ERR(f))
		return (NULL);
	drm_syncobj_replace_fence(so, f);
	*second = NULL;
	if (drm_syncobj_find_fence(file, handle, 0, 0, second) != 0 ||
	    *second == NULL || *second == f) {
		drm_syncobj_replace_fence(so, NULL);
		dma_fence_put(*second);
		dma_fence_put(f);
		return (NULL);
	}
	return (f);
}

static int
cdrmnull_ioctl_fence_selftest(struct drm_device *dev, void *data,
    struct drm_file *file)
{
	struct cdrmnull_fence_selftest *a = data;
	struct cdrmnull_cbt *t;
	struct drm_syncobj *so;
	struct dma_fence *f, *g;
	uint32_t passed = 0;

	if (a->pad != 0 || a->pad2 != 0)
		return (-EINVAL);
	if ((so = drm_syncobj_find(file, a->syncobj)) == NULL)
		return (-ENOENT);
	t = kmem_zalloc(2 * sizeof (*t), KM_SLEEP);

	/* SELF_PUT: the callback drops the last reference to f. */
	if ((f = cdrmnull_fence_pair(dev, file, so, a->syncobj, &g)) != NULL) {
		t[0].put_fence = B_TRUE;
		if (dma_fence_add_callback(f, &t[0].cb, cdrmnull_cbt_func) ==
		    0) {
			/* f's reference is the callback's now. */
			dma_fence_signal(g);
			if (t[0].ran == 1)
				passed |= CDRMNULL_FENCE_T_SELF_PUT;
		} else {
			dma_fence_put(f);
		}
		dma_fence_put(g);
		drm_syncobj_replace_fence(so, NULL);
	}

	/* REMOVE and SIGNALED, on one structure. */
	bzero(t, 2 * sizeof (*t));
	f = drm_illumos_fence_create(dev, dma_fence_context_alloc(1), 1);
	if (!IS_ERR(f)) {
		if (dma_fence_add_callback(f, &t[0].cb, cdrmnull_cbt_func) ==
		    0 && dma_fence_remove_callback(f, &t[0].cb)) {
			dma_fence_signal(f);
			if (t[0].ran == 0)
				passed |= CDRMNULL_FENCE_T_REMOVE;
		}
		if (dma_fence_add_callback(f, &t[1].cb, cdrmnull_cbt_func) ==
		    -ENOENT && t[1].ran == 0)
			passed |= CDRMNULL_FENCE_T_SIGNALED;
		dma_fence_put(f);
	}

	/* RELEASED: f goes with its callback added. */
	bzero(t, 2 * sizeof (*t));
	if ((f = cdrmnull_fence_pair(dev, file, so, a->syncobj, &g)) != NULL) {
		if (dma_fence_add_callback(f, &t[0].cb, cdrmnull_cbt_func) ==
		    0) {
			dma_fence_put(f);
			dma_fence_signal(g);
			if (t[0].ran == 0)
				passed |= CDRMNULL_FENCE_T_RELEASED;
		} else {
			dma_fence_put(f);
		}
		dma_fence_put(g);
		drm_syncobj_replace_fence(so, NULL);
	}

	/* QUEUED: the first callback releases f; the second never runs. */
	bzero(t, 2 * sizeof (*t));
	if ((f = cdrmnull_fence_pair(dev, file, so, a->syncobj, &g)) != NULL) {
		t[0].put_fence = B_TRUE;
		if (dma_fence_add_callback(f, &t[0].cb, cdrmnull_cbt_func) ==
		    0 && dma_fence_add_callback(f, &t[1].cb,
		    cdrmnull_cbt_func) == 0) {
			dma_fence_signal(g);
			if (t[0].ran == 1 && t[1].ran == 0)
				passed |= CDRMNULL_FENCE_T_QUEUED;
		} else {
			dma_fence_put(f);
		}
		dma_fence_put(g);
		drm_syncobj_replace_fence(so, NULL);
	}

	kmem_free(t, 2 * sizeof (*t));
	drm_syncobj_put(so);
	a->passed = passed;
	return (0);
}

/*
 * Permissions as rdrmnull's: the tests run as root on the primary node,
 * without authentication; the fence, wait and PRIME ioctls also on render
 * nodes.
 */
static const struct drm_ioctl_desc cdrmnull_ioctls[] = {
	DRM_IOCTL_DEF_DRV(CDRMNULL_CREATE_MOVABLE,
	    cdrmnull_ioctl_create_movable, 0),
	DRM_IOCTL_DEF_DRV(CDRMNULL_MOVE, cdrmnull_ioctl_move, 0),
	DRM_IOCTL_DEF_DRV(CDRMNULL_PUT_LATER, cdrmnull_ioctl_put_later, 0),
	DRM_IOCTL_DEF_DRV(CDRMNULL_HOLD, cdrmnull_ioctl_hold, 0),
	DRM_IOCTL_DEF_DRV(CDRMNULL_FENCE, cdrmnull_ioctl_fence,
	    DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CDRMNULL_BO_WAIT, cdrmnull_ioctl_bo_wait,
	    DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CDRMNULL_PRIME_SELFTEST,
	    cdrmnull_ioctl_prime_selftest, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(CDRMNULL_FENCE_SELFTEST,
	    cdrmnull_ioctl_fence_selftest, DRM_RENDER_ALLOW),
};

/* --- the DRM driver ------------------------------------------------------- */

static int
cdrmnull_open(struct drm_device *dev, struct drm_file *file)
{
	(void) dev;
	file->driver_priv = kmem_zalloc(sizeof (struct cdrmnull_file),
	    KM_SLEEP);
	return (0);
}

static void
cdrmnull_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct cdrmnull_file *fp = file->driver_priv;

	if (fp == NULL)
		return;
	/* The core has released the file's handles before this call. */
	if (fp->handles != 0) {
		drm_warn(dev, "file of pid %d closed with %u handles left",
		    (int)file->pid, fp->handles);
	}
	drm_dbg(dev, "file of pid %d closed; %u test fences",
	    (int)file->pid, fp->fences);
	file->driver_priv = NULL;
	kmem_free(fp, sizeof (*fp));
}

/*
 * A detach attempt that may still be refused. Only references and work
 * the driver holds for itself go here; the timeline keeps running. The
 * reference of HOLD stays, so that the detach is refused while it exists.
 */
static void
cdrmnull_before_detach(struct drm_device *dev)
{
	struct cdrmnull *cn = to_cn(dev);
	uint_t n;

	/*
	 * Cancel the fence timers and signal the fences that are left: every
	 * fence must signal.
	 */
	cdrmnull_cancel_timers(cn);
	n = cdrmnull_cancel_fences(cn);
	if (n != 0) {
		/* The kernel test looks for this line. */
		cmn_err(CE_NOTE, "cdrmnull%d: detach cancelled %u pending "
		    "test fences (ECANCELED)", cn->instance, n);
	}
	cdrmnull_cancel_put_later(cn);
}

/*
 * The detach completes: no timer may reach the soft interrupt any more,
 * which goes now.
 */
static void
cdrmnull_before_teardown(struct drm_device *dev)
{
	struct cdrmnull *cn = to_cn(dev);
	uint_t n;

	cdrmnull_cancel_timers(cn);
	if ((n = cdrmnull_cancel_fences(cn)) != 0) {
		cmn_err(CE_NOTE, "cdrmnull%d: detach cancelled %u pending "
		    "test fences (ECANCELED)", cn->instance, n);
	}
	cdrmnull_softint_remove(cn);
}

/*
 * The last reference to the device is gone (attach failed, or the detach
 * completed); the core frees the structure after this.
 */
static void
cdrmnull_release(struct drm_device *dev)
{
	struct cdrmnull *cn = to_cn(dev);

	/*
	 * Normally before_detach and before_teardown have done all of this;
	 * after a failed attach there is nothing to cancel. Timers go before
	 * the soft interrupt they trigger.
	 */
	if (cn->locks_ready) {
		cdrmnull_cancel_timers(cn);
		cdrmnull_cancel_put_later(cn);
	}
	cdrmnull_softint_remove(cn);
	if (cn->tl_ready) {
		(void) cdrmnull_cancel_fences(cn);
		list_destroy(&cn->tl_due);
		mutex_destroy(&cn->tl_lock);
		cn->tl_ready = B_FALSE;
	}
	if (cn->locks_ready) {
		list_destroy(&cn->tl_timers);
		mutex_destroy(&cn->timers_lock);
		mutex_destroy(&cn->later_lock);
		mutex_destroy(&cn->move_lock);
		mutex_destroy(&cn->lock);
		cn->locks_ready = B_FALSE;
	}
}

static const struct drm_driver cdrmnull_driver = {
	.open = cdrmnull_open,
	.postclose = cdrmnull_postclose,
	.release = cdrmnull_release,
	.dumb_create = cdrmnull_dumb_create,
	.major = 1,
	.minor = 0,
	.patchlevel = 0,
	.name = CDRMNULL_NAME,
	.desc = "C DRM null driver (illumos-drm R5a)",
	.driver_features = DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_RENDER |
	    DRIVER_GEM | DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE,
	.ioctls = cdrmnull_ioctls,
	.num_ioctls = (int)ARRAY_SIZE(cdrmnull_ioctls),
	DRM_FACADE_DRIVER_VERSION,
	.illumos_date = "20261001",
	.illumos_max_object_size = CDRMNULL_MAX_OBJECT,
	.illumos_max_memory = CDRMNULL_MAX_MEMORY,
	.illumos_before_detach = cdrmnull_before_detach,
	.illumos_before_teardown = cdrmnull_before_teardown,
};

/*
 * What GET_CAP reports: a 24-bit preferred depth with a shadow, and
 * framebuffer modifiers; the cursor size 0 reports Linux's default 64.
 */
static const struct drm_illumos_mode_config cdrmnull_mode_config = {
	.cursor_width = 0,
	.cursor_height = 0,
	.preferred_depth = 24,
	.prefer_shadow = true,
	.fb_modifiers = true,
};

/* --- autoconfiguration ---------------------------------------------------- */

static int
cdrmnull_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	struct cdrmnull **slot;
	struct cdrmnull *cn;
	int instance, r;

	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);
	instance = ddi_get_instance(dip);
	if (ddi_soft_state_zalloc(cdrmnull_state, instance) != DDI_SUCCESS)
		return (DDI_FAILURE);
	slot = ddi_get_soft_state(cdrmnull_state, instance);

	cn = drm_dev_alloc_container(dip, &cdrmnull_driver, struct cdrmnull,
	    drm);
	if (IS_ERR(cn)) {
		dev_err(dip, CE_WARN, "cannot allocate the DRM device (%ld)",
		    PTR_ERR(cn));
		ddi_soft_state_free(cdrmnull_state, instance);
		return (DDI_FAILURE);
	}
	/* From here drm_dev_put() undoes everything (cdrmnull_release). */
	cn->instance = instance;
	mutex_init(&cn->lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&cn->move_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&cn->later_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&cn->timers_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&cn->tl_timers, sizeof (struct cdrmnull_timer),
	    offsetof(struct cdrmnull_timer, link));
	cn->locks_ready = B_TRUE;

	if (ddi_intr_add_softint(dip, &cn->softint, DDI_INTR_SOFTPRI_DEFAULT,
	    cdrmnull_softint, (caddr_t)cn) != DDI_SUCCESS) {
		dev_err(dip, CE_WARN, "cannot add the timeline soft interrupt");
		goto fail;
	}
	cn->softint_added = B_TRUE;
	if (ddi_intr_get_softint_pri(cn->softint, &cn->softint_pri) !=
	    DDI_SUCCESS) {
		dev_err(dip, CE_WARN, "no priority of the soft interrupt");
		goto fail;
	}
	/* The handler takes tl_lock: a lock of its priority. */
	mutex_init(&cn->tl_lock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(cn->softint_pri));
	list_create(&cn->tl_due, sizeof (struct cdrmnull_tlf),
	    offsetof(struct cdrmnull_tlf, link));
	cn->tl_ready = B_TRUE;
	cn->tl_context = dma_fence_context_alloc(1);

	/* The core's locks that a signal from the soft interrupt takes. */
	if ((r = drm_dev_illumos_set_fence_priority(&cn->drm,
	    cn->softint_pri)) != 0) {
		dev_err(dip, CE_WARN, "cannot set the fence priority (%d)", r);
		goto fail;
	}
	if ((r = drm_dev_illumos_set_mode_config(&cn->drm,
	    &cdrmnull_mode_config)) != 0) {
		dev_err(dip, CE_WARN, "cannot set the mode configuration (%d)",
		    r);
		goto fail;
	}
	*slot = cn;
	if ((r = drm_dev_register(&cn->drm, 0)) != 0) {
		dev_err(dip, CE_WARN, "cannot register the DRM device (%d)", r);
		*slot = NULL;
		goto fail;
	}
	drm_info(&cn->drm, "card%d and renderD%d, test timeline at soft "
	    "interrupt priority %u", cn->drm.primary->index,
	    cn->drm.render != NULL ? cn->drm.render->index : -1,
	    cn->softint_pri);
	return (DDI_SUCCESS);

fail:
	cdrmnull_softint_remove(cn);
	drm_dev_put(&cn->drm);
	ddi_soft_state_free(cdrmnull_state, instance);
	return (DDI_FAILURE);
}

/*
 * The unregistration test: while detach unregisters the device, periodic
 * handlers on other threads, one at thread level and one at soft
 * interrupt level, keep calling a facade function that uses the
 * registered device (drm_illumos_fence_context_alloc(), any context).
 * They must neither fail nor touch a freed device: misc/drm_core takes
 * the device out of its tables and waits for every such call before it
 * frees it.
 */
static void
cdrmnull_unregister_race(void *arg)
{
	struct cdrmnull *cn = arg;
	int i;

	for (i = 0; i < 4096; i++)
		(void) drm_illumos_fence_context_alloc(&cn->drm, 1);
	atomic_add_64(&cn->race_calls, 4096);
}

static int
cdrmnull_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	struct cdrmnull **slot;
	struct cdrmnull *cn;
	ddi_periodic_t p0, p1;
	int instance, r;

	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);
	instance = ddi_get_instance(dip);
	slot = ddi_get_soft_state(cdrmnull_state, instance);
	if (slot == NULL || (cn = *slot) == NULL)
		return (DDI_FAILURE);
	/*
	 * Refused (-EBUSY) while files are open or objects, fences or sync
	 * objects remain; the device is then still registered and working.
	 */
	cn->race_calls = 0;
	p0 = ddi_periodic_add(cdrmnull_unregister_race, cn, 1000000,
	    DDI_IPL_0);
	p1 = ddi_periodic_add(cdrmnull_unregister_race, cn, 1000000,
	    DDI_IPL_1);
	/* Let both run a few times before and during the unregistration. */
	delay(drv_usectohz(5000));
	r = drm_dev_unregister(&cn->drm);
	delay(drv_usectohz(5000));
	ddi_periodic_delete(p0);
	ddi_periodic_delete(p1);
	dev_err(dip, CE_NOTE, "!unregister (%d) raced %llu facade calls", r,
	    (unsigned long long)cn->race_calls);
	if (r != 0) {
		dev_err(dip, CE_NOTE, "!detach refused (%d)", r);
		return (DDI_FAILURE);
	}
	cdrmnull_softint_remove(cn);
	*slot = NULL;
	drm_dev_put(&cn->drm);
	ddi_soft_state_free(cdrmnull_state, instance);
	return (DDI_SUCCESS);
}

static struct cb_ops cdrmnull_cb_ops = DRM_CB_OPS;

static struct dev_ops cdrmnull_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_refcnt = 0,
	.devo_getinfo = drm_getinfo,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = cdrmnull_attach,
	.devo_detach = cdrmnull_detach,
	.devo_reset = nodev,
	.devo_cb_ops = &cdrmnull_cb_ops,
	.devo_bus_ops = NULL,
	.devo_power = NULL,
	.devo_quiesce = ddi_quiesce_not_needed,
};

static struct modldrv cdrmnull_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = "C DRM null driver r5a",
	.drv_dev_ops = &cdrmnull_dev_ops,
};

static struct modlinkage cdrmnull_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &cdrmnull_modldrv, NULL },
};

int
_init(void)
{
	int r;

	/* misc/drm_core must be the facade this driver was compiled against. */
	if ((r = drm_facade_check(CDRMNULL_NAME)) != 0)
		return (r);
	if ((r = ddi_soft_state_init(&cdrmnull_state,
	    sizeof (struct cdrmnull *), 1)) != 0)
		return (r);
	if ((r = mod_install(&cdrmnull_modlinkage)) != 0)
		ddi_soft_state_fini(&cdrmnull_state);
	return (r);
}

int
_fini(void)
{
	int r;

	if ((r = mod_remove(&cdrmnull_modlinkage)) == 0)
		ddi_soft_state_fini(&cdrmnull_state);
	return (r);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&cdrmnull_modlinkage, modinfop));
}
