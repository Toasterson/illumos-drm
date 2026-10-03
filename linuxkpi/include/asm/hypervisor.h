/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's hypervisor detection: the layer reports none by type (only the CPUID HYPERVISOR bit). */
#ifndef _ASM_HYPERVISOR_H
#define	_ASM_HYPERVISOR_H
enum x86_hypervisor_type {
	X86_HYPER_NATIVE = 0,
	X86_HYPER_VMWARE,
	X86_HYPER_MS_HYPERV,
	X86_HYPER_XEN_PV,
	X86_HYPER_XEN_HVM,
	X86_HYPER_KVM
};
#define	hypervisor_is_type(t)	((t) == X86_HYPER_NATIVE)
#endif
