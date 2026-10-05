#!/bin/bash
# Derive the R5c Linux configuration (i915 headless measurement) from the R5
# analysis config: i915 only, no xe/amdgpu, and the Linux features illumos
# does not provide switched off (see notes/r5c-i915-headless-inventory.md).
# Runs inside the measurement container in /work/linux.
set -euo pipefail
cd /work/linux
cp /work/linux-v7.2-r5.config .config
s=scripts/config
$s --disable DRM_XE --disable DRM_AMDGPU --disable HSA_AMD --disable DRM_RADEON \
   --module DRM_I915 \
   --disable DRM_I915_GVT --disable DRM_I915_GVT_KVMGT --disable DRM_I915_PXP \
   --disable DRM_I915_CAPTURE_ERROR --disable DRM_I915_COMPRESS_ERROR \
   --disable DRM_I915_USERPTR --disable DRM_I915_DP_TUNNEL --disable DRM_I915_SELFTEST \
   --disable DRM_I915_WERROR --disable DRM_I915_DEBUG --disable DRM_I915_DEBUG_GEM \
   --disable DRM_I915_DEBUG_RUNTIME_PM --disable DRM_I915_SW_FENCE_DEBUG_OBJECTS \
   --disable DRM_FBDEV_EMULATION --disable SND_HDA_I915 --disable CEC_CORE --disable CEC_NOTIFIER \
   --disable DEBUG_FS --disable HWMON --disable PERF_EVENTS \
   --disable IA32_EMULATION --disable X86_X32_ABI --disable COMPAT \
   --disable FTRACE --disable TRACEPOINTS --disable KPROBES --disable BPF_EVENTS \
   --disable ACPI --disable PM --disable PM_SLEEP --disable SUSPEND --disable HIBERNATION \
   --disable INTEL_IOMMU --disable IOMMU_SUPPORT \
   --disable MMU_NOTIFIER --disable KVM --disable DEBUG_INFO \
   --disable LOCKDEP --disable PROVE_LOCKING --disable DEBUG_LOCK_ALLOC
make -s LLVM=1 ARCH=x86_64 olddefconfig
# The committed copy keeps only the settings (no Kconfig help or menu text):
{
	echo "# Linux v7.2 configuration of the R5c headless i915 build (make-config.sh)."
	echo "# Only the CONFIG_ settings are kept (no Kconfig text)."
	grep -E '^(CONFIG_|# CONFIG_.* is not set$)' .config
} > linux-v7.2-r5c.config
