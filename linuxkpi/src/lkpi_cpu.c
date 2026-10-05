/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The x86 CPU interfaces of the layer (<asm/cpu_device_id.h>,
 * <asm/tsc.h>, <asm/fpu/api.h>, <asm/cacheflush.h>):
 *
 * boot_cpu_data	filled once by lkpi_cpu_init() from CPUID on the
 *			loading CPU: vendor, family/model/stepping with the
 *			extended family (family 0xf) and extended model
 *			(family >= 6) rules, x86_vfm, the CLFLUSH line size
 *			(CPUID.1:EBX[15:8] * 8) and the feature words
 *			[0] CPUID.1:EDX, [1] CPUID.1:ECX, [2] CPUID.(7,0):EBX.
 * tsc_khz		measured at init: rdtsc against gethrtime(9F) over
 *			drv_usecwait(9F) intervals of 10 ms (the median of
 *			three); there is no DDI interface for the TSC rate.
 * wbinvd		writes back and invalidates the caches of the calling
 *			CPU only: the DDI has no cross-call, so other CPUs'
 *			caches are not flushed (see <linux/smp.h>).
 * kernel_fpu_*()	over illumos kernel_fpu_begin()/kernel_fpu_end() with
 *			KFPU_NO_STATE and preemption disabled, as Linux's
 *			sections are non-preemptible. illumos panics on a
 *			nested begin; the layer counts nesting per CPU instead
 *			(the outermost pair enters/leaves the FPU section).
 */

#include <linux/kernel.h>
#include <linux/bug.h>
#include <asm/cpu_device_id.h>
#include <asm/processor.h>
#include <asm/tsc.h>
#include <asm/fpu/api.h>
#include <asm/cacheflush.h>
#include <asm/special_insns.h>
#include <linux/smp.h>
#include <lkpi_internal.h>

/*
 * illumos' kernel FPU interface (uts/common/sys/kfpu.h, implemented in
 * uts/intel/os/fpu.c). Not a DDI interface and not in every installed
 * header set, so declared here exactly as in the gate.
 */
typedef struct kfpu_state kfpu_state_t;
extern void (kernel_fpu_begin)(kfpu_state_t *, uint_t);
extern void (kernel_fpu_end)(kfpu_state_t *, uint_t);
#define	KFPU_NO_STATE	0x01

struct cpuinfo_x86 boot_cpu_data;

static unsigned int lkpi_tsc_khz_val;

static inline void
lkpi_cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
	__asm__ __volatile__("cpuid"
	    : "=a" (*a), "=b" (*b), "=c" (*c), "=d" (*d)
	    : "a" (leaf), "c" (sub));
}

static bool
lkpi_vendor_is(const u32 v[3], const char *name)
{
	const char *p = (const char *)v;
	int i;

	for (i = 0; i < 12; i++) {
		if (p[i] != name[i])
			return (false);
	}
	return (true);
}

/* One TSC rate sample in kHz over a 10 ms busy wait. */
static u64
lkpi_tsc_sample(void)
{
	hrtime_t t0, t1;
	u64 c0, c1;

	t0 = gethrtime();
	c0 = rdtsc();
	drv_usecwait(10000);
	t1 = gethrtime();
	c1 = rdtsc();
	if (t1 <= t0 || c1 <= c0)
		return (0);
	/* cycles per ns * 10^6 = kHz; the product stays below 2^63. */
	return ((c1 - c0) * 1000000ULL / (u64)(t1 - t0));
}

static unsigned int
lkpi_tsc_measure(void)
{
	u64 s[3], t;
	int i, j;

	for (i = 0; i < 3; i++)
		s[i] = lkpi_tsc_sample();
	for (i = 0; i < 3; i++) {
		for (j = i + 1; j < 3; j++) {
			if (s[j] < s[i]) {
				t = s[i];
				s[i] = s[j];
				s[j] = t;
			}
		}
	}
	return (s[1] > UINT_MAX ? UINT_MAX : (unsigned int)s[1]);
}

int
lkpi_cpu_init(void)
{
	struct cpuinfo_x86 *c = &boot_cpu_data;
	u32 a, b, cx, d, max, vendor[3];
	u32 family, model;

	bzero(c, sizeof (*c));
	lkpi_cpuid(0, 0, &max, &vendor[0], &vendor[2], &vendor[1]);
	if (lkpi_vendor_is(vendor, "GenuineIntel"))
		c->x86_vendor = X86_VENDOR_INTEL;
	else if (lkpi_vendor_is(vendor, "AuthenticAMD"))
		c->x86_vendor = X86_VENDOR_AMD;
	else if (lkpi_vendor_is(vendor, "HygonGenuine"))
		c->x86_vendor = X86_VENDOR_HYGON;
	else
		c->x86_vendor = X86_VENDOR_UNKNOWN;

	if (max >= 1) {
		lkpi_cpuid(1, 0, &a, &b, &cx, &d);
		family = (a >> 8) & 0xf;
		model = (a >> 4) & 0xf;
		if (family == 0xf)
			family += (a >> 20) & 0xff;
		if (family >= 6)
			model += ((a >> 16) & 0xf) << 4;
		c->x86 = (u8)family;
		c->x86_model = (u8)model;
		c->x86_stepping = (u8)(a & 0xf);
		c->x86_capability_lkpi[0] = d;
		c->x86_capability_lkpi[1] = cx;
		if (d & (1U << 19))
			c->x86_clflush_size = (u16)(((b >> 8) & 0xff) * 8);
	}
	if (max >= 7) {
		lkpi_cpuid(7, 0, &a, &b, &cx, &d);
		c->x86_capability_lkpi[2] = b;
	}
	if (c->x86_clflush_size == 0)
		c->x86_clflush_size = 64;
	c->x86_cache_alignment = c->x86_clflush_size;
	c->x86_vfm = VFM_MAKE((u32)c->x86_vendor, (u32)c->x86,
	    (u32)c->x86_model);

	lkpi_tsc_khz_val = lkpi_tsc_measure();
	return (0);
}

/*
 * The first entry of the table (ended by an entry without
 * X86_CPU_ID_FLAG_ENTRY_VALID) that matches the boot CPU, or NULL.
 */
const struct x86_cpu_id *
x86_match_cpu(const struct x86_cpu_id *match)
{
	const struct cpuinfo_x86 *c = &boot_cpu_data;
	const struct x86_cpu_id *m;

	for (m = match; m->flags & X86_CPU_ID_FLAG_ENTRY_VALID; m++) {
		if (m->vendor != X86_VENDOR_ANY && m->vendor != c->x86_vendor)
			continue;
		if (m->family != X86_FAMILY_ANY && m->family != c->x86)
			continue;
		if (m->model != X86_MODEL_ANY && m->model != c->x86_model)
			continue;
		if (m->steppings != X86_STEPPING_ANY &&
		    !(m->steppings & (1U << c->x86_stepping)))
			continue;
		if (m->feature != X86_FEATURE_ANY &&
		    (m->feature >= 32 * ARRAY_SIZE(c->x86_capability_lkpi) ||
		    !boot_cpu_has(m->feature)))
			continue;
		return (m);
	}
	return (NULL);
}

unsigned int
lkpi_tsc_khz(void)
{
	return (lkpi_tsc_khz_val);
}

void
lkpi_wbinvd_all(void)
{
	wbinvd();
}

/* --- kernel FPU sections ------------------------------------------------- */

static struct lkpi_fpu_cpu {
	kthread_t *owner;
	uint_t depth;
} lkpi_fpu_cpu[NR_CPUS];

void
lkpi_kernel_fpu_begin(void)
{
	struct lkpi_fpu_cpu *s;
	processorid_t id;

	kpreempt_disable();
	id = CPU->cpu_id;
	if (id < 0 || id >= NR_CPUS) {
		/* No nesting state for this CPU: enter as Linux does. */
		(kernel_fpu_begin)(NULL, KFPU_NO_STATE);
		return;
	}
	s = &lkpi_fpu_cpu[id];
	if (s->depth != 0) {
		/*
		 * Nested on this CPU: by the same thread (illumos would
		 * panic) or by an interrupt thread over a section (Linux's
		 * irq_fpu_usable() would be false). The FPU is already
		 * enabled; only count.
		 */
		if (s->owner != curthread)
			WARN_ONCE(1, "kernel_fpu_begin() in an interrupt "
			    "during an FPU section");
		s->depth++;
		return;
	}
	(kernel_fpu_begin)(NULL, KFPU_NO_STATE);
	s->owner = curthread;
	s->depth = 1;
}

void
lkpi_kernel_fpu_end(void)
{
	struct lkpi_fpu_cpu *s;
	processorid_t id = CPU->cpu_id;

	if (id < 0 || id >= NR_CPUS) {
		(kernel_fpu_end)(NULL, KFPU_NO_STATE);
		kpreempt_enable();
		return;
	}
	s = &lkpi_fpu_cpu[id];
	if (WARN_ONCE(s->depth == 0, "kernel_fpu_end() without begin"))
		return;
	if (--s->depth == 0) {
		s->owner = NULL;
		(kernel_fpu_end)(NULL, KFPU_NO_STATE);
	}
	kpreempt_enable();
}
