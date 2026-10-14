# i915n display enablement pilot

The default display build now uses the Linux display description for every
GPU admitted by patch 0009: PCH-based integrated Gen6 through Gen9.5. This
includes Sandy Bridge, Ivy Bridge, Haswell, Broadwell, Skylake and the
Kaby/Coffee/Comet Lake families represented by the pinned PCI tables.
Only Ivy Bridge has prior hardware acceptance. Other families are experimental.
This change does not expand the admission guard to Gen10+, low-power SoCs,
or discrete GPUs. Compiling their display translation units is not support.

## Recovery policy

`/kernel/drv/i915n.conf` documents three attach-time integer properties:

* `i915n-force-no-display=1;` disables display on matching device nodes.
* `i915n-no-display-device-ids=0x1916,0x1926;` matches PCI device IDs.
* `i915n-no-display-generations=9;` matches `GRAPHICS_VER`, so 7 includes
  Ivy Bridge and Haswell, and 9 includes SKL/KBL/CFL/CML. It is not the
  display IP version and does not take fractional generations.

The controls are ORed; an empty/unset list matches nothing. Per-node device
specifications can restrict the first property to one observed PCI node.
Properties do not propagate from parent bridges. They are read before display
initialization, and Linux's `no_display` description clears MODESET/ATOMIC.
Patch 0006 is retained: it only programs headless display interrupt masking
when `HAS_DISPLAY` is false and preserves GT interrupts on that path.

Use a disposable boot environment and preserve the known-good boot entry.
Update the alternate root's boot archive after editing configuration, then
boot it. Headless is not a framebuffer-preservation mode: it also disables
the i915n console. Have SSH/serial recovery access before testing it.

## Audited boundaries

* The display IRQ, HPD, power-well, DDI/FDI, AUX, GMBUS, DMC and DP MST
  translation units are already built. Display devices take their Linux IRQ
  reset/postinstall/handler paths; the native PCI layer supplies the interrupt.
* The OpRegion VBT reader is already present (patch 0013). ACPI notifications,
  ASLE, platform audio integration and backlight class services are not.
* The DMC worker calls `request_firmware`; LinuxKPI loads through illumos
  `firmware_open/read/close`, using `firmware/i915/<basename>` in the module
  search path. Missing DMC retains the display INIT power reference and logs
  that runtime power management is disabled. The package does not supply the
  firmware blobs. This is functional display without DMC power saving, not
  validation of Skylake power behavior. GuC/HuC are not needed for display on
  these admitted devices.
* The runtime-PM shim keeps devices active. PCI attach/detach currently reject
  DDI_RESUME/DDI_SUSPEND; this pilot makes no suspend/resume claim.
* DSI and LPE audio TUs remain excluded, but the admitted PCH GPU families do
  not require them. HDCP is deliberately unavailable, including on families
  that have hardware support. The TTM/local-memory/GSC stubs and stolen-memory
  decoding are additional barriers to newer/discrete hardware.
* Normal console restoration uses atomic modesetting. The register-only panic
  restoration is now explicitly Ivy-Bridge-only, avoiding writes through the
  wrong plane register layout on newly enabled platforms. Other platforms may
  keep a stale/blank panic screen; capture dumps remotely. Generalizing that
  path needs a separate hardware-validated implementation.

## Validation required before a release

Build both display and headless variants natively; preserve the exact source
revision, Linux PIN, compiler/flags, licence audit and link results. A successful
link against mapfiles does not prove runtime symbol providers or hardware.

For each platform, record PCI ID/stepping, BIOS, panel/output and dock, exact
package hashes and loaded modules. Exercise first attach, internal panel,
HDMI/DP, DP MST, modesets, vblank/page flips, rendering, VT transitions and
headless overrides (all-device, ID, generation, nonmatching controls). Check
IRQ storms and failure to quiesce. On Gen9 record DMC present/missing outcomes
and power-well warnings. Test kernel HPD separately from automatic Xorg RandR
notification, which this branch does not implement.
