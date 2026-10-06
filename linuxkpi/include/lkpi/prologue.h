/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * illumos LinuxKPI: the prologue of every translation unit compiled
 * against the layer (cc -include lkpi/prologue.h, after the driver's
 * configuration header).
 *
 * It includes every illumos kernel header the layer needs, once and
 * before any Linux-API header, and then renames the few illumos
 * identifiers that Linux code uses for something else:
 *
 *	struct mutex	illumos: the tag of kmutex_t; Linux: struct mutex,
 *			which is the facade's struct lx_mutex
 *	struct page	illumos: page_t (vm/page.h); Linux: the layer's page
 *	struct pid	illumos: sys/proc.h; Linux: the layer's struct pid
 *	min(), max()	illumos: macros of sys/ddi.h; Linux: type-checked
 *
 * The renames are macros, so they apply to every later token: a Linux
 * field or variable called "page" becomes "lx_page" consistently in all
 * code compiled with the layer, which changes no layout. Layer sources
 * that need the illumos meaning use the typedefs (kmutex_t, page_t).
 * Nothing after this point may include an illumos header that uses those
 * names; the layer's own headers include only <lkpi/...>, <linux/...>,
 * <asm/...> and the facade.
 */

#ifndef _LKPI_PROLOGUE_H
#define	_LKPI_PROLOGUE_H

#if !defined(_KERNEL)
#error "the LinuxKPI layer is for the illumos kernel (-D_KERNEL)"
#endif

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/systm.h>
#include <sys/cmn_err.h>
#include <sys/varargs.h>
#include <sys/kmem.h>
#include <sys/ksynch.h>
#include <sys/atomic.h>
#include <sys/debug.h>
#include <sys/conf.h>
#include <sys/modctl.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ddi_intr.h>
#include <sys/ddidmareq.h>
#include <sys/taskq.h>
#include <sys/time.h>
#include <sys/errno.h>
#include <sys/stdbool.h>
#include <sys/stddef.h>
#include <sys/pci.h>
#include <sys/firmload.h>
#include <sys/uio.h>
#include <sys/file.h>
#include <sys/cpuvar.h>
#include <sys/disp.h>
#include <sys/stat.h>
#include <sys/ddidevmap.h>
#include <sys/poll.h>
#include <sys/cred.h>
#include <sys/ioccom.h>

/*
 * Port output: illumos' outb(port, value), outw() and outl() (declared by
 * <sys/sunddi.h> above) take the arguments in the other order than
 * Linux's outb(value, port). <linux/io.h> redefines them in Linux's
 * order; a TU that calls them without it fails to compile here rather
 * than swap the port and the value. (outb)(...) still reaches illumos'.
 */
extern void lkpi_port_output_needs_linux_io_h(void)
    __attribute__((__error__("outb(value, port) needs <linux/io.h>")));
#define	outb(v, p)	lkpi_port_output_needs_linux_io_h()
#define	outw(v, p)	lkpi_port_output_needs_linux_io_h()
#define	outl(v, p)	lkpi_port_output_needs_linux_io_h()

/* illumos macros whose names Linux code defines differently. */
#undef	min
#undef	max
#undef	ARRAY_SIZE
#undef	roundup
#undef	__aligned
#undef	__packed
#undef	__unused
#undef	__used
#undef	__weak
#undef	__noreturn
#undef	__unreachable
#undef	ALIGN
#undef	SET
#undef	CLR
#undef	tracing
#undef	sigmask
#undef	TS_RUN
#undef	BIT
#undef	__maybe_unused
#undef	__section
#undef	__cacheline_aligned
#undef	NOPID		/* <sys/proc.h>; i915 names a register so */

/* illumos structure tags that are Linux structures in Linux code. */
#define	mutex	lx_mutex
#define	page	lx_page
#define	pid	lx_pid
#define	file	lx_file
#define	module	lx_module
#define	kstat	lx_kstat
/* struct kmem_cache: illumos' kmem_cache_t tag; Linux code means the layer's. */
#define	kmem_cache	lkpi_kmem_cache
/* sys/disp.h declares resume(); i915 has a static function of that name. */
#define	resume	lx_resume
/*
 * Linux functions the layer defines whose names genunix uses too (krtld
 * would report them multiply defined and bind every caller to genunix's):
 * the layer's are linked under these names.
 */
#define	timer_delete	lkpi_timer_delete
#define	free_pages	lkpi_free_pages
#define	get_device	lkpi_get_device
#define	sort		lkpi_sort
#define	sscanf		lkpi_sscanf
#define	vsscanf		lkpi_vsscanf

#include <lkpi/config.h>

/*
 * The DRM facade's handle types (facade 1.1): drivers built on the layer
 * see the layer's struct device and struct file in drm_device.dev,
 * drm_minor.kdev, drm_gem_object.filp, drm_file.filp and dma_buf.file.
 */
struct device;
struct lx_file;
extern dev_info_t *lkpi_dev_dip(const struct device *dev);
#define	DRM_FACADE_DEVICE_T		struct device
#define	DRM_FACADE_DEVICE_DIP(d)	lkpi_dev_dip(d)
#define	DRM_FACADE_FILE_T		struct lx_file
#define	DRM_FACADE_INODE_T		struct inode

#endif	/* _LKPI_PROLOGUE_H */
