/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * GEM buffer objects in the illumos-drm facade.
 *
 * Origin: struct drm_gem_object, struct drm_gem_object_funcs and the
 * functions below follow Linux v7.2's include/drm/drm_gem.h and
 * include/drm/drm_vma_manager.h (both MIT): Linux's names, types and
 * relative order, illumos types where Linux's do not exist. The
 * illumos_* callbacks are illumos additions. This file is a
 * reimplementation for illumos, MIT-licensed.
 */

#ifndef _DRM_GEM_H
#define	_DRM_GEM_H

#include <drm/drm_facade_types.h>
#include <drm/dma_resv.h>
#include <sys/ddidevmap.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* The page range of an object's fake mmap offset. */
struct drm_vma_range {
	uint64_t start;		/* in pages */
	uint64_t size;		/* in pages */
};

/*
 * Linux's struct drm_vma_offset_node, reduced to what drivers read
 * through drm_vma_node_start() and drm_vma_node_offset_addr(). The core
 * fills vm_node when the object gets its offset (drm_gem_create_mmap_offset(),
 * MODE_MAP_DUMB, or a handle's first mapping).
 */
struct drm_vma_offset_node {
	struct drm_vma_range vm_node;
	void *driver_private;
};

static inline uint64_t
drm_vma_node_start(const struct drm_vma_offset_node *node)
{
	return (node->vm_node.start);
}

static inline uint64_t
drm_vma_node_offset_addr(const struct drm_vma_offset_node *node)
{
	return (node->vm_node.start << PAGESHIFT);
}

struct drm_illumos_mapping;
struct drm_illumos_fault;

/*
 * An object's callbacks. Facade version 0 calls free, open and close of
 * Linux's set, and the illumos ones; the others of Linux's table (pin,
 * vmap, mmap, ...) arrive with the movable-object layer.
 *
 * free		frees the driver's structure once the last reference is
 *		gone and the core has released its state. Required. May be
 *		called from a task queue (never from interrupt context).
 * open		`file` is getting its first handle to the object (made,
 *		opened by name or imported). Once per file and object, not
 *		per handle as in Linux: a second handle on the same file
 *		(GEM_OPEN, MODE_GETFB, drm_gem_handle_create()) causes no
 *		call, and PRIME_FD_TO_HANDLE of an object the file holds
 *		returns the existing handle without one. A negative errno
 *		refuses the handle. Called with no lock a driver can see; it
 *		must not create or delete handles of `file`. Optional.
 * close	the last handle of the object on `file` was closed, or the
 *		handle that open was just called for could not be made.
 *		Never while `file` still holds a handle to the object, so
 *		open and close of one file and object strictly alternate
 *		(a driver that counts them, as Linux drivers do, sees the
 *		same balance). Optional.
 * illumos_release
 *		called when the last reference is gone and the core has
 *		released its state, before free, in thread context: the driver
 *		tells its device to stop using the object's memory. Returning
 *		false says the device may still use it (it did not answer);
 *		the core then never frees the object's backings (they are
 *		leaked, with a warning) rather than let the device write into
 *		freed pages. Optional.
 * illumos_umem	the ddi_umem cookie that user mappings of the object map
 *		now. NULL means the system memory drm_gem_object_init()
 *		allocated. Required for objects made with
 *		drm_gem_private_object_init() that can be mapped. An object
 *		whose backing can move sets illumos_movable.
 * illumos_mapped
 *		a user mapping was set up (devmap_map(9E)). Optional.
 * illumos_access
 *		a fault on a mapping that was unloaded with
 *		drm_illumos_mapping_unload() (devmap_access(9E)): return
 *		drm_illumos_fault_load(fault), or an errno to fail it.
 *		Required for movable objects.
 * illumos_movable
 *		mappings may be unloaded and refaulted; the core then
 *		installs devmap callbacks that call illumos_access.
 * illumos_close_every_handle
 *		close is called for every handle a file removes, as Linux
 *		does, not only for the file's last handle to the object (the
 *		default, which pairs it with open). For drivers that drop
 *		per-handle state in close (i915's execbuffer lookup tables).
 */
struct drm_gem_object_funcs {
	void (*free)(struct drm_gem_object *obj);
	int (*open)(struct drm_gem_object *obj, struct drm_file *file);
	void (*close)(struct drm_gem_object *obj, struct drm_file *file);
	/* illumos additions */
	bool (*illumos_release)(struct drm_gem_object *obj);
	ddi_umem_cookie_t (*illumos_umem)(struct drm_gem_object *obj);
	int (*illumos_mapped)(struct drm_gem_object *obj,
	    struct drm_illumos_mapping *map);
	int (*illumos_access)(struct drm_gem_object *obj,
	    struct drm_illumos_mapping *map, struct drm_illumos_fault *fault);
	bool illumos_movable;
	bool illumos_close_every_handle;
};

/*
 * A GEM object. Drivers embed it in their own buffer structure and find
 * that with container_of().
 *
 * Which fields are maintained:
 *	refcount
 *		the object's reference count, the only one (facade 1.0): it
 *		counts every reference, the driver's (drm_gem_object_init(),
 *		drm_gem_object_get(), drm_gem_object_lookup()) and the core's
 *		(handles, names, user mappings, dma-bufs). A driver may read
 *		it (kref_read()) and take a reference with
 *		kref_get_unless_zero() from a list of its own that holds
 *		none, as long as that list's lock keeps the memory valid
 *		(the driver removes the object from the list, under its
 *		lock, in funcs->free); once the count is zero it stays zero
 *		and the object is being freed. The core frees with no RCU
 *		grace period, so a lockless (RCU) lookup is not safe. Every
 *		reference is dropped with drm_gem_object_put().
 *	dev, vma_node, size, import_attach (non-NULL for an object made by
 *	drm_gem_prime_import() of another device's dma-buf), resv (points
 *	to _resv), _resv, gpuva (initialised, unused), funcs.
 *	Not maintained: handle_count, name and dma_buf (the core keeps
 *	them). filp is NULL (illumos objects have no shmem file), lru_node
 *	and lru are unused.
 */
struct drm_gem_object {
	struct kref refcount;
	unsigned int handle_count;
	struct drm_device *dev;
	DRM_FACADE_FILE_T *filp;
	struct drm_vma_offset_node vma_node;
	size_t size;
	int name;
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *import_attach;
	struct dma_resv *resv;
	struct dma_resv _resv;
	struct {
		struct list_head list;
		struct lx_mutex lock;
	} gpuva;
	const struct drm_gem_object_funcs *funcs;
	struct list_head lru_node;
	void *lru;
	/* private: the core's state */
	uint64_t __core[8];
};

/*
 * Initialises an object of `size` bytes (a multiple of the page size)
 * with system memory from ddi_umem_alloc(9F), bound for DMA if the device
 * has DMA attributes (drm_dev_illumos_set_dma_attr()). obj->funcs must be
 * set, with free. The object starts with one reference, the caller's.
 * Returns 0, -EINVAL (size, funcs), -ENOMEM (larger than the device's
 * largest object or its remaining memory, or no memory), or the errno of
 * a failed DMA bind; on failure nothing refers to obj.
 */
extern int drm_gem_object_init(struct drm_device *dev,
    struct drm_gem_object *obj, size_t size);

/*
 * Initialises an object without memory: the driver provides its backing
 * through funcs->illumos_umem. Returns 0 or a negative errno (Linux's
 * returns void; registering the object with the core can fail here).
 */
extern int drm_gem_private_object_init(struct drm_device *dev,
    struct drm_gem_object *obj, size_t size);

/*
 * Linux drivers call this from their free callback. The core has released
 * its state before calling free, so it does nothing in facade version 0.
 */
extern void drm_gem_object_release(struct drm_gem_object *obj);

extern void drm_gem_object_get(struct drm_gem_object *obj);

/*
 * Drops a reference, from a thread that may block or from an interrupt
 * handler (whose last reference is released later by a task queue). The
 * core tells the two apart by servicing_interrupt(), which cannot tell a
 * timeout(9F) handler, quiesce(9E), chpoll(9E) or a thread holding a spin
 * lock from a thread that may block: there, use
 * drm_illumos_gem_object_put_deferred() (drm_illumos.h).
 */
extern void drm_gem_object_put(struct drm_gem_object *obj);

/*
 * Creates a handle for `obj` on `file`, which then holds its own
 * reference. Returns 0 or a negative errno.
 */
extern int drm_gem_handle_create(struct drm_file *file,
    struct drm_gem_object *obj, uint32_t *handlep);

extern int drm_gem_handle_delete(struct drm_file *file, uint32_t handle);

/* The object of a handle with a new reference, or NULL. */
extern struct drm_gem_object *drm_gem_object_lookup(struct drm_file *file,
    uint32_t handle);

/*
 * Calls `fn` with every handle of `file` and its object, in handle order,
 * until `fn` returns non-zero, which is then returned; 0 after the last
 * handle. The replacement for walking Linux's drm_file.object_idr under
 * table_lock (idr_for_each()): the file's handle lock is held throughout,
 * so no handle of `file` is created or closed during the walk (a
 * concurrent GEM_CLOSE or close(2) waits for it), and every object stays
 * valid during its call. `fn` may look handles up (drm_gem_object_lookup())
 * and take references, but must not create or delete handles of `file`.
 * Returns -EINVAL (NULL), -ENODEV, -ENOMEM. Thread context.
 */
extern int drm_illumos_gem_for_each_handle(struct drm_file *file,
    int (*fn)(uint32_t handle, struct drm_gem_object *obj, void *arg),
    void *arg);

/* Whether the object was imported from another device's dma-buf. */
static inline bool
drm_gem_is_imported(const struct drm_gem_object *obj)
{
	return (obj->import_attach != NULL);
}

/* Gives the object its fake mmap offset (vma_node) if it has none yet. */
extern int drm_gem_create_mmap_offset(struct drm_gem_object *obj);

/* MODE_MAP_DUMB's default: the fake offset of a handle's object. */
extern int drm_gem_dumb_map_offset(struct drm_file *file,
    struct drm_device *dev, uint32_t handle, uint64_t *offset);

/*
 * Computes pitch and size of a dumb buffer from width, height and bpp
 * with the core's rules (bpp 8, 16, 24 or 32; flags 0; Linux's limits),
 * rounding the pitch up to `hw_pitch_align` and the size to
 * `hw_size_align` bytes (0 for none; the size is a whole number of pages
 * in any case). Fills args->pitch and args->size. Returns 0 or -EINVAL.
 */
extern int drm_mode_size_dumb(struct drm_device *dev,
    struct drm_mode_create_dumb *args, unsigned long hw_pitch_align,
    unsigned long hw_size_align);

#ifdef	__cplusplus
}
#endif

#endif	/* _DRM_GEM_H */
