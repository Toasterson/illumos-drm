/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The illumos module of Linux's i915 driver (headless build): the kernel
 * module "i915n" (the name G7 reserved for this stack's Intel driver; its
 * package ships the driver without PCI aliases and `drmadm select new`
 * moves the Intel GPU aliases to it).
 *
 * _init checks the DRM facade and the LinuxKPI layer it was built
 * against, runs i915's module_init (which registers its struct pci_driver
 * with the layer) and installs the module. attach(9E) and detach(9E) are
 * the layer's PCI glue with i915's driver; the character device is the
 * DRM core's (DRM_CB_OPS). _fini removes the module and runs i915's
 * module_exit.
 *
 * Also here: the GEM handle lookups patch 0003 routes to the DRM core,
 * and the user mappings of patch 0005.
 */

#include <linux/pci.h>
#include <linux/rcupdate.h>
#include <linux/shmem_fs.h>
#include <lkpi/version.h>
#include <drm/drm_facade.h>

#include "i915_drv.h"
#include "gem/i915_gem_object.h"

extern int lkpi_module_init(void);
extern void lkpi_module_exit(void);

static struct pci_driver *i915n_pci_driver;

static int
i915n_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	if (i915n_pci_driver == NULL)
		return (DDI_FAILURE);
	return (lkpi_pci_attach(dip, cmd, i915n_pci_driver));
}

static int
i915n_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	if (i915n_pci_driver == NULL)
		return (DDI_FAILURE);
	return (lkpi_pci_detach(dip, cmd, i915n_pci_driver));
}

static struct cb_ops i915n_cb_ops = DRM_CB_OPS;

static struct dev_ops i915n_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_refcnt = 0,
	.devo_getinfo = drm_getinfo,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = i915n_attach,
	.devo_detach = i915n_detach,
	.devo_reset = nodev,
	.devo_cb_ops = &i915n_cb_ops,
	.devo_bus_ops = NULL,
	.devo_power = NULL,
	/*
	 * The GPU keeps running whatever i915 left it doing; fast reboot
	 * is refused rather than handing over a busy GPU.
	 */
	.devo_quiesce = ddi_quiesce_not_supported,
};

static struct modldrv i915n_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = "Intel GPU (Linux i915, headless)",
	.drv_dev_ops = &i915n_dev_ops,
};

static struct modlinkage i915n_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &i915n_modldrv, NULL },
};

int
_init(void)
{
	int ret;

	if ((ret = drm_facade_check("i915n")) != 0)
		return (ret);
	if ((ret = linuxkpi_check("i915n", LINUXKPI_ABI)) != 0)
		return (ret);
	ret = lkpi_module_init();
	if (ret != 0) {
		cmn_err(CE_WARN, "i915n: i915 module init failed (%d)", ret);
		return (EINVAL);
	}
	i915n_pci_driver = lkpi_pci_driver_lookup("i915");
	if (i915n_pci_driver == NULL) {
		cmn_err(CE_WARN, "i915n: i915 registered no PCI driver");
		lkpi_module_exit();
		return (EINVAL);
	}
	if ((ret = mod_install(&i915n_modlinkage)) != 0) {
		i915n_pci_driver = NULL;
		lkpi_module_exit();
	}
	return (ret);
}

int
_fini(void)
{
	int ret;

	if ((ret = mod_remove(&i915n_modlinkage)) != 0)
		return (ret);
	i915n_pci_driver = NULL;
	lkpi_module_exit();
	return (0);
}

int
_info(struct modinfo *mi)
{
	return (mod_info(&i915n_modlinkage, mi));
}

/* --- GEM handle lookups (patch 0003) ------------------------------------ */

struct i915n_deferred_put {
	struct rcu_head rcu;
	struct drm_gem_object *obj;
};

static void
i915n_put_after_gp(struct rcu_head *rcu)
{
	struct i915n_deferred_put *d =
	    container_of(rcu, struct i915n_deferred_put, rcu);

	drm_gem_object_put(d->obj);
	kfree(d);
}

/*
 * i915 looks a handle up under rcu_read_lock() and uses the object until
 * rcu_read_unlock() without a reference. The DRM core's lookup returns a
 * reference; dropping it after a grace period keeps the object alive for
 * the caller's read section. Without memory for the deferral the lookup
 * finds nothing (the caller reports -ENOENT, as for a closed handle).
 */
struct drm_i915_gem_object *
i915_gem_object_lookup_rcu(struct drm_file *file, u32 handle)
{
	struct i915n_deferred_put *d;
	struct drm_gem_object *obj;

	d = kmalloc(sizeof (*d), GFP_KERNEL);
	if (d == NULL)
		return (NULL);
	obj = drm_gem_object_lookup(file, handle);
	if (obj == NULL) {
		kfree(d);
		return (NULL);
	}
	d->obj = obj;
	call_rcu(&d->rcu, i915n_put_after_gp);
	return (to_intel_bo(obj));
}

bool
i915_illumos_handle_is(struct drm_file *file, u32 handle,
    struct drm_i915_gem_object *obj)
{
	struct drm_gem_object *o = drm_gem_object_lookup(file, handle);
	bool same = (o == &obj->base);

	if (o != NULL)
		drm_gem_object_put(o);
	return (same);
}

/* --- user mappings (patch 0005) ----------------------------------------- */

/*
 * The memory the DRM core maps for the object: the shmem file's (one
 * ddi_umem allocation, which stays put while the file lives). Internal,
 * stolen and imported objects have none; i915_illumos_mmap_offset() never
 * gives them an offset, so the core never asks for them.
 */
ddi_umem_cookie_t
i915_illumos_gem_umem(struct drm_gem_object *obj)
{
	if (obj->filp == NULL)
		return (NULL);
	return (lkpi_shmem_umem(obj->filp, NULL, NULL));
}

/*
 * DRM_IOCTL_I915_GEM_MMAP_OFFSET on illumos: the object's single core
 * offset, which the core maps write-back. A write-back request always
 * gets it (user space flushes through set_domain as on Linux). WC and UC
 * requests get it only where a write-back mapping behaves the same for
 * the GPU: on a shared-LLC platform (gen7 and gen9.5 integrated GPUs)
 * for an object the CPU cache is coherent with for reads and writes.
 * GTT mappings (through the aperture, with fences) are not supported yet.
 * Called with the object locked.
 */
int
i915_illumos_mmap_offset(struct drm_i915_gem_object *obj,
    enum i915_mmap_type mmap_type, u64 *offset)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	const unsigned int coherent = I915_BO_CACHE_COHERENT_FOR_READ |
	    I915_BO_CACHE_COHERENT_FOR_WRITE;
	int err;

	if (obj->base.filp == NULL)
		return (-ENODEV);
	switch (mmap_type) {
	case I915_MMAP_TYPE_WB:
		break;
	case I915_MMAP_TYPE_WC:
	case I915_MMAP_TYPE_UC:
		if (!HAS_LLC(i915) ||
		    (obj->cache_coherent & coherent) != coherent)
			return (-ENODEV);
		break;
	default:
		return (-ENODEV);
	}
	err = drm_gem_create_mmap_offset(&obj->base);
	if (err != 0)
		return (err);
	*offset = drm_vma_node_offset_addr(&obj->base.vma_node);
	return (0);
}

