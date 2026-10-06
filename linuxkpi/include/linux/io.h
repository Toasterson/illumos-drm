/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's MMIO accessors and ioremap() (include/linux/io.h, asm/io.h;
 * GPL-2.0; reimplemented). Register mappings are ddi_regs_map_setup(9F)
 * mappings of a PCI BAR (uncached, or write-combining for ioremap_wc and
 * the _wc forms); the accessors are volatile loads and stores of the
 * access size, which is what ddi_get32(9F) and friends compile to on
 * x86. ioremap() of a physical address finds the BAR of the driver's
 * device that contains it (src/lkpi_pci.c) and fails (NULL) otherwise.
 */
#ifndef _LINUX_IO_H
#define	_LINUX_IO_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/string.h>
#include <linux/err.h>
#include <asm/barrier.h>

#define	__lkpi_rd(T, a)		(*(const volatile T *)(a))
#define	__lkpi_wr(T, v, a)	(*(volatile T *)(a) = (v))

static inline u8 readb(const volatile void __iomem *a) { return (__lkpi_rd(u8, a)); }
static inline u16 readw(const volatile void __iomem *a) { return (__lkpi_rd(u16, a)); }
static inline u32 readl(const volatile void __iomem *a) { return (__lkpi_rd(u32, a)); }
static inline u64 readq(const volatile void __iomem *a) { return (__lkpi_rd(u64, a)); }
static inline void writeb(u8 v, volatile void __iomem *a) { __lkpi_wr(u8, v, a); }
static inline void writew(u16 v, volatile void __iomem *a) { __lkpi_wr(u16, v, a); }
static inline void writel(u32 v, volatile void __iomem *a) { __lkpi_wr(u32, v, a); }
static inline void writeq(u64 v, volatile void __iomem *a) { __lkpi_wr(u64, v, a); }

#define	readb_relaxed(a)	readb(a)
#define	readw_relaxed(a)	readw(a)
#define	readl_relaxed(a)	readl(a)
#define	readq_relaxed(a)	readq(a)
#define	writeb_relaxed(v, a)	writeb(v, a)
#define	writew_relaxed(v, a)	writew(v, a)
#define	writel_relaxed(v, a)	writel(v, a)
#define	writeq_relaxed(v, a)	writeq(v, a)
#define	__raw_readl(a)		readl(a)
#define	__raw_writel(v, a)	writel(v, a)
#define	__raw_readq(a)		readq(a)
#define	__raw_writeq(v, a)	writeq(v, a)
#define	ioread8(a)		readb(a)
#define	ioread16(a)		readw(a)
#define	ioread32(a)		readl(a)
#define	ioread64(a)		readq(a)
#define	iowrite8(v, a)		writeb(v, a)
#define	iowrite16(v, a)		writew(v, a)
#define	iowrite32(v, a)		writel(v, a)
#define	iowrite64(v, a)		writeq(v, a)

static inline void
memcpy_fromio(void *to, const volatile void __iomem *from, size_t n)
{
	memcpy(to, (const void *)(uintptr_t)from, n);
}

static inline void
memcpy_toio(volatile void __iomem *to, const void *from, size_t n)
{
	memcpy((void *)(uintptr_t)to, from, n);
}

static inline void
memset_io(volatile void __iomem *a, int c, size_t n)
{
	memset((void *)(uintptr_t)a, c, n);
}

#define	IOMEM_ERR_PTR(err)	((void __iomem *)ERR_PTR(err))

extern void __iomem *ioremap(resource_size_t offset, unsigned long size);
extern void __iomem *ioremap_wc(resource_size_t offset, unsigned long size);
extern void __iomem *ioremap_uc(resource_size_t offset, unsigned long size);
extern void iounmap(volatile void __iomem *addr);
extern int arch_phys_wc_add(unsigned long base, unsigned long size);
extern void arch_phys_wc_del(int handle);
extern int arch_io_reserve_memtype_wc(resource_size_t start,
    resource_size_t size);
extern void arch_io_free_memtype_wc(resource_size_t start,
    resource_size_t size);

#define	ioremap_cache(o, s)	ioremap(o, s)
#define	devm_ioremap(d, o, s)	ioremap(o, s)
#define	devm_ioremap_wc(d, o, s)	ioremap_wc(o, s)
#define	mmiowb()		barrier()

/*
 * memremap(): a kernel mapping of physical memory that is not a BAR of
 * the driver's device, such as the ACPI OpRegion the firmware leaves for
 * the GPU (src/lkpi_page.c). Only MEMREMAP_WB is supported; other types
 * fail (NULL), as do ranges beyond the CPU's physical address width.
 * memunmap() takes the returned address.
 */
#define	MEMREMAP_WB	(1UL << 0)
#define	MEMREMAP_WT	(1UL << 1)
#define	MEMREMAP_WC	(1UL << 2)
#define	MEMREMAP_ENC	(1UL << 3)
#define	MEMREMAP_DEC	(1UL << 4)

extern void *memremap(resource_size_t offset, size_t size,
    unsigned long flags);
extern void memunmap(void *addr);

/*
 * Port I/O in Linux's argument order, outb(value, port). The prologue
 * included illumos' <sys/sunddi.h>, whose inb() .. outl() (inline
 * assembly with _ASM_INLINES) take the port first: outb(port, value).
 * Linux code calling outb(value, port) against those would write the
 * port number to the port the value names (i915's VGA sequencer access
 * in intel_vga.c). The in*() functions take the same argument in both,
 * so only the out*() calls are redirected; the parenthesised names reach
 * illumos' functions.
 */
static inline void
lkpi_outb(u8 v, unsigned long port)
{
	(outb)((int)port, v);
}

static inline void
lkpi_outw(u16 v, unsigned long port)
{
	(outw)((int)port, v);
}

static inline void
lkpi_outl(u32 v, unsigned long port)
{
	(outl)((int)port, v);
}

/* Replaces the prologue's guard (lkpi/prologue.h). */
#undef	outb
#undef	outw
#undef	outl
#define	outb(v, p)	lkpi_outb((v), (p))
#define	outw(v, p)	lkpi_outw((v), (p))
#define	outl(v, p)	lkpi_outl((v), (p))

#endif
