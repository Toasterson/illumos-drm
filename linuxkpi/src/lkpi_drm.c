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

/*
 * A copy of a driver table per DRM device, with the trampoline in its
 * ioctl entries and the layer's release, and the device's detach state.
 * It lives until the device's release has run; the next copy (or unload)
 * frees released ones.
 */
struct lkpi_drm_driver {
	struct drm_driver drv;
	struct list_head link;
	const struct drm_ioctl_desc *orig;
	void (*orig_release)(struct drm_device *dev);
	struct device *parent;		/* referenced until release */
	struct drm_device *drm;
	bool stuck;			/* the core refused to unregister */
	bool released;
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

/*
 * The device's release (its last drm_dev_put()): the driver's release,
 * then the reference on the parent device the allocation took.
 */
static void
lkpi_drm_release(struct drm_device *dev)
{
	struct lkpi_drm_driver *ld =
	    container_of(dev->driver, struct lkpi_drm_driver, drv);

	if (ld->orig_release != NULL)
		ld->orig_release(dev);
	put_device(ld->parent);
	/*
	 * The copy is marked released by the put that ends here
	 * (lkpi_drm_dev_put_action()), once the core no longer refers to
	 * dev->driver.
	 */
}

/* Frees the copies of released devices; lkpi_drm_lock held. */
static void
driver_reap_locked(void)
{
	struct lkpi_drm_driver *ld, *n;

	list_for_each_entry_safe(ld, n, &lkpi_drm_drivers, link) {
		if (ld->released) {
			list_del(&ld->link);
			kfree(ld);
		}
	}
}

static void
driver_free(struct lkpi_drm_driver *ld)
{
	mutex_enter(&lkpi_drm_lock);
	list_del(&ld->link);
	mutex_exit(&lkpi_drm_lock);
	kfree(ld);
}

static struct lkpi_drm_driver *
driver_copy(const struct drm_driver *driver, struct device *parent)
{
	struct lkpi_drm_driver *ld;
	int i, n = driver->num_ioctls;

	ld = kzalloc(struct_size(ld, table, n), GFP_KERNEL);
	if (ld == NULL)
		return (NULL);
	ld->drv = *driver;
	ld->orig_release = driver->release;
	ld->drv.release = lkpi_drm_release;
	ld->parent = parent;
	ld->orig = driver->ioctls;
	ld->num_ioctls = n;
	for (i = 0; i < n; i++) {
		ld->table[i] = driver->ioctls[i];
		if (ld->table[i].func != NULL)
			ld->table[i].func = lkpi_ioctl_trampoline;
	}
	ld->drv.ioctls = n > 0 ? ld->table : NULL;
	mutex_enter(&lkpi_drm_lock);
	driver_reap_locked();
	list_add(&ld->link, &lkpi_drm_drivers);
	mutex_exit(&lkpi_drm_lock);
	return (ld);
}

/* The live copy of the DRM device whose parent is `parent`, or NULL. */
static struct lkpi_drm_driver *
driver_of_parent(struct device *parent)
{
	struct lkpi_drm_driver *ld, *found = NULL;

	mutex_enter(&lkpi_drm_lock);
	list_for_each_entry(ld, &lkpi_drm_drivers, link) {
		if (ld->parent == parent && ld->drm != NULL && !ld->released) {
			found = ld;
			break;
		}
	}
	mutex_exit(&lkpi_drm_lock);
	return (found);
}

/*
 * The devres action of devm_drm_dev_alloc(), after the driver's remove()
 * (or a failed probe): the driver's reference goes. The device was
 * unplugged (detach checked that user space no longer holds it; a failed
 * probe ran before anyone could open it), so the core runs the driver's
 * release, which frees the driver's own GEM objects, and then unregisters
 * and frees the device. If the core still finds state (fences that never
 * signal), it keeps the device, leaked and unreachable, and detach fails
 * (lkpi_drm_detach_end()), so that the driver's text stays loaded.
 */
static void
lkpi_drm_dev_put_action(void *data)
{
	struct drm_device *drm = data;
	struct lkpi_drm_driver *ld =
	    container_of(drm->driver, struct lkpi_drm_driver, drv);
	dev_info_t *dip = lkpi_dev_dip(ld->parent);

	if (drm->registered && (drm_dev_illumos_unplug)(drm) != 0)
		dev_err(drm->dev, "user space still holds the DRM device\n");
	if ((drm_dev_illumos_put)(drm) != 0) {
		/* drm is not freed: the core keeps it. */
		if (dip != NULL)
			(dev_err)(dip, CE_WARN, "the DRM core keeps the device: "
			    "fences or objects remain; the device stays "
			    "attached");
		mutex_enter(&lkpi_drm_lock);
		ld->stuck = true;
		mutex_exit(&lkpi_drm_lock);
		return;
	}
	mutex_enter(&lkpi_drm_lock);
	ld->released = true;
	mutex_exit(&lkpi_drm_lock);
}

/*
 * detach(9E) of a device, before the driver's remove(): refuses while
 * user space holds the DRM device (open files, descriptors, mappings);
 * otherwise no new open reaches it. 0 for a device without one.
 */
int
lkpi_drm_detach_begin(struct device *dev)
{
	struct lkpi_drm_driver *ld = driver_of_parent(dev);

	if (ld == NULL || !ld->drm->registered)
		return (0);
	return ((drm_dev_illumos_unplug)(ld->drm) != 0 ? -EBUSY : 0);
}

/* detach(9E), after remove() and devres: -EBUSY if the core kept it. */
int
lkpi_drm_detach_end(struct device *dev)
{
	struct lkpi_drm_driver *ld;
	int ret = 0;

	mutex_enter(&lkpi_drm_lock);
	list_for_each_entry(ld, &lkpi_drm_drivers, link) {
		if (ld->parent == dev && ld->stuck && !ld->released)
			ret = -EBUSY;
	}
	mutex_exit(&lkpi_drm_lock);
	return (ret);
}

static void lkpi_drm_set_kdev(struct drm_device *dev);

void *
lkpi_devm_drm_dev_alloc(struct device *parent,
    const struct drm_driver *driver, size_t size, size_t offset)
{
	struct lkpi_drm_driver *ld;
	struct drm_device *drm;
	struct inode *inode;
	void *p;
	int ret;

	if (parent == NULL || lkpi_dev_dip(parent) == NULL)
		return (ERR_PTR(-EINVAL));
	ld = driver_copy(driver, parent);
	if (ld == NULL)
		return (ERR_PTR(-ENOMEM));
	p = __drm_dev_alloc(lkpi_dev_dip(parent), &ld->drv, size, offset);
	if (IS_ERR(p)) {
		driver_free(ld);
		return (p);
	}
	/* The device structure may outlive detach: it holds its parent. */
	get_device(parent);
	drm = (struct drm_device *)((char *)p + offset);
	ld->drm = drm;
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
	/*
	 * The core's GEM, fence and reservation state exists from
	 * registration on, and Linux drivers make GEM objects long before
	 * their drm_dev_register() (i915's GGTT scratch page, its kernel
	 * contexts). So the device is registered here, in attach(9E): its
	 * nodes exist from now on, but opens wait until attach returns, and
	 * the driver's drm_dev_register() then only completes the minors.
	 */
	if ((ret = (drm_dev_register)(drm, 0)) != 0)
		return (ERR_PTR(ret));
	lkpi_drm_set_kdev(drm);
	return (p);
}

/* The core leaves the minors' kdev to the driver (facade 1.1). */
static void
lkpi_drm_set_kdev(struct drm_device *dev)
{
	if (dev->primary != NULL)
		dev->primary->kdev = dev->dev;
	if (dev->render != NULL)
		dev->render->kdev = dev->dev;
}

int
lkpi_drm_dev_register(struct drm_device *dev, unsigned long flags)
{
	int ret;

	/* A devm_drm_dev_alloc() device is registered already (above). */
	if (!dev->registered && (ret = (drm_dev_register)(dev, flags)) != 0)
		return (ret);
	lkpi_drm_set_kdev(dev);
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

/* --- messages: formatted here, logged by the core ---------------------- */

#undef	drm_illumos_vprintk
#undef	__drm_printfn_info
#undef	__drm_printfn_dbg
#undef	__drm_printfn_err
extern void drm_illumos_vprintk(dev_info_t *, const char *, int, const char *,
    struct va_format *);
extern void __drm_printfn_info(struct drm_printer *, struct va_format *);
extern void __drm_printfn_dbg(struct drm_printer *, struct va_format *);
extern void __drm_printfn_err(struct drm_printer *, struct va_format *);

#define	LKPI_DRM_MSG	256

/*
 * Formats `vaf` with the layer's vsnprintf into `sbuf`, or into a larger
 * allocation (returned; the caller frees it when it is not sbuf) for a
 * longer message; without memory the message is cut. Any context.
 */
static char *
lkpi_drm_format(char *sbuf, size_t ssz, struct va_format *vaf)
{
	va_list ap;
	char *big;
	int n;

	va_copy(ap, *vaf->va);
	n = lkpi_vsnprintf(sbuf, ssz, vaf->fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n < ssz)
		return (sbuf);
	big = kmalloc((size_t)n + 1, GFP_NOWAIT | __GFP_NOWARN);
	if (big == NULL)
		return (sbuf);
	va_copy(ap, *vaf->va);
	(void) lkpi_vsnprintf(big, (size_t)n + 1, vaf->fmt, ap);
	va_end(ap);
	return (big);
}

static void
lkpi_drm_text(void (*fn)(struct drm_printer *, struct va_format *),
    struct drm_printer *p, const char *fmt, ...)
{
	struct va_format vaf;
	va_list ap;

	va_start(ap, fmt);
	vaf.fmt = fmt;
	vaf.va = &ap;
	fn(p, &vaf);
	va_end(ap);
}

static void
lkpi_drm_vprintk_text(dev_info_t *dev, const char *level, int category,
    const char *prefix, const char *fmt, ...)
{
	struct va_format vaf;
	va_list ap;

	va_start(ap, fmt);
	vaf.fmt = fmt;
	vaf.va = &ap;
	drm_illumos_vprintk(dev, level, category, prefix, &vaf);
	va_end(ap);
}

/* Whether a debug message of `category` would be dropped by the core. */
static bool
lkpi_drm_dbg_off(const char *level, int category)
{
	return (level != NULL && strcmp(level, KERN_DEBUG) == 0 &&
	    category >= 0 && category < 32 &&
	    (drm_debug & (1U << category)) == 0);
}

void
lkpi_drm_vprintk(dev_info_t *dev, const char *level, int category,
    const char *prefix, struct va_format *vaf)
{
	char sbuf[LKPI_DRM_MSG], *msg;

	if (lkpi_drm_dbg_off(level, category))
		return;
	msg = lkpi_drm_format(sbuf, sizeof (sbuf), vaf);
	lkpi_drm_vprintk_text(dev, level, category, prefix, "%s", msg);
	if (msg != sbuf)
		kfree(msg);
}

static void
lkpi_drm_printfn(void (*fn)(struct drm_printer *, struct va_format *),
    struct drm_printer *p, struct va_format *vaf)
{
	char sbuf[LKPI_DRM_MSG], *msg;

	msg = lkpi_drm_format(sbuf, sizeof (sbuf), vaf);
	lkpi_drm_text(fn, p, "%s", msg);
	if (msg != sbuf)
		kfree(msg);
}

void
lkpi_drm_printfn_info(struct drm_printer *p, struct va_format *vaf)
{
	lkpi_drm_printfn(__drm_printfn_info, p, vaf);
}

void
lkpi_drm_printfn_dbg(struct drm_printer *p, struct va_format *vaf)
{
	if (lkpi_drm_dbg_off(KERN_DEBUG, (int)p->category))
		return;
	lkpi_drm_printfn(__drm_printfn_dbg, p, vaf);
}

void
lkpi_drm_printfn_err(struct drm_printer *p, struct va_format *vaf)
{
	lkpi_drm_printfn(__drm_printfn_err, p, vaf);
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

/*
 * Marks the device unplugged for drm_dev_enter(), waits for the sections
 * that entered before, and keeps new opens away from it. The core
 * unregisters it after the driver's remove() (lkpi_drm_dev_put_action()),
 * once the driver's own GEM objects are gone; detach(9E) has checked that
 * user space no longer holds it (lkpi_drm_detach_begin()).
 */
void
drm_dev_unplug(struct drm_device *dev)
{
	WRITE_ONCE(dev->unplugged, true);
	synchronize_srcu(&lkpi_drm_unplug_srcu);
	if (dev->registered && (drm_dev_illumos_unplug)(dev) != 0)
		dev_warn(dev->dev, "drm_dev_unplug: user space still holds "
		    "the device\n");
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
	const char *prefix = p->prefix != NULL ? p->prefix : "";
	const char *pad = p->prefix != NULL ? " " : "";

	/* %pV: the message is formatted by the printer it goes to. */
	if (p->line.series != 0)
		drm_printf(dp, "%s%s%u.%u: %pV", prefix, pad, p->line.series,
		    ++p->line.counter, vaf);
	else
		drm_printf(dp, "%s%s%u: %pV", prefix, pad, ++p->line.counter,
		    vaf);
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

	lkpi_resv_fini();
	cleanup_srcu_struct(&lkpi_drm_unplug_srcu);
	list_for_each_entry_safe(ld, n, &lkpi_drm_drivers, link) {
		list_del(&ld->link);
		kfree(ld);
	}
	(mutex_destroy)(&lkpi_drm_lock);
}
