/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The DRM side of the LinuxKPI layer: Linux's DRM core functions that a
 * C driver calls and the facade leaves to a compatibility layer, built on
 * the facade (misc/drm_core).
 *
 * Device lifetime: devm_drm_dev_alloc() allocates through the facade and
 * stores the layer's struct device in drm_device.dev (facade 1.1); the
 * driver table the core gets is a copy whose ioctl entries go through
 * lkpi_ioctl_trampoline(), which calls the driver's handler and turns
 * Linux-only error numbers into illumos ones (lkpi_errno()). The copies
 * live until misc/linuxkpi unloads (the core may read the table until the
 * device's last reference is gone, which the layer does not see).
 *
 * Not supported here (explicit failures, see the headers): Linux's mmap
 * offset manager (drm_vma_*), PRIME helpers for driver-made dma-bufs,
 * attaching to other drivers' dma-bufs.
 */

#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/sync_file.h>
#include <drm/drm_auth.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>
#include <drm/drm_vma_manager.h>

#include "lkpi_internal.h"

/* A copy of a driver table with the trampoline in its ioctl entries. */
struct lkpi_drm_driver {
	struct drm_driver drv;
	struct list_head link;
	const struct drm_ioctl_desc *orig;
	int num_ioctls;
	struct drm_ioctl_desc table[];
};

static kmutex_t lkpi_drm_lock;
static LIST_HEAD(lkpi_drm_drivers);

static int
lkpi_ioctl_trampoline(struct drm_device *dev, void *data,
    struct drm_file *file)
{
	const struct lkpi_drm_driver *ld =
	    container_of(dev->driver, struct lkpi_drm_driver, drv);
	unsigned int cmd = drm_illumos_ioctl_cmd();
	int i, ret;

	for (i = 0; i < ld->num_ioctls; i++) {
		if (ld->orig[i].func != NULL && ld->orig[i].cmd == cmd) {
			ret = ld->orig[i].func(dev, data, file);
			return (ret < 0 ? -lkpi_errno(-ret) : ret);
		}
	}
	return (-EINVAL);
}

static struct lkpi_drm_driver *
driver_copy(const struct drm_driver *driver)
{
	struct lkpi_drm_driver *ld;
	int i, n = driver->num_ioctls;

	ld = kzalloc(struct_size(ld, table, n), GFP_KERNEL);
	if (ld == NULL)
		return (NULL);
	ld->drv = *driver;
	ld->orig = driver->ioctls;
	ld->num_ioctls = n;
	for (i = 0; i < n; i++) {
		ld->table[i] = driver->ioctls[i];
		if (ld->table[i].func != NULL)
			ld->table[i].func = lkpi_ioctl_trampoline;
	}
	ld->drv.ioctls = n > 0 ? ld->table : NULL;
	mutex_enter(&lkpi_drm_lock);
	list_add(&ld->link, &lkpi_drm_drivers);
	mutex_exit(&lkpi_drm_lock);
	return (ld);
}

static void
lkpi_drm_dev_put_action(void *data)
{
	drm_dev_put(data);
}

void *
lkpi_devm_drm_dev_alloc(struct device *parent,
    const struct drm_driver *driver, size_t size, size_t offset)
{
	struct lkpi_drm_driver *ld;
	struct drm_device *drm;
	struct inode *inode;
	void *p;

	if (parent == NULL || lkpi_dev_dip(parent) == NULL)
		return (ERR_PTR(-EINVAL));
	ld = driver_copy(driver);
	if (ld == NULL)
		return (ERR_PTR(-ENOMEM));
	p = __drm_dev_alloc(lkpi_dev_dip(parent), &ld->drv, size, offset);
	if (IS_ERR(p))
		return (p);
	drm = (struct drm_device *)((char *)p + offset);
	drm->dev = parent;
	drm->dma_dev = parent;
	/*
	 * Linux drivers reach the device's user mappings through
	 * anon_inode->i_mapping (to revoke them); the mappings are the
	 * core's, so this is an empty address space that
	 * unmap_mapping_range() accepts.
	 */
	inode = drmm_kzalloc(drm, sizeof (*inode), GFP_KERNEL);
	if (inode != NULL) {
		inode->i_mapping = &inode->i_data;
		inode->i_data.host = inode;
		drm->anon_inode = inode;
	}
	if (devm_add_action_or_reset(parent, lkpi_drm_dev_put_action, drm) != 0)
		return (ERR_PTR(-ENOMEM));
	return (p);
}

int
lkpi_drm_dev_register(struct drm_device *dev, unsigned long flags)
{
	int ret = (drm_dev_register)(dev, flags);

	if (ret != 0)
		return (ret);
	/* The core leaves the minors' kdev to the driver (facade 1.1). */
	if (dev->primary != NULL)
		dev->primary->kdev = dev->dev;
	if (dev->render != NULL)
		dev->render->kdev = dev->dev;
	return (0);
}

void
lkpi_drm_dev_unregister(struct drm_device *dev)
{
	int ret = (drm_dev_unregister)(dev);

	/*
	 * Linux's unregister cannot fail. The core refuses while files,
	 * objects or fences of the device are alive, and the device stays
	 * registered; the driver's detach(9E) then fails too.
	 */
	if (ret != 0)
		dev_warn(dev->dev, "drm_dev_unregister: device busy (%d); "
		    "still registered\n", ret);
}

/* --- unplug ----------------------------------------------------------- */

static struct srcu_struct lkpi_drm_unplug_srcu;

bool
drm_dev_enter(struct drm_device *dev, int *idx)
{
	*idx = srcu_read_lock(&lkpi_drm_unplug_srcu);
	if (READ_ONCE(dev->unplugged)) {
		srcu_read_unlock(&lkpi_drm_unplug_srcu, *idx);
		return (false);
	}
	return (true);
}

void
drm_dev_exit(int idx)
{
	srcu_read_unlock(&lkpi_drm_unplug_srcu, idx);
}

void
drm_dev_unplug(struct drm_device *dev)
{
	/*
	 * Marks the device unplugged for drm_dev_enter() and waits for the
	 * sections that entered before; then unregisters it.
	 */
	WRITE_ONCE(dev->unplugged, true);
	synchronize_srcu(&lkpi_drm_unplug_srcu);
	lkpi_drm_dev_unregister(dev);
}

int
drm_dev_wedged_event(struct drm_device *dev, unsigned long method,
    void *info)
{
	/* No uevents on illumos: the event is logged. */
	dev_warn(dev->dev, "device wedged, recovery method(s) 0x%lx\n",
	    method);
	return (0);
}

void
__drmm_mutex_release(struct drm_device *dev, void *res)
{
	mutex_destroy((struct mutex *)res);
}

int
drm_noop(struct drm_device *dev, void *data, struct drm_file *file)
{
	return (0);
}

int
drm_invalid_op(struct drm_device *dev, void *data, struct drm_file *file)
{
	return (-EINVAL);
}

/* --- printers ------------------------------------------------------------ */

void
__drm_printfn_seq_file(struct drm_printer *p, struct va_format *vaf)
{
	seq_vprintf(p->arg, vaf->fmt, *vaf->va);
}

void
__drm_puts_seq_file(struct drm_printer *p, const char *str)
{
	seq_puts(p->arg, str);
}

void
__drm_printfn_line(struct drm_printer *p, struct va_format *vaf)
{
	struct drm_printer *dp = p->arg;
	char buf[256];

	(void) vsnprintf(buf, sizeof (buf), vaf->fmt, *vaf->va);
	if (p->line.series != 0)
		drm_printf(dp, "%s %u.%u: %s", p->prefix != NULL ? p->prefix :
		    "", p->line.series, ++p->line.counter, buf);
	else
		drm_printf(dp, "%s %u: %s", p->prefix != NULL ? p->prefix :
		    "", ++p->line.counter, buf);
}

void
drm_print_bits(struct drm_printer *p, unsigned long value,
    const char * const bits[], unsigned int nbits)
{
	bool first = true;
	unsigned int i;

	if (value == 0) {
		drm_printf(p, "(none)");
		return;
	}
	for (i = 0; i < nbits && i < BITS_PER_LONG; i++) {
		if (!(value & (1UL << i)))
			continue;
		drm_printf(p, "%s%s", first ? "" : ",",
		    bits[i] != NULL ? bits[i] : "?");
		first = false;
	}
}

void
drm_print_hex_dump(struct drm_printer *p, const char *prefix, const u8 *buf,
    size_t len)
{
	char line[16 * 3 + 1];
	size_t i;

	for (i = 0; i < len; i += 16) {
		(void) hex_dump_to_buffer(buf + i, min_t(size_t, 16, len - i),
		    16, 1, line, sizeof (line), false);
		drm_printf(p, "%s%s\n", prefix != NULL ? prefix : "", line);
	}
}

/* --- GEM ------------------------------------------------------------------ */

void
drm_gem_object_free(struct kref *kref)
{
	/* The facade's drm_gem_object_put() frees objects; never reached. */
	WARN(1, "drm_gem_object_free: GEM objects are freed by the DRM core");
}

void
drm_gem_free_mmap_offset(struct drm_gem_object *obj)
{
	/* The object's offset is the core's and goes with the object. */
	(void) obj;
}

/* --- mmap offsets: the core's ------------------------------------------ */

int
drm_vma_offset_add(struct drm_vma_offset_manager *mgr,
    struct drm_vma_offset_node *node, unsigned long pages)
{
	return (-ENODEV);
}

void
drm_vma_offset_remove(struct drm_vma_offset_manager *mgr,
    struct drm_vma_offset_node *node)
{
}

struct drm_vma_offset_node *
drm_vma_offset_lookup_locked(struct drm_vma_offset_manager *mgr,
    unsigned long start, unsigned long pages)
{
	return (NULL);
}

void
drm_vma_offset_lock_lookup(struct drm_vma_offset_manager *mgr)
{
}

void
drm_vma_offset_unlock_lookup(struct drm_vma_offset_manager *mgr)
{
}

int
drm_vma_node_allow(struct drm_vma_offset_node *node, struct drm_file *tag)
{
	return (-ENODEV);
}

int
drm_vma_node_allow_once(struct drm_vma_offset_node *node,
    struct drm_file *tag)
{
	return (-ENODEV);
}

void
drm_vma_node_revoke(struct drm_vma_offset_node *node, struct drm_file *tag)
{
}

bool
drm_vma_node_is_allowed(struct drm_vma_offset_node *node,
    struct drm_file *tag)
{
	return (false);
}

/* --- PRIME: the core's ------------------------------------------------- */

struct dma_buf *
drm_gem_dmabuf_export(struct drm_device *dev,
    struct dma_buf_export_info *exp_info)
{
	return (ERR_PTR(-ENODEV));
}

void
drm_gem_dmabuf_release(struct dma_buf *dma_buf)
{
}

int
drm_gem_prime_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	return (-ENODEV);
}

struct sg_table *
drm_gem_map_dma_buf(struct dma_buf_attachment *attach,
    enum dma_data_direction dir)
{
	return (ERR_PTR(-ENODEV));
}

void
drm_gem_unmap_dma_buf(struct dma_buf_attachment *attach,
    struct sg_table *sgt, enum dma_data_direction dir)
{
}

/* An imported object's import state is the core's; nothing to undo. */
void
drm_prime_gem_destroy(struct drm_gem_object *obj, struct sg_table *sg)
{
}

int
drm_gem_map_attach(struct dma_buf *dma_buf, struct dma_buf_attachment *attach)
{
	return (-ENODEV);
}

void
drm_gem_map_detach(struct dma_buf *dma_buf, struct dma_buf_attachment *attach)
{
}

/* --- dma-buf attachments: not supported -------------------------------- */

struct dma_buf *
dma_buf_export(const struct dma_buf_export_info *exp_info)
{
	return (ERR_PTR(-ENODEV));
}

struct dma_buf_attachment *
dma_buf_attach(struct dma_buf *dmabuf, struct device *dev)
{
	return (ERR_PTR(-ENODEV));
}

struct dma_buf_attachment *
dma_buf_dynamic_attach(struct dma_buf *dmabuf, struct device *dev,
    const struct dma_buf_attachment_ops *importer_ops, void *importer_priv)
{
	return (ERR_PTR(-ENODEV));
}

void
dma_buf_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
}

struct sg_table *
dma_buf_map_attachment(struct dma_buf_attachment *attach,
    enum dma_data_direction dir)
{
	return (ERR_PTR(-ENODEV));
}

struct sg_table *
dma_buf_map_attachment_unlocked(struct dma_buf_attachment *attach,
    enum dma_data_direction dir)
{
	return (ERR_PTR(-ENODEV));
}

void
dma_buf_unmap_attachment(struct dma_buf_attachment *attach,
    struct sg_table *sgt, enum dma_data_direction dir)
{
}

void
dma_buf_unmap_attachment_unlocked(struct dma_buf_attachment *attach,
    struct sg_table *sgt, enum dma_data_direction dir)
{
}

int
dma_buf_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	return (-ENODEV);
}

void
dma_buf_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
}

int
dma_buf_pin(struct dma_buf_attachment *attach)
{
	return (-ENODEV);
}

void
dma_buf_unpin(struct dma_buf_attachment *attach)
{
}

/*
 * A sync file for a fence. The descriptor is made by the core
 * (drm_sync_file_install()); a driver installs it with fd_install(),
 * which the layer cannot provide, so i915's out-fence path uses
 * drm_sync_file_install() on illumos (patch series) and this returns a
 * holder of the fence only.
 */
struct sync_file *
sync_file_create(struct dma_fence *fence)
{
	struct sync_file *sf = kzalloc(sizeof (*sf), GFP_KERNEL);

	if (sf == NULL)
		return (NULL);
	sf->fence = dma_fence_get(fence);
	return (sf);
}

/* --- module state ------------------------------------------------------ */

int
lkpi_drm_init(void)
{
	(mutex_init)(&lkpi_drm_lock, NULL, MUTEX_DRIVER, NULL);
	return (init_srcu_struct(&lkpi_drm_unplug_srcu));
}

void
lkpi_drm_fini(void)
{
	struct lkpi_drm_driver *ld, *n;

	cleanup_srcu_struct(&lkpi_drm_unplug_srcu);
	list_for_each_entry_safe(ld, n, &lkpi_drm_drivers, link) {
		list_del(&ld->link);
		kfree(ld);
	}
	(mutex_destroy)(&lkpi_drm_lock);
}
