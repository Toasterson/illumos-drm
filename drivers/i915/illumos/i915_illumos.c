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
 * the user mappings of patch 0005, and the display-engine interrupt
 * registers that patch 0006 needs without the display code.
 */

#include <linux/pci.h>
#include <linux/rcupdate.h>
#include <linux/shmem_fs.h>
#include <lkpi/version.h>
#include <drm/drm_facade.h>

#include "i915_drv.h"
#include "i915_irq.h"
#include "intel_uncore.h"
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
		lkpi_module_quiesce(&i915n_modlinkage);
		return (EINVAL);
	}
	i915n_pci_driver = lkpi_pci_driver_lookup("i915");
	if (i915n_pci_driver == NULL) {
		cmn_err(CE_WARN, "i915n: i915 registered no PCI driver");
		lkpi_module_exit();
		lkpi_module_quiesce(&i915n_modlinkage);
		return (EINVAL);
	}
	if ((ret = mod_install(&i915n_modlinkage)) != 0) {
		i915n_pci_driver = NULL;
		lkpi_module_exit();
		lkpi_module_quiesce(&i915n_modlinkage);
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
	/* Nothing of the layer may call into this module's text any more. */
	lkpi_module_quiesce(&i915n_modlinkage);
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
 * rcu_read_unlock() without a reference (busy, get_tiling, get_caching;
 * the other lookups take a reference directly, patch 0010). The DRM
 * core's lookup returns a reference; dropping it after a grace period
 * keeps the object alive for the caller's read section. Inside the read
 * section nothing may wait for memory: without it the lookup finds
 * nothing (the caller reports -ENOENT, as for a closed handle).
 */
struct drm_i915_gem_object *
i915_gem_object_lookup_rcu(struct drm_file *file, u32 handle)
{
	struct i915n_deferred_put *d;
	struct drm_gem_object *obj;

	d = kmalloc(sizeof (*d), GFP_NOWAIT | __GFP_NOWARN);
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

/* i915_gem_object_lookup() (patch 0010): the core's lookup, a reference. */
struct drm_i915_gem_object *
i915_illumos_object_lookup(struct drm_file *file, u32 handle)
{
	struct drm_gem_object *obj = drm_gem_object_lookup(file, handle);

	return (obj != NULL ? to_intel_bo(obj) : NULL);
}

/*
 * Whether `handle` still names `obj` (execbuffer's __eb_add_lut(), with
 * obj->lut_lock held, patch 0003). The core's lookup takes the file's
 * handle-table lock under lut_lock, and the put here may be the last
 * one. Both are safe: lut_lock is an adaptive mutex in this layer, the
 * core never takes lut_lock (i915's close callback takes it under the
 * file's hook lock, which lookups never take), so the order lut_lock ->
 * table lock has no cycle; and the caller holds its own reference to obj,
 * so this put is never the last of obj.
 */
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
		/*
		 * The write-back mapping stands for WC/UC only while the
		 * object stays coherent: patch 0008 refuses SET_CACHING to a
		 * non-coherent level from now on.
		 */
		set_bit(I915_BO_ILLUMOS_WB_FOR_WC_BIT, &obj->flags);
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

/* --- display-engine interrupt registers (patch 0006) --------------------- */

/*
 * On gen5 to gen7 (ILK..HSW) the master enable of every interrupt, the
 * GT's included, is DE_MASTER_IRQ_CONTROL in DEIER, a display-engine
 * register; on gen8+ the display engine has interrupt registers of its
 * own that must be quiet when the master enable is set. The display code
 * that owns them is not built (headless): these keep the display sources
 * masked and disabled, and run the master enable as i915's display code
 * would. Register offsets as in Linux's display/intel_display_regs.h.
 */
#define	ILLUMOS_DEIMR		_MMIO(0x44004)
#define	ILLUMOS_DEIIR		_MMIO(0x44008)
#define	ILLUMOS_DEIER		_MMIO(0x4400c)
#define	ILLUMOS_GEN7_ERR_INT	_MMIO(0x44040)
#define	ILLUMOS_SDEIMR		_MMIO(0xc4004)
#define	ILLUMOS_SDEIIR		_MMIO(0xc4008)
#define	ILLUMOS_SDEIER		_MMIO(0xc400c)
#define	ILLUMOS_SERR_INT	_MMIO(0xc4040)
#define	ILLUMOS_DE_MASTER	(1U << 31)
#define	ILLUMOS_GEN8_DE_PIPE_A_IMR	_MMIO(0x44404)
#define	ILLUMOS_GEN8_DE_PIPE_A_IIR	_MMIO(0x44408)
#define	ILLUMOS_GEN8_DE_PIPE_A_IER	_MMIO(0x4440c)
#define	ILLUMOS_GEN8_DE_PORT_IMR	_MMIO(0x44444)
#define	ILLUMOS_GEN8_DE_PORT_IIR	_MMIO(0x44448)
#define	ILLUMOS_GEN8_DE_PORT_IER	_MMIO(0x4444c)
#define	ILLUMOS_GEN8_DE_MISC_IMR	_MMIO(0x44464)
#define	ILLUMOS_GEN8_DE_MISC_IIR	_MMIO(0x44468)
#define	ILLUMOS_GEN8_DE_MISC_IER	_MMIO(0x4446c)

static struct i915_irq_regs
illumos_irq_regs(i915_reg_t imr, i915_reg_t ier, i915_reg_t iir)
{
	struct i915_irq_regs r = { .imr = imr, .ier = ier, .iir = iir };

	return (r);
}

/* The south display engine (PCH) exists on every platform these run on. */
static bool
illumos_has_pch(struct drm_i915_private *i915)
{
	return (!IS_VALLEYVIEW(i915) && !IS_CHERRYVIEW(i915) &&
	    !IS_GEN9_LP(i915));
}

/* ilk_irq_reset(): every display source masked, disabled and cleared. */
void
i915_illumos_de_irq_reset(struct drm_i915_private *i915)
{
	struct intel_uncore *uncore = &i915->uncore;

	gen2_irq_reset(uncore, illumos_irq_regs(ILLUMOS_DEIMR, ILLUMOS_DEIER,
	    ILLUMOS_DEIIR));
	if (GRAPHICS_VER(i915) == 7)
		intel_uncore_write(uncore, ILLUMOS_GEN7_ERR_INT, 0xffffffff);
	if (illumos_has_pch(i915)) {
		gen2_irq_reset(uncore, illumos_irq_regs(ILLUMOS_SDEIMR,
		    ILLUMOS_SDEIER, ILLUMOS_SDEIIR));
		intel_uncore_write(uncore, ILLUMOS_SERR_INT, 0xffffffff);
	}
}

/*
 * ilk_irq_postinstall(): the display sources stay masked; DEIER gets the
 * master enable only, which lets the GT and PM interrupts through.
 */
void
i915_illumos_de_irq_postinstall(struct drm_i915_private *i915)
{
	gen2_irq_init(&i915->uncore, illumos_irq_regs(ILLUMOS_DEIMR,
	    ILLUMOS_DEIER, ILLUMOS_DEIIR), 0xffffffff, ILLUMOS_DE_MASTER);
}

/* ilk_irq_handler(): the master enable off while the sources are read. */
void
i915_illumos_de_master_disable(struct drm_i915_private *i915, u32 *de_ier,
    u32 *sde_ier)
{
	struct intel_uncore *uncore = &i915->uncore;

	*de_ier = intel_uncore_read_fw(uncore, ILLUMOS_DEIER);
	intel_uncore_write_fw(uncore, ILLUMOS_DEIER,
	    *de_ier & ~ILLUMOS_DE_MASTER);
	*sde_ier = 0;
	if (illumos_has_pch(i915)) {
		*sde_ier = intel_uncore_read_fw(uncore, ILLUMOS_SDEIER);
		intel_uncore_write_fw(uncore, ILLUMOS_SDEIER, 0);
	}
}

void
i915_illumos_de_master_enable(struct drm_i915_private *i915, u32 de_ier,
    u32 sde_ier)
{
	struct intel_uncore *uncore = &i915->uncore;

	intel_uncore_write_fw(uncore, ILLUMOS_DEIER, de_ier);
	if (sde_ier != 0)
		intel_uncore_write_fw(uncore, ILLUMOS_SDEIER, sde_ier);
}

/*
 * gen8_irq_reset(): the gen8+ display interrupt registers quiet, so that
 * nothing the firmware left armed reaches the master interrupt. Pipe A
 * is in the always-on power well; the other pipes' registers are only
 * reachable with their power wells on (display code, not built), and the
 * firmware does not arm their interrupts.
 */
void
i915_illumos_gen8_de_irq_reset(struct drm_i915_private *i915)
{
	struct intel_uncore *uncore = &i915->uncore;

	gen2_irq_reset(uncore, illumos_irq_regs(ILLUMOS_GEN8_DE_PIPE_A_IMR,
	    ILLUMOS_GEN8_DE_PIPE_A_IER, ILLUMOS_GEN8_DE_PIPE_A_IIR));
	gen2_irq_reset(uncore, illumos_irq_regs(ILLUMOS_GEN8_DE_PORT_IMR,
	    ILLUMOS_GEN8_DE_PORT_IER, ILLUMOS_GEN8_DE_PORT_IIR));
	gen2_irq_reset(uncore, illumos_irq_regs(ILLUMOS_GEN8_DE_MISC_IMR,
	    ILLUMOS_GEN8_DE_MISC_IER, ILLUMOS_GEN8_DE_MISC_IIR));
	if (illumos_has_pch(i915))
		gen2_irq_reset(uncore, illumos_irq_regs(ILLUMOS_SDEIMR,
		    ILLUMOS_SDEIER, ILLUMOS_SDEIIR));
}

