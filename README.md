# illumos-drm

A Direct Rendering Manager (DRM) core for illumos written in Rust, and the
drivers built on it.

The project is hosted at <https://code.aopc.cloud/illumos/illumos-drm>. It
sits next to [illumos-ddi](https://code.aopc.cloud/illumos/illumos-ddi),
whose `ddi-sys` and `ddi` crates provide the kernel interfaces, and it uses
the same toolchain, kernel target and build recipe.

The direction was decided on 27 September 2026: a Rust DRM core that
provides the userland interface libdrm and Mesa expect (card and render
nodes, authentication, capabilities, GEM, PRIME, syncobj, KMS), and a Rust
virtio-gpu driver on top of it. Existing vendor drivers stay in C and would
be carried later through a C interface of the core. The milestones are R0
(uAPI, device registration, per-file state, core ioctls), R1 (GEM and dumb
buffers), R2 (fences, syncobj, PRIME), R3 (KMS and atomic), R4
(virtio-gpu), all five done, and R5 (C interface), whose first step R5a
(the `misc/drm_core` kernel module with a versioned C facade, and the C test
driver `cdrmnull`) is done.

## Where things stand

Milestones R0, R1, R2, R3 and R4 work, and so do R5a and R5b.

R5b (public-layout base objects, facade 1.0; see *R5b* under *Decisions*):
on 3 October 2026, on fresh guests of `golden-20260930` (illumos
`7e802e88a3`, libdrm 2.4.134) with a virtio-gpu device, at commit
`c24a58e` (the later commits change tests and comments only),
`tools/test/run-r5b-test.sh` passed: `run-r5a-test.sh` (R0 to R4 on the
three drivers, R5a's cross-driver test after three unload orders, G6,
R1 for two `cdrmnull` devices, the facade mismatch probes including a
same-size `drm_device` with a smaller private tail; 168 passing checks,
none failed), the managed resources of `cdrmnull` released in reverse
order at all 7 of its detaches, `drm_r5b_test` (47 checks, none failed:
the 15 fence-ops checks of `FENCE_OPS_SELFTEST` including 200 rounds of
add/remove/signal against the soft interrupt, the driver's timeline
fences in a binary sync object, an array of two contexts, a chain
signalled out of order and an error sync file, 3000 handle walks while a
thread created and closed 1592 handles, the live GEM reference count
through name, dma-buf and mapping), every logging path in the system log
with `drm_debug` 0 and 0x6, timeline fences released in thread context
only, and `run-g8-test.sh kernel`. gcc's layout of the facade (40 sizes,
256 offsets) matched the Rust bindings, and `kernel-check` with
`misc/drm_core` loaded verified `drm-facade-sys` against the module's CTF
(sizes 40 of 45 and offsets 244 of 256, the rest members of anonymous
types checked through their parents; symbols 123 of 123). At `b3fab03`
every phase of `run-g7-test.sh` passed again (81 checks), and with the new
stack selected `run-r5a-test.sh` (`LEGACY_GUARD=skip`) behaved as in G7:
its first run after the packaged drivers' removal failed the unprivileged
opens of `cdrmnull`'s stale `0600` nodes (the open issue of G7 below),
and the run after it passed (76 checks). Logs:
`logs/hephaistos/illumos-drm/20261003-r5b-*` in the workspace.

R5e (the Linux DRM helpers C drivers need; see *R5e* under *Decisions*):
`misc/drm_helpers` carries Linux v7.2's `drm_mm` unmodified, on MIT
reimplementations of the Linux interfaces it needs (`linuxkpi/`), and
exports exactly the ten `drm_mm` functions i915's headless build imports.
On 4 October 2026 it was built on illumos (gcc 14.3, the gate's kernel
flags, `-z defs`) and tested on a fresh guest of `golden-20260930`
(illumos `7e802e88a3`) with `tools/test/run-r5e-test.sh`: the module
loaded and unloaded alone; the test driver `drm_helpers_test`, linked with
`-N misc/drm_helpers` against the exported functions only, ran the 14
`drm_mm` self-tests at attach for five seeds up to scale 50 (6,421,733
checks in the largest run, none failed), `misc/drm_helpers` refused to
unload under it and unloaded after `rem_drv`; an injected failing check was
counted and logged with the attach still succeeding; `/etc` and
`/devices/pseudo` were as before. `carried/host-test.sh` runs the same
tests plus the host-only ones and the red-black and interval tree tests
(24 tests, 839,569 checks) with clang under AddressSanitizer and
UndefinedBehaviorSanitizer and with gcc on illumos; a seed gives the same
counts on every compiler and in the kernel.

G7 (both DRM stacks on one machine): on 3 October 2026, on a fresh guest
of `golden-20260930` (illumos `7e802e88a3`) with a virtio-gpu device,
gfx-drm's `driver/graphics/drm` built with the reverse guard (gfx-drm
branch `drm-coexist` on top of `libdrm-update`, with `drm-devlink` and
libdrm 2.4.134) and this repository's `drm-core`, `drmadm`, `virtio-gpu`
and `cdrmnull` installed into one boot environment, `tools/test/run-g7-test.sh`
passed every phase (81 checks, no failure; logs
`logs/hephaistos/illumos-drm/20261003-g7-run3-*` in the workspace; the
phases up to `select-root` ran at commit `89ea95e`, `overlap`, `rollback`
and the recovery at `3ba24f1` and the overlap's synchronisation of this
branch's head). On
OpenIndiana's own `driver/graphics/drm` the legacy `misc/drm` still loads
beside `misc/drm_core` and the old link rule leaves a dangling
`/dev/dri/card0`. With both guards each core refused while the other was
loaded, also for an explicit-path `modload`, for dependent drivers
(`i915`, `virtio_gpu`, `cdrmnull`) and while the other was held busy before
its `_init` by a DTrace chill; a core left in memory by a refused dependency
did not keep the other out; 100 concurrent loads ended with exactly one
core loaded each time (51 legacy, 49 new). `drmadm select new` refused while
a module was loaded and while its lock was held, moved the 110 `i915`
aliases off (unbound: no `i915n` yet) and wrote the fragment; after the
reboot `misc/drm`, `/kernel/misc/amd64/drm` and `drv/i915` were excluded
(no guard ran) and libdrm listed the two new devices. `select legacy`
gave `i915` its 110 aliases back (`pkg verify` clean again); after the
reboot the new modules were excluded, the legacy core and `i915` loaded
(no Intel GPU to attach to), `devfsadm -C` removed all four stale
`/dev/dri` links and libdrm listed no device. The aliases moved to a
stand-in `i915n` and back, `drmadm -R` changed only a mounted clone, a
failing alias move was undone, and the hand recovery (fragment removed,
reboot) left nothing excluded. With the new stack selected,
`run-r5a-test.sh` (`LEGACY_GUARD=skip`) passed: R0 to R4, R5a's 81-check
test after three unload orders, `run-g6-test.sh` and `run-r1-test.sh` for
`cdrmnull`, nothing failed (75 checks; the run before it, right after the
packaged `virtio_gpu` and `cdrmnull` were uninstalled, failed 16 checks
of unprivileged opens of `cdrmnull`'s nodes, which the packaged driver had
left with mode 0600, and a `/dev/dri` state check; see the open issue
below). Nothing panicked. The first guest runs found
that both guards counted a core left in memory by a failed dependency
`_init` (fixed in both, illumos-ddi `g7-module-busy`), that `update_drv -d`
with a list of aliases removes nothing, and that `r` is a ksh93 alias.

R5a (`misc/drm_core` and the C facade): since 1 October 2026 the core is one
kernel module, `misc/drm_core`, that exports a C interface (the headers in
`include/drm`), and every driver is linked against it with
`-N misc/drm_core`: `rdrmnull` and `virtio_gpu` through the safe Rust crate
`drm-facade`, and `cdrmnull`, a C port of `rdrmnull`, directly. See
*R5a* under *Decisions* for the design. On 3 October 2026, at commit
`b040324` (illumos-ddi `10bdfba`), `tools/test/run-r5a-test.sh` passed on a
fresh guest of `golden-20260930` (illumos `7e802e88a3`, libdrm 2.4.134)
with 167 checks and no failure. `misc/drm_core` exports exactly the 102
symbols of `drm_core.mapfile` and loads and unloads alone; gcc's view of
the facade (38 sizes, 233 offsets) matches the Rust bindings; drivers
built for another facade major version or another size of
`struct drm_device` refuse to load with the check's warning; with
gfx-drm's `misc/drm` loaded, `misc/drm_core` refuses to load with its
warning, and loads once the legacy module is gone. `cdrmnull`,
`rdrmnull` and `virtio_gpu` attached together on `misc/drm_core`
(neither Rust driver carries a symbol of the core's stateful modules;
the pattern finds 122 in the core's archive): R0 and R1 passed on both
null drivers with their 32-bit probes, R2 on both (200 checks each), R3
on `rdrmnull`, R4 on `virtio_gpu` (63 checks), and `drm_r5a_test`
(81 checks: PRIME between every pair of the three drivers, one handle
per object per file, cross-driver fences, `cdrmnull`'s
`PRIME_SELFTEST` and `FENCE_SELFTEST`), four times: once, and after
unloading the drivers in three orders and attaching them again, while
`misc/drm_core` refused to unload as long as any driver depended on it.
Each of `cdrmnull`'s four detaches raced 330,000 to 600,000 facade calls
from a task queue thread and a soft interrupt, 5,900 to 73,000 of them
while `drm_dev_unregister()` ran, without a failure. `mdb -k` printed the
facade structures from the module's CTF and `cdrmnull`'s live
`drm_device`. `run-g6-test.sh` and `run-r1-test.sh` with two `cdrmnull`
devices passed inside the run, and `/etc` and `/devices` were as before.
At `08ca41e`, whose kernel code differs from `b040324` only in message
prefixes and in leaving finished callbacks' `node` empty, the same suite
passed (167 checks), and with `misc/drm_core` loaded `cargo xtask
kernel-check` verified
`drm-facade-sys` against the module's CTF (38 sizes, 233 offsets, 99
symbols) and the private tier's new `modctl` layout and symbols. The
packages `driver/graphics/drm-core` and `driver/graphics/cdrmnull`,
published from the run's build, installed beside `driver/graphics/drm`
and libdrm 2.4.134 into a new boot environment. Logs:
`logs/hephaistos/illumos-drm/20261003-r5a-*` in the workspace.

R0 (uAPI, device registration, per-file state, core ioctls): on 28
September 2026 the test driver `rdrmnull` was built for the kernel target,
linked with `ld -ztype=kmod`, loaded with `add_drv` on OpenIndiana Hipster
2026.04 (illumos `cda5b9a49f`), and exercised through the installed libdrm
2.4.109: `drmOpen("rdrmnull", NULL)` finds and opens `/dev/dri/card0`,
`drmGetVersion`, `drmGetCap`, `drmSetClientCap`, `drmSetInterfaceVersion`,
`drmGetBusid`/`drmSetBusid`, `drmGetMagic` with `drmAuthMagic` between an
unprivileged client and a root master, `drmGetClient`,
`drmSetMaster`/`drmDropMaster`, and `drmOpenRender(128)` as root and as
`nobody` all behave as on Linux. Eight unprivileged threads opening,
authenticating and closing concurrently while the root master drops and
retakes master, the 511-file limit, a 32-bit caller (core commands refused
with `ENOTSUP`, `ENOTTY` otherwise), the error paths, and the refusal to
unload while a file is open were tested too, and the module unloaded
cleanly with `rem_drv`. Every size, offset and ioctl number of the Rust
uAPI matches what gcc computes on illumos from the same headers.

R1 (GEM objects, handles, names, mmap, dumb buffers), on the same machine
and day: a process creates a dumb buffer, gets its fake offset with
`MODE_MAP_DUMB`, maps it with `mmap(2)` (the memory reads as zeros), writes
a pattern, and reads it back through a second mapping; a forked child reads
it through the inherited mapping and through a mapping of its own, and the
parent sees the child's writes; a separate root process and an unprivileged
process (once the master has authenticated it) open the object by its
`GEM_FLINK` name and map it; a render node may close handles but neither
name, open by name nor create dumb buffers (`EACCES`); a file without a
handle to an object cannot map it; partial `munmap(2)` splits a mapping;
closing the last handle removes the name while a mapping keeps the object;
the last `munmap(2)` frees it; `close(2)` releases every handle of a file;
eight threads create, map, name, open and destroy objects concurrently; a
reference dropped from a `timeout(9F)` callback (interrupt context) is
parked and released by the task queue; the per-object (16 MiB) and
per-device (64 MiB) memory limits of `rdrmnull` hold. The `gem` kstat
(`kstat -p rdrmnull:0:gem`) shows every object, handle, name, offset,
mapping and byte of object memory back at zero before the module unloads,
and the vmem arenas and identifier spaces of the device disappear with it.
The refault experiment below worked.

R2 (fences, reservations, sync objects with timelines, sync files, PRIME
with anonymous file descriptors), on 29 September 2026 on a disposable
guest with the same kernel: a dumb buffer exported with
`drmPrimeHandleToFD` travels over `SCM_RIGHTS` to an exec'd child, which
imports it with `drmPrimeFDToHandle` on its card and render files, maps
it through the handle and through the dma-buf descriptor, sees the
parent's contents and changes them for the parent; exporting an object
twice gives one dma-buf (one device number), importing it on the
exporting file gives the file's own handle, close-on-exec survives
`exec`, a read-only export refuses writable mappings, and the object
lives on through its dma-buf after every handle is closed. Binary and
timeline sync objects behave as libdrm expects (`drmSyncobj*`: create,
signal, reset, wait any and all, `WAIT_FOR_SUBMIT`, `WAIT_AVAILABLE`,
timeline signal, query, transfer both ways, export and import as sync
object files and as sync files) with fences that `rdrmnull`'s test
timeline signals from a soft interrupt: points signalled out of order
complete the timeline in order, and `poll(2)` of a sync file is woken by
`pollwakeup(9F)` in the soft interrupt (DTrace: at PIL 1, from the
timeline's handler). A sync object and a sync file passed to another
process work there; reservation waits distinguish writers from readers;
a signal interrupts a wait with `EINTR`; eight threads do all of it at
once. The `sync` kstat (`kstat -p rdrmnull:0:sync`) shows every fence,
callback, sync object, sync file, dma-buf and anonymous file gone after
each step, and the module unloads. A detach attempted while `rdrmnull`
holds a buffer with a pending fence in its reservation cancels the fence,
is refused, and leaves the display (`modetest` lists it) and the sync
paths (a sync object signalled by the test timeline, a sync file waking
`poll(2)`) working; once the buffer is released the module unloads. The
first run panicked the machine in
the devmap framework on the first `mmap(2)` of a dma-buf; the analysis
and the fix, which made that impossible in illumos-ddi, are in
[docs/anon-fd.md](docs/anon-fd.md) and illumos-ddi's `docs/DESIGN.md`
section 5.12.

R3 (kernel mode setting), on 29 September 2026 on a disposable Hipster
guest with the same kernel (`illumos-cda5b9a49f`): `rdrmnull` has one
virtual connector with a synthesized EDID, one encoder, one CRTC with a
primary and a cursor plane, a software vblank at the mode's refresh rate
and a software scanout. libdrm's `modetest` (package `system/test/libdrm`,
gfx-drm's build of the libdrm 2.4.109 tests) lists every object with its
properties and decodes the `IN_FORMATS` blobs, sets 1024x768 with a dumb
framebuffer (`-s`), page-flips at 60.00 Hz with vblank events (`-v`) and
sets the mode and primary plane through the atomic ioctl (`-a`).
`tools/test/drm_r3_test.c` (180 checks) covers mode objects, properties
and their get/set round trips, blobs, `ADDFB`/`ADDFB2`/`GETFB`/`GETFB2`
(an unsupported modifier is `EINVAL`), `TEST_ONLY` that changes nothing,
real commits with page-flip events read through `poll(2)` and `read(2)`,
the checksum of what the scanout shows after each flip, the legacy
wrappers, the vblank rate (60.07 Hz measured, timestamps 16.665 ms apart),
`WAIT_VBLANK` and CRTC sequence events, the 4096-byte event budget, close
semantics, four threads querying while the master flips, and the `kms` and
`gem` kstats back at their idle values before the module unloads. The R0
and R1 tests pass unchanged apart from the two capabilities R3 turns on.
A detach refused while `rdrmnull` holds a GEM object leaves the display
working: after the refused `modunload`, `modetest` lists it and
page-flips with vblank events at 60 Hz, and the module unloads once the
object is released (29 September 2026, same guest image).

R4 (virtio-gpu), 30 September 2026, on a disposable Hipster guest
(illumos `cda5b9a49f`) in QEMU 6.2 with `-device virtio-gpu-gl-pci` and
`-display egl-headless` (virglrenderer 0.9.1 on the host's GPU):
`virtio_gpu` attaches to the device (`virgl true, edid true`, two
capability sets, virgl and virgl2, the modern interface), and
`tools/test/run-r4-test.sh` passes: `drm_r4_test` checks 63 things, among
them a minimal virgl command stream (surface, framebuffer, clear) whose
result reads back from the host renderer with `TRANSFER_FROM_HOST`, fence
descriptors across the three `EXECBUFFER` layouts, binary and timeline
sync objects, a timeline wait that times out with `ETIME` after 100 ms,
PRIME into another file's context, every refusal (unknown `EXECBUFFER`
flags `EINVAL`, rings without `CONTEXT_INIT`, unknown handles and sync
objects, bad strides, oversized and unreadable streams), 2000 submissions
in a row and eight threads creating, clearing, fencing, reading back and
closing 800 textures at once; modetest lists the display with the
device's EDID (1024x768 at 75 Hz preferred), sets the mode, page-flips at
the mode's 75 Hz with vblank events, moves a cursor and commits
atomically; the module refuses to unload while a process holds a file,
and while only a dma-buf is left, works again afterwards, and unloads
once released, with `/etc` and `/devices` as before. At detach the driver
counted 19,543 requests, 7,224 of them fenced, none failed, and 2,011
resources created, none left. Xorg's modesetting driver runs on it (EDID,
1024x768, damage through `DIRTYFB`), and `glxgears` shows on the
virtio-gpu scanout (QMP `screendump`). The R0 to R3 suites pass unchanged
with `rdrmnull` in the same boot. The runs found three bugs, fixed on
this branch: a legacy cursor kept a framebuffer on a CRTC that a close had
turned off (a core fix, extended after review to every legacy cursor of
the closing file), the driver held the fence domain so the module
never unloaded, and the test's rate check. Mesa's virgl driver (Mesa
21.3.9) renders on the host through `virtio_gpu`: through EGL on a GBM
device, `GL_RENDERER` is `virgl` (GL 3.1), a cleared framebuffer reads
back exactly, and 600 clear-and-read-back frames take about 0.46 s; drawn to a
GBM scanout surface and page-flipped, 600 frames show at 74.8 per second
on the 75 Hz mode, and a host screendump shows the picture. The module
then unloads with no resources left. Mesa's virgl driver cannot run under
X yet, because OpenIndiana's Xorg 1.19.7 is built without glamor, and it
needs a workaround for libdrm's device enumeration (see *R4: Mesa*).
The guest's disk is a transitional virtio-blk device (`disable-modern=on`),
because illumos bug 18358, fixed after `cda5b9a49f`, makes the virtio
framework map the wrong BAR for the second modern device. A last run
with the final code, including the close of a file that set a cursor on
a dark CRTC, passed the driver test, the R0 to R3 suites and both virgl
tests again (Xorg was not rerun).

G6 (one device numbering across drivers, Linux's ioctl argument sizes),
1 October 2026, on a disposable Hipster guest with illumos `7e802e88a3`
(osnet 23295) in QEMU 6.2 with `-device virtio-gpu-gl-pci`, and gfx-drm's
libdrm 2.4.134 (gfx-drm pull request 8, `1e68727`) installed in the
guest: `tools/test/run-g6-test.sh` passed with 354 checks and none
failed. With `virtio_gpu` attached first and then two `rdrmnull`
devices, the devices were `card0`/`renderD128`, `card1`/`renderD129`
and `card2`/`renderD130` (node minors 0, 1, 2 and 128, 129, 130);
with the two `rdrmnull` devices first, `rdrmnull` had 0 and 1 and
`virtio_gpu` 2. libdrm's `drmdevice` and gfx-drm's `getsundev` listed
all three devices (getsundev: 73 checks passed), two and one after
unloads. With all three attached, the R0 to R3 tests passed on both
`rdrmnull` devices (R0 106 and 107 checks, R1 153, R2 216 including the
cross-device checks between the two, R3 184, and the 32-bit probes),
`drm_r4_test` on `virtio_gpu` (63 checks) as `card0` and as `card2`,
`drm_g6_test` (27 checks) on each device, and `modetest` listed every
display. The drivers unloaded in both orders, a device kept its number
while the other driver unloaded and came back, a reloaded driver got
its old numbers back and a driver attached into a gap got the lowest
free one, `misc/drm_minor` refused to unload while a driver was loaded
and unloaded after both, loading `rdrmnull` loaded it, and nothing
panicked. `run-r4-test.sh` (kept attached), `run-r1-test.sh` with two
`rdrmnull` devices and `run-r3-test.sh` then passed beside the attached
`virtio_gpu` (513, 477 and 72 checks), with `/etc`, `/devices` and
their own `/dev/dri` links as before. Under `truss`, libdrm 2.4.134
sent each of the four grown sync object commands once, at its new size,
and the kernel answered 0; its retry at the 2.4.109 size never ran.
The run used the kernel code of commit `0155ded`; the later changes to
the kernel code (the GEM registry entry is recorded as the last step of
attach, and the anonymous-file id space is named after the instance)
were tested on the host and linked on illumos, and wait for the next
guest run, as does `cargo xtask kernel-check` with `misc/drm_minor`
loaded (R5a's guest runs covered both, on `misc/drm_core`).

G8 (a re-import no longer detaches a buffer; see *Driver hooks per file
and object (G8)*), 3 October 2026, on a fresh guest of `golden-20260930`
(illumos `7e802e88a3`, libdrm 2.4.134) with `-device virtio-gpu-gl-pci`:
at commit `c2ea15b`, `tools/test/run-g8-test.sh` passed with
`drm_g8_test` at 143 checks and none failed: on `cdrmnull`, `rdrmnull`
and `virtio_gpu`, a file that imports its own buffer twice gets its own
handle and the `gem` kstat's handles, attachments, objects, memory and
open-hook calls do not move; a second file's double import adds one
handle, one attachment and one open; `GEM_OPEN` and `MODE_GETFB` add a
second handle and no attachment; each other driver's double import makes
one import object. On `virtio_gpu` (virgl), after the server file and a
client file each imported a 1024x768 texture twice, the server's virgl
clear and a `TRANSFER_TO_HOST` round trip through its first handle read
back exactly, the texture shown with `SETCRTC` and flushed with
`DIRTYFB` (`RESOURCE_FLUSH`) appeared in a host screendump, and the
client's drawing and its close left the server's context intact; QEMU
logged nothing. With the archives of the base `2306e85` and the same
test, 53 checks failed: the server's clear and transfers read back the
old contents, the display stayed red, and virglrenderer logged "Illegal
resource". `run-r5a-test.sh` (R0 to R4, G6, R5a, three reattach orders,
R1 with two `cdrmnull` devices) passed with 167 checks, among them
`drm_r1_test`'s second handle by `GEM_OPEN`. After a `pkg update` to
osnet 23298 (illumos `2cafbda106`) and G3's Mesa 25.3.6, G3's two-process
reproducer `tools/test/dmabuf_import_test.c` passed in all three modes
(import twice, once, draw first) and failed with the base's archives
exactly as G3 saw it (the importer's drawing into the twice-imported
buffer lost). Xorg's DRI3 path itself (`dri3_probe` of G4) could not be
run: OpenIndiana's xorg-server 1.19.7 needs `EGL_MESA_drm_image` for
glamor, which Mesa removed in 25.2. Logs:
`logs/hephaistos/illumos-drm/20261003-g8-*` in the workspace.

## What is in the repository

`crates/drm-uapi` is the DRM userland interface as Rust types: the
`#[repr(C)]` argument structures and constants of `drm.h`, `drm_mode.h` and
`virtgpu_drm.h` from libdrm 2.4.109, generated with bindgen, and the
`DRM_IOCTL_*` numbers in the illumos `_IOWR` encoding. It has no
dependencies and can serve userland programs as well as the kernel.

`crates/drm-core` (named `crates/drm` until R5a) is the core, a `no_std`
library on top of the `ddi` crate. Since R5a only `misc/drm_core` compiles it
with its `kernel` feature, which holds the stateful kernel layer; the
drivers use the facade and take only value helpers from it (display
modes, dumb-buffer layouts). The description below is of the core's own
interface, which `misc/drm_core` implements for every driver. A
driver implements the `drm_core::Driver` trait (name, date, description,
version, feature flags, mode-setting properties, bus ID, buffer-object
type, dumb-buffer rules, DMA attributes, memory limits, private ioctls)
and uses `drm_core::Device<D>` as the instance type of its `ddi::Driver`. The
device creates the minor nodes `card<N>` and `renderD<128+N>`, keeps
per-file state through clone opens, answers the core and GEM ioctls, and
maps objects through `devmap(9E)`. The decision logic (`state`, `caps`,
`ioctl`, `minor`, `objects` for handles and names, `idmap`, `offsets`,
`dumb`, and the reference counting in `karc` and `defer`) is plain data and
is unit-tested on the host, the reference counting and the handle and name
rules from eight threads; `device`, `gem` and `uaccess` are the kernel
layer. Since R2 the core also has fences, fence arrays and fence chains
(`fence`), wound/wait mutexes (`wwmutex`), reservation objects (`resv`),
sync objects (`syncobj`) and the merge rules of sync files (`sync_file`),
all plain logic over `ksync` (which is `ddi` in the kernel and `std` in
host tests) and tested on the host with real threads; the kernel layer
of R2 is `fence_dev` (the device's synchronisation state and the `sync`
kstat), `anon_fd` (file descriptors for dma-bufs, sync object files and
sync files, over illumos-ddi's private tier), `syncobj_ioctl` and
`prime`. Since R5b `cfence` holds Linux's fence rules on the facade's
`struct dma_fence` (with `spinlock`, the facade's `spinlock_t`),
`managed` the `drmm_*` resources of a `struct drm_device`, `refcount`
the reference count kept in a C structure, and `print` the rules of the
facade's logging; all four are tested on the host.

`examples/rdrmnull` is a DRM driver without hardware. It registers one
device with a card and a render node, has dumb buffers in system memory,
sync objects with timelines, and test-only private ioctls (movable
objects for the refault experiment, a reference dropped from a timer,
since R2 fences on a test timeline that a soft interrupt signals after a
delay and a reservation wait), and is the kernel module the core is
tested with, as well as the template for a real driver. Since R3 it also
has a virtual display (`examples/rdrmnull/src/kms.rs`) and two more
test-only ioctls: one reports what its software scanout shows, the other
keeps a reference to an object across the close of every file, so that
the tests can check that a refused detach leaves the device working.

`xtask` builds and links kernel modules and checks interfaces (see below).
`tools/uapi` regenerates `drm-uapi` and holds the C half of the layout
check. `tools/test` holds the libdrm test programs of R0 to R3 and the
scripts that link, load, test and unload `rdrmnull` on an illumos machine
(`run-r1-test.sh` runs the R0, R1 and R2 tests, `run-r3-test.sh` runs the
R0, R1 and R3 tests and `modetest`). `crates/drm-core/tests/edid` holds the
EDID test blobs and `gen_edid.py`, which synthesizes them.
`docs/anon-fd.md` records how file descriptors are made for dma-bufs,
sync objects and sync files.
`packaging/devlink.tab` is the `/dev/dri` link rule for packages.

`modules/drm_core` is the kernel module `misc/drm_core` (crate `drm-module`): the core
monomorphised once, behind the C facade, plus the table of DRM device
numbers every driver shares (`misc/drm_minor` until R5a; see *Device
numbers across drivers*). `include/drm` holds the facade's C headers (MIT;
Linux's names), `include/drm/drm_facade.mapfile` the facade's symbols for
a driver's `-z defs` link and `modules/drm_core/drm_core.mapfile` the module's
symbol scope. `crates/drm-facade-sys` is the facade as Rust declarations
(bindgen), `crates/drm-facade` the safe layer the Rust drivers use, and
`drivers/cdrmnull` the C test driver (since R5b with a GPU-like fence
timeline, managed resources and the fence-ops, handle-walk and printing
self-tests that `tools/test/drm_r5b_test.c` and `run-r5b-test.sh`
drive). `tools/facade` regenerates the
bindings, both mapfiles and the layout tables, and holds the C half of the
layout check; `modules/drm_core/ctf` the CTF shadow of the facade structures.
`packaging/drm-core.p5m` packages the module, `packaging/cdrmnull.p5m` the C
driver. `tools/drmadm` holds `drmadm(8)`, which selects the legacy or this
stack for boot (`packaging/drmadm.p5m`), and `tools/test/run-g7-test.sh`
tests both stacks on one machine; `docs/x230.md` is the operator note for
the ThinkPad X230.
`tools/test/run-g6-test.sh` attaches two `rdrmnull` devices and
`virtio_gpu` together and runs everything on each, `drm_g6_test.c`
checks the argument size rule, and `drm-test-lib.sh` holds the shell
functions the run scripts share.

`drivers/virtio-gpu` is the virtio-gpu driver of R4 (see *R4* under
*Decisions*): `gpu` (requests, completions, fences, contexts), `object`
(GEM objects with host resources), `ioctl` (the `DRM_IOCTL_VIRTGPU_*`
ioctls), `kms` (the display), `proto` (the device protocol) and host
tests of the pure logic (`cargo test -p virtio_gpu --no-default-features`).
`tools/test/drm_r4_test.c` and `run-r4-test.sh` test it on a guest,
`packaging/virtio-gpu.p5m` and `packaging/xorg.conf.d/20-virtio-gpu.conf`
package it. `tools/uapi/include/virtgpu_drm.h` is the Linux v6.12 uAPI
header the bindings come from, `crates/drm-uapi/src/virtgpu_compat.rs`
the older layouts, and `tools/uapi/virtgpu_compat_layout.c` their gcc
check.

`carried/` holds the Linux helper code that C drivers call and the
Rust core does not provide (R5e), carried as Linux C: per helper a
directory with the Linux files vendored unmodified at the revision of
`carried/linux.pin` (`<helper>/linux/`, MIT or dual MIT only, listed with
SHA-256 and licence in `<helper>/SOURCES`), the illumos patch series
(`<helper>/patches/series`) and the self-tests (`<helper>/tests/`).
`carried/fetch-and-patch` checks the vendored files (with `-f` against the
pinned Linux revision, with `-u` it refreshes them) and applies the series;
`carried/build.sh` builds `misc/drm_helpers` (`carried/drm_helpers/`) and
the test driver `drm_helpers_test` (`carried/test/`, run by
`tools/test/run-r5e-test.sh`); `carried/host-test.sh` runs the self-tests
as a userland program. `linuxkpi/` holds the Linux kernel interfaces the
carried code compiles against (`linuxkpi/include/{linux,asm}/*.h`,
`linuxkpi/lib/`); see *R5e: compatibility headers* for how they
relate to R5c's compatibility layer. `packaging/drm-helpers.p5m` packages
the module.

## Decisions

**Dependency on illumos-ddi.** `ddi-sys` and `ddi` are a git dependency on
<https://code.aopc.cloud/illumos/illumos-ddi.git>, pinned by revision in
the workspace `Cargo.toml`. The pinned revision is the `drm-r0` branch
(`bfc7ff7b2a`): illumos-ddi `main` (`eb18ab93b7`) plus clone-open support
in the `CharDevice` glue (`CharDevice::clone_open`, commits `8463d9da92`,
`f36d47958e` and `e019d5bd3c`) and the `devmap(9E)` entry point
(`CharDevice::devmap` and `module! { ops: [char, devmap] }`, commits
`94680b671c`, `77df4ce177` and `bfc7ff7b2a`); illumos-ddi pull request
<https://code.aopc.cloud/illumos/illumos-ddi/pulls/1>. Per-file DRM
state is impossible without clone opens, because a character device
otherwise sees only the last `close(9E)` of a device number, and GEM
mmap needs `devmap(9E)`, `ddi_devmap_segmap(9F)` and `D_DEVMAP`; both
changes belong in the framework glue rather than in a copy of it. Anything
else the safe `ddi` crate does not wrap yet (copying ioctl structures that
contain pointers, the `devmap_callback_ctl(9S)` callbacks, `ddi_umem`,
`vmem` and `id_space`) is done with `ddi-sys` inside the core
(`drm_core::uaccess`, `drm_core::gem`).

**The private tier (R2).** File descriptors for dma-bufs, sync objects and
sync files need kernel interfaces without a manual page (`falloc`, `setf`,
`getf`, `makespecvp`, `VOP_OPEN`, ...). They come from illumos-ddi's
private tier, the crate `illumos-kernel-private-sys` pinned to gate commit
`cda5b9a49f`, through the safe wrapper `ddi::private::fd`; the `drm`
crate enables the `ddi` feature `unstable-gate-cda5b9a49f` for it, and no
private symbol is declared anywhere else. R2 also needs from illumos-ddi
the `segmap(9E)` check (`CharDevice::segmap_check`), and, like R3, the
`chpoll(9E)` entry point: a sync file keeps its `pollhead` in a
`ddi::PollBox`, registered with `PollHead::registration_unchecked`
because the file, not the device, owns it, and sync object files, which
are always ready, register a head of the device that is never woken.
`makespecvp` lives in the specfs module, so a driver module is linked with
`-N fs/specfs` (`cargo xtask link` takes it from the private crate's kmod
metadata; the test scripts and CI name it).

**xtask is vendored.** `xtask/src/main.rs` and `xtask/src/kernel_check.rs`
are copied from illumos-ddi at `eb18ab93b7`, with a header comment that
lists the changes: `deps` and `kernel-check` read the full `cargo metadata`
graph, because the `-sys` crates come from the git dependency rather than
from this workspace. `lint-man` is not vendored, since this repository has
no `-sys` crate of its own. `uapi-dump` and `uapi-check` are new. Vendoring
was preferred to running illumos-ddi's xtask from the git checkout because
that xtask only looks at its own workspace. When the pinned illumos-ddi
revision moves, compare the two copies.

**uAPI generation.** `tools/uapi/generate.sh` downloads libdrm 2.4.109 (the
release gfx-drm builds, checked against the same SHA-256), applies gfx-drm's
illumos patch to `include/drm/drm.h` (`tools/uapi/patches`, copied from
gfx-drm `d109414c4f`; it makes `drm_handle_t` 64 bits wide on illumos), and
runs bindgen for `x86_64-pc-solaris2.11` against the illumos-gate system
headers. bindgen cannot expand `DRM_IOWR(nr, type)`, so
`tools/uapi/gen_uapi.py` generates the ioctl numbers from the same headers,
together with the two halves of the layout check. The generated files are
committed. The patched header is the ABI of the installed libdrm, because
gfx-drm compiles libdrm against it.

**Layout proof.** `tools/uapi/drm_layout.c` prints the size of every
structure and union, the offset of every named member and the value of
every `DRM_IOCTL_*` macro; built with gcc on illumos it gives
`tools/uapi/drm_layout.oibldofgate.out`. `cargo xtask uapi-check <file>`
computes the same facts from the Rust definitions and compares them line
by line: 103 sizes, 448 offsets and 115 ioctl numbers, no differences.
`crates/drm-uapi/src/abi.rs` (generated by `tools/uapi/gen_abi.py` from
the gcc output) turns the values for the structures the core and the
planned drivers need into compile-time assertions, on top of the
assertions bindgen emits for every structure.

**Module structure (deviation from the R0 work item).** The work item asks
for a `drm` kernel module. Here the core is a library crate linked
statically into each driver module, and `rdrmnull` is the module that is
built and loaded. A separate `misc/drm` module that exports the core to
other kernel modules is deferred to R5, because exporting Rust symbols
across kernel modules needs a stable, versioned C ABI (mangled Rust
symbols and generic code do not cross module boundaries), and designing
that ABI is exactly the R5 C-facade work. With one Rust driver planned
before R5, static linking costs nothing. The one exception since G6 is
the table of device numbers, `misc/drm_minor`, which every driver module
depends on (see *Device numbers across drivers*); it is the first piece
of that C interface. R5a made `misc/drm_core` the core's one module; see
*R5a*.

**Node type (deviation from the plan).** The minor nodes have the node type
`ddi_display:drm` (`DDI_NT_DISPLAY_DRM`), not plain `ddi_display`. The
plain type would make devfsadm's `misc_link` module add `/dev/fbs/card0`,
`/dev/fbs/renderD128` and a `/dev/fb<N>` console framebuffer link for the
nodes. `ddi_display:drm` is what the gfx-drm C driver uses, and
OpenIndiana's `driver/graphics/drm` package delivers the devfsadm module
`SUNW_drm_link`, which creates `/dev/dri/card0` and `/dev/dri/renderD128`
from the minor names. Where that module is missing, `packaging/devlink.tab`
has the equivalent `/etc/devlink.tab` rule. On the test machine the module
was present, so no `devlink.tab` change was needed.

**Minor numbers.** Node minors are `N` for `card<N>` and `128 + N` for
`renderD<128+N>` in the low nine bits, as in gfx-drm, where `N` is the
device number (below). Every open is a clone
open with a clone id from 1 to 511 in bits 9 to 17, the bits the illumos
libdrm patch strips (`0x3fe00`) to map an open file back to its `/dev/dri`
node. A device therefore has at most 511 open files at a time; the next
open fails with `EMFILE`.

**Device numbers across drivers (G6).** Until G6 the device number was the
driver's instance number, so every driver's first device was `card0` and
`renderD128`, and `rdrmnull` and `virtio_gpu` attached together claimed
the same `/dev/dri` links. Now every device of every driver built on the
core takes the lowest free number of 64 from one table when it attaches
and gives it back when it detaches. The table lives in the small kernel
module `misc/drm_minor` (`modules/drm-minor`), which exports two C
functions, `drm_minor_alloc(owner)` and `drm_minor_free(id, owner)`
(declared for Rust by `crates/drm-minor-sys`); every driver module is
linked with `-N misc/drm_minor` (the `[package.metadata.kmod]` table of
`drm-minor-sys` gives it to `cargo xtask link`). A number stays with its
device while it is attached, whatever else attaches and detaches; a
driver that unloads and loads again gets the lowest free numbers, its old
ones if nobody took them. Each driver module records which of its
instances holds which number (`drm_core::minor::bind`), and
`drm_core::minor::instance_of_minor` reads that record, so the instance
number is free (no longer limited to 63). The table is 64 atomic words
holding the owner (the device's `dev_info_t`) or 0: both calls are
lock-free and never block or allocate, and a free by the wrong owner, of
a free number or out of range changes nothing and is logged, where an
`id_space(9F)` `id_free` of an unallocated number would be a kernel
panic. `misc/drm_minor` is linked with an exports mapfile and
`ld -B reduce`, so it exports only `_init`, `_fini`, `_info` and its two
functions (checked by CI and the test scripts). The kernel keeps a
module loaded while a module that depends on it is loaded, so the table
outlives every number in it for any order of loads and unloads;
`_fini` also refuses while a number is taken, which only a driver that
leaked one could cause. Since R5a the table is part of `misc/drm_core`
(`drm_core::slots`); `drm_dev_register()` takes the number, the two C
functions are gone, and the behaviour is the same.

Alternatives: a registry that the first driver module creates and
publishes through a well-known symbol does not work, because a module
resolves symbols only from `unix`, `genunix`, itself and its `-N`
dependencies (illumos-ddi `docs/DESIGN.md` section 4: a symbol resolves
from the module that exports it), so the others would have to look it up
at run time (`ddi_modopen(9F)`), and it would die with the module that
made it while other drivers still hold numbers from it. Fixed ranges per
driver (virtio from 0, rdrmnull from 32) waste the 64 numbers and
collide with the next driver; scanning the device tree for existing
`card<N>` minor names at attach time races between concurrent attaches.
A separate module is also what Linux does (the numbers belong to
`drm.ko`), and it is the first piece of the C interface R5 needs.

Deviations from Linux: Linux 7.3's `drm_minor_alloc` takes the lowest
free index of one global table for each node type separately (primary
0 to 63, render 128 to 191, then an extended range from 192), so a
Linux device's card and render numbers need not agree. Here a device has
one number for both, `card<N>` and `renderD<128+N>` (the illumos libdrm
pairs a device's nodes by `/devices` path, so it does not depend on
this); and there is no extended range, because the clone
layout gives a node minor nine bits, so a 65th DRM device fails to attach
("all 64 DRM device numbers are in use"). gfx-drm's C drivers (`drm`,
`i915`) still name their nodes after the instance number and do not use
this table, so an i915 and an illumos-drm driver attached together can
still both claim `card0`; that ends when gfx-drm calls
`drm_minor_alloc` too (R5).

A device's `/dev/dri` links outlive a detach (until `devfsadm -C`), and
opening one attaches the device again through devfs, which then takes the
lowest free number: its old one unless another device took it in the
meantime, in which case the link names a minor node that no longer
exists. The same holds when the system detaches idle devices on its own
(the drivers do not set `ddi-no-autodetach`): if two idle devices are
detached and come back in the other order, their numbers are swapped
until devfsadm renews the links. Linux numbers devices once per driver
bind as well; a stable name for a particular device is its
`/devices` path (libdrm's `drmGetDevice2` reports it), not `card<N>`.
Whether the drivers should keep their devices attached is open.

**GEM objects and references (R1).** A driver's buffer type implements
`drm_core::gem::GemObject` (size, the `ddi_umem` cookie its mappings show,
optional hooks for movable objects); the core wraps it in a counted
`GemInner` with its handle count, FLINK name and mmap offset, and hands out
`GemRef`s. The count is `drm_core::karc::KArc`, not `alloc::sync::Arc`: its
construction is fallible with the context token, and `KArc::release`
returns the last reference's value in place instead of dropping it, which
is what makes the interrupt-context rule possible without allocating:
`GemRef::put` (with `Sleepable`) destroys the object at once,
`GemRef::put_atomic` (with `Atomic`) parks it on a lock-free list linked
through the object and dispatches a task-queue drain with `DDI_NOSLEEP`;
if that dispatch fails, the next thread-context entry point or `detach`
drains. A `GemRef` dropped implicitly takes the interrupt-safe path. The
rule and the list are tested on the host from eight threads, half of them
in simulated interrupt context, and in the kernel by a reference dropped
from a `timeout(9F)` callback.

**Handles, names and offsets (R1).** Handle numbers come from a per-file
`id_space(9F)` and names from a per-device one, first fit
(`id_allocff_nosleep`), so numbers start at 1 and the lowest free one is
reused, as Linux's `idr_alloc(…, 1, 0, …)` does; the number-to-object maps
are `alloc::collections::BTreeMap`s behind the file's and the device's
mutex. BTreeMap needs a global allocator, so driver modules enable the
`ddi` crate's `global-alloc-sleep` feature (always `KM_SLEEP`) and the core
inserts only with the `Sleepable` token. As in Linux, the handle count and
the name change only with the name table locked, a name disappears with
the object's last handle, and the name table is locked before a file's.
Fake mmap offsets are page ranges from a `vmem(9F)` identifier arena over
Linux's 64-bit range (256 MiB up, 64 GiB long), one range per object,
assigned at the first `MAP_DUMB` and freed with the object; `mmap(2)` must
start at an object's offset and stay within it.

**Mapping (R1).** `mmap(2)` goes through `devmap_setup(9F)` to the core's
`devmap(9E)`, which finds the object, takes a reference and calls
`devmap_umem_setup(9F)` with callbacks for map, dup, unmap and access.
`devmap_map(9E)` refuses private mappings and files that hold no handle to
the object (Linux's `drm_vma_node_is_allowed`); `devmap_dup(9E)` (fork)
and `devmap_unmap(9E)` (munmap, exit, splits) take and drop references for
each piece; `devmap_access(9E)` defaults to `devmap_default_access(9F)`.
Every piece a partial `munmap(2)` leaves gets private data holding its own
reference, whatever else fails, because the framework keeps the piece (and,
for the middle case, a copy of the old private-data pointer). Such a piece
is *unsettled* until its next fault: the framework adjusts its offset into
the memory only after `devmap_unmap(9E)` returns, so it must not be
remapped before then; a move invalidates it, and its next fault (which
holds the address-space lock the `munmap(2)` held) calls
`GemObject::mapped` with the final geometry.
The framework can free a mapping handle after a successful `devmap(9E)`
without calling back (a private mapping fails that way), so the reference
is parked by handle and thread and dropped by the next entry point of the
same thread or by `close(9E)`. A mapping keeps its file open, so objects
cannot outlive their device. Since R2 the core refuses a mapping in `segmap(9E)`
(`CharDevice::segmap_check`), before the devmap framework creates a
segment, with Linux's errors: `EINVAL` for a private mapping or an offset
that is not an object's, `EACCES` for a file without a handle. (In R1 the
refusal came from `devmap_map(9E)`, which `devmap_setup(9F)` reports as
`ENXIO` and which leaves the segment in the process until it exits.)
Mappings are cached; write-combining needs the private tier.

**System memory and DMA (R1).** `drm_core::gem::SysMem` is locked kernel memory
from `ddi_umem_alloc(9F)` with `DDI_UMEM_SLEEP` (there is no
`DDI_UMEM_PAGELOCK`; non-pageable is the default), which the function
zero-fills itself (`vm/seg_dev.c` `bzero`s every non-pageable allocation).
With `Driver::DMA_ATTR` set, the memory is bound with
`ddi_dma_addr_bind_handle(9F)` against the device's own node and
`SysMem::sg_iter` yields the cookies as the scatter/gather table; the core
checks that the cookies cover the object exactly. `rdrmnull` is a pseudo
device and the pseudo nexus refuses DMA handles, so its objects are not
bound; a trial run with `rdrmnull` as a child of the root nexus bound every
object (300 cookies for a 1,228,800-byte buffer, sizes summing up), but
such a node leaves its instance in `/etc/path_to_inst` after `rem_drv`, and
binding against `ddi_root_node(9F)` from the pseudo node panicked the test
machine, so neither is used. Both can be repeated on purpose with the DMA
drill, a test aid: `Driver::gem_dma` chooses the binding at attach time,
and `rdrmnull` reads it from the `dma-mode` property
(`tools/test/rdrmnull-dma-self.conf` binds against its own node under the
root nexus, `tools/test/rdrmnull-dma-root-node.conf` reproduces the panic;
`RDRMNULL_CONF=<file> ./run-r1-test.sh`, only in a disposable VM with a
dump device).

Since illumos-ddi's own-node rule (its `docs/DESIGN.md` section 5.12), a
DMA handle can only be allocated from the `ddi::OwnNode` that
`ddi::Driver::attach` receives. `drm_core::Device::attach` takes that node and
hands it to the GEM state, and `SysMem::new` takes `&OwnNode`; a `DevInfo`
no longer allocates DMA handles, and illumos-ddi refuses the root node and
nodes of other drivers with `DmaError::NotOwnNode` (`ENXIO`). The
`root-node` drill therefore goes around illumos-ddi with raw
`ddi_dma_alloc_handle(9F)` calls through `GemDma::against`, which exists
only with the test-only cargo feature `panic-drill` of `drm` and
`rdrmnull`, off by default. A production build of `rdrmnull` cannot name
another node at all: it refuses `dma-mode="root-node"` at attach with a
warning, and its module has no reference to `ddi_root_node`. The drill
module is built with `cargo xtask build rdrmnull --features panic-drill`.
The minor nodes are created from the same `OwnNode`, which illumos-ddi
requires for them too.

A failed DMA allocation or binding makes the creating ioctl fail with the
errno Linux's DRM gives a failed object allocation (`drm_core::dmaerr`):
`ENOMEM` when DMA resources are short or the memory cannot be mapped for
the device (`DDI_DMA_NORESOURCES`, `DDI_DMA_TOOBIG`, `DDI_DMA_NOMAPPING`),
`EINVAL` for attributes that can never be satisfied, `ENXIO` for a node
illumos-ddi's own-node guard refuses, and `EIO` for framework faults a
fresh handle cannot produce. Before 28 September 2026 `DDI_DMA_NOMAPPING`
reached userland as `EFAULT`, which in a create ioctl reads as a bad user
pointer. `tools/test/rdrmnull-dma-self-tiny.conf` lowers the largest DMA
transfer of the `self` drill to one page, and
`tools/test/drm_dma_errno_test.c` checks that a one-page dumb buffer binds
and a four-page one fails with `ENOMEM` and leaves nothing behind in the
`gem` kstat; it did on 28 September 2026 (illumos `cda5b9a49f`).

Object memory is locked, so a driver sets
limits per object and per device (`MAX_OBJECT_SIZE`, `MAX_MEMORY`; 256 MiB
and 1 GiB by default, 16 and 64 MiB in `rdrmnull`); requests above them are
`ENOMEM`. The device budget is charged before the memory is allocated
(`GemCore::reserve`, `GemCore::create_reserved`), so concurrent requests
cannot exceed it even for a moment.

**Dumb buffers (R1).** `MODE_CREATE_DUMB` follows Linux's
`drm_mode_create_dumb`: any non-zero `bpp` up to `U32_MAX - 8`, counted as
`ceil(bpp / 8)` bytes per pixel, non-zero dimensions, `flags` ignored, and
Linux's 32-bit size limit; every overflow is `EINVAL`. The default layout
(`Driver::dumb_layout` may replace it) pads rows to 64 bytes and rounds the
size up to a page. `MAP_DUMB` and `GEM_CLOSE` ignore `pad`, as Linux does,
since userland has never been required to clear it. `DRM_CAP_DUMB_BUFFER` is 1 for drivers with `Features::GEM` and
`Features::DUMB`, and `DUMB_PREFERRED_DEPTH` and `DUMB_PREFER_SHADOW` report
the driver's `ModeConfig`; `ADDFB2_MODIFIERS` reports the driver's
`ModeConfig` since R3.

**The refault experiment (R1).** Movable objects (TTM-like, needed for VRAM
drivers in R5) must invalidate user mappings when their memory moves and
reload them afterwards. `rdrmnull` has test-only objects with two backings
and an ioctl that switches the backing of every mapping piece with
`devmap_umem_remap` and invalidates it with `devmap_unload(9F)`; their
`devmap_access(9E)` reloads with `devmap_load(9F)`. It works: after
`devmap_unload` alone, the next read faults into `devmap_access` (one fault)
and sees the same memory; after a move, a read through the existing mapping
faults (one per page) and sees the other backing's contents; moving back
shows the first backing's contents unchanged; one move redirects the
mappings of a parent and a forked child. Findings for R5:
`devmap_umem_remap` has no manual page and is not in the `ddi` mapfile (it
is declared in `rdrmnull` only; R5 needs it in the private tier or
documented upstream); it requires `DEVMAP_ALLOW_REMAP` at setup, and the
framework then refuses soft-locking such mappings (raw I/O from them
fails); `devmap_unload(9F)` counts offsets from the mapping piece's own
user offset, so the manual's "offset 0, length 0 for the whole mapping"
fails with `EINVAL` for GEM offsets; a piece split off by a partial
`munmap(2)` must not be remapped until the framework has finished it (see
*Mapping*), so a move only invalidates such a piece and its next fault
remaps it. The test covers a head and a middle `munmap(2)` of a movable
mapping with per-page patterns, before and after the pieces settle.

**Fences (R2).** `drm_core::fence` is Linux's `dma_fence`: a fence on a
context with a sequence number, compared wraparound-safe per context,
signalled once with an optional error and a timestamp. Signalling never
allocates and runs the callbacks, which are allocated ahead of time
(`FenceCb`), outside the fence lock in the signaller's context; a callback
may return a fence to signal next, and the loop runs such cascades
iteratively, so the chain of a long timeline cannot overflow the kernel
stack. `remove_callback` unlinks a callback that has not started or waits
until it has finished. Waits take an absolute `CLOCK_MONOTONIC` deadline
(the clock of `gethrtime(9F)`) and end with `ETIME` or `EINTR` as on
Linux. Fence arrays (all or any, the first error) and fence chains (the
points of a timeline) complete through such callbacks; a chain point
drops its link to the previous one as soon as that has signalled. The
context rules are types: signalling and dropping take `defer::Ctx`
(`Sleepable` or `Atomic`), waiting takes `Sleepable`, and a fence whose
last reference goes in interrupt context is parked on its domain's list
and destroyed by a task queue, because `mutex_destroy(9F)` is only
allowed in user or kernel context. Every lock a signal takes is created
with the interrupt priority the driver declares
(`Driver::fence_priority`; `rdrmnull` uses its soft interrupt's), and
high-level priorities are refused. As on Linux, every fence must
eventually signal: arrays and chain points are kept alive by the
callbacks on their members, and a driver signals what is left when the
device detaches (`rdrmnull` with `ECANCELED`, in `before_detach`). Detach
is refused while fences remain; like every detach check, this one runs
before anything is torn down, so a refused device keeps its display and
its fence timeline.

**Reservations and wound/wait mutexes (R2).** Every GEM object has a
reservation (`drm_core::resv`, Linux's `dma_resv`) holding fences with the
usages kernel, write, read and bookkeep. Adding a fence needs the
reservation's wound/wait mutex (`drm_core::wwmutex`), and readers iterate
under a short internal mutex (no RCU). The wound/wait mutex follows the
wait-die rule of Linux's reservation class: a transaction waits for
younger holders and backs off with `EDEADLK` from older ones while it
holds other mutexes of the class, then sleeps on the contended one with
`lock_slow` and starts over; `lock_all` implements the loop, and a host
test has two threads lock two mutexes in opposite orders at the same
moment.

**Sync objects (R2).** `drm_core::syncobj` is Linux's `drm_syncobj`: a binary
fence slot or a timeline of chain points, with Linux's point lookup and
query. `SYNCOBJ_WAIT` and `SYNCOBJ_TIMELINE_WAIT` wait for all or any of
several objects, for submissions (`WAIT_FOR_SUBMIT`) or only for them
(`WAIT_AVAILABLE`), with fence callbacks and submission waiters pointing
at one waiter on the waiting thread's stack that the cleanup removes
before it goes. The ioctls check their arguments and answer with the
errnos Linux does (`drm_core::syncobj_ioctl` lists the rules: which `pad`
fields are checked, which unknown handles are `ENOENT` and which
`EINVAL`). Deviations from Linux in R2, all refusals of input Linux
ignores or cannot allocate for: `TRANSFER` refuses flags other than
`WAIT_FOR_SUBMIT`, `PRIME_FD_TO_HANDLE` refuses non-zero `flags`, and
arrays of handles are limited to 65536 entries (`EINVAL` above; Linux
tries the allocation). Each file has its own handle table, freed at
`close(9E)`. Sync object files and sync files of one device can be
imported into another device of the same driver, which shares this code,
if both signal their fences at the same interrupt priority
(`Driver::fence_priority`; `ENOTSUP` otherwise, since the locks waiting
on the fences are created with the importer's); the imported objects keep
their device from detaching until they are gone. Files of other drivers, even ones built on this core, are `EINVAL`:
each driver module links its own copy of the core, and sharing objects
between copies needs the C interface of R5.

**File descriptors, PRIME and sync files (R2).** A dma-buf, an exported
sync object and a sync file are specfs files of an *anonymous minor* of
the driver, created inside the ioctl as `pipe(2)` makes its descriptors;
`close(9E)` of the minor drops the object, `chpoll(9E)` polls a sync
file, `devmap(9E)` maps a dma-buf, and imports find the object by minor.
[docs/anon-fd.md](docs/anon-fd.md) gives the design, the alternatives
and why. `PRIME_HANDLE_TO_FD` honours `DRM_CLOEXEC` and `DRM_RDWR` and
gives one dma-buf per object; `PRIME_FD_TO_HANDLE` returns the calling
file's existing handle (with no driver hook, since G8) or creates one,
for dma-bufs of the same device (`ENOTSUP` for another device of the
driver, because importing across
devices needs an attach and a scatter/gather mapping no driver has yet;
`EINVAL` for other files). Sync files answer `SYNC_IOC_MERGE` and
`SYNC_IOC_FILE_INFO`, dma-bufs `DMA_BUF_IOCTL_SYNC`: the start of CPU
access waits, interruptibly, for the object's reservation (the writers
before reading, everyone before writing), as Linux does; there is no
cache to flush, the memory is coherent. Polling a
dma-buf for its reservation is not implemented yet (`ENOTSUP`); nothing in
R2 needs it, and a compositor using implicit synchronisation will.


**Ioctl semantics.** The permission flags and the behaviour follow Linux
(`drm_ioctls[]`, `drm_auth.c`, `drm_getcap`, `drm_setclientcap`):

| Ioctl | Allowed for | Notes |
| --- | --- | --- |
| `VERSION` | all files, render too | strings copied up to the caller's lengths, full lengths returned |
| `GET_CAP` | all files, render too | `TIMESTAMP_MONOTONIC` 1; `PRIME` `IMPORT|EXPORT` for GEM drivers; `SYNCOBJ` and `SYNCOBJ_TIMELINE` 1 with `Features::SYNCOBJ` (and `SYNCOBJ_TIMELINE`); `DUMB_BUFFER` 1 with `GEM` and `DUMB`; `VBLANK_HIGH_CRTC` and `CRTC_IN_VBLANK_EVENT` 1, `ASYNC_PAGE_FLIP` and `PAGE_FLIP_TARGET` 0 (R3); `CURSOR_WIDTH/HEIGHT`, `DUMB_PREFERRED_DEPTH`, `DUMB_PREFER_SHADOW` and `ADDFB2_MODIFIERS` from the driver's `ModeConfig`; everything else `EOPNOTSUPP` for a driver without `MODESET`, unknown `EINVAL` otherwise |
| `SET_CLIENT_CAP` | primary | `STEREO_3D`, `UNIVERSAL_PLANES`, `ATOMIC` (needs `Features::ATOMIC`, also sets universal planes and aspect ratio, values 0 to 2), `ASPECT_RATIO`, `WRITEBACK_CONNECTORS` (needs `ATOMIC` first), values 0 or 1; Linux's refusal of `ATOMIC` = 1 to processes named `X*` needs the process name, which no committed DDI interface provides; not implemented |
| `GET_UNIQUE` | primary | empty until the master sets interface 1.1 or later or calls `SET_UNIQUE`, as `drmOpenByName` expects |
| `SET_UNIQUE` | authenticated master with `PRIV_SYS_DEVICES` | only the device's own bus ID, once (`EBUSY` after) |
| `SET_VERSION` | master | interface 1.0 to 1.4, driver version checked; versions reported back in every case |
| `GET_MAGIC` | primary | per-master namespace; the number stays with the file |
| `AUTH_MAGIC` | authenticated master | consumes the magic; unknown or consumed magic `EINVAL` |
| `GET_CLIENT` | primary | addition beyond the R0 work item, to make authentication observable: index 0 only (the caller), authentication state, the caller's pid, the anonymous uid, as on Linux |
| `SET_MASTER` | primary | needs `PRIV_SYS_DEVICES`, or a former master used by its opening process (`PRIV_SYS_DEVICES` is checked only if the second condition fails) |
| `DROP_MASTER` | primary | same permission, only by the current master |
| `GEM_CLOSE` | all files, render too | unknown handle `EINVAL`; `pad` ignored; `EOPNOTSUPP` without `GEM` |
| `GEM_FLINK` | authenticated (primary) | unknown handle `ENOENT`; one name per object, from 1 |
| `GEM_OPEN` | authenticated (primary) | unknown name `ENOENT`; returns a new handle and the size |
| `MODE_CREATE_DUMB` | primary | see *Dumb buffers*; `ENOSYS` without `DUMB` |
| `MODE_MAP_DUMB` | primary | any GEM handle; unknown `ENOENT`; the object's one fake offset |
| `MODE_DESTROY_DUMB` | primary | closes the handle; unknown `EINVAL` |
| mode setting (R3) | see *Mode setting (R3)* | `SETCRTC`, `CURSOR`, `CURSOR2`, `SETGAMMA`, `SETPROPERTY`, `OBJ_SETPROPERTY`, `PAGE_FLIP`, `DIRTYFB`, `SETPLANE`, `ATOMIC`, `ATTACHMODE`, `DETACHMODE` need the master; the others (getters, `ADDFB`, `ADDFB2`, `RMFB`, blobs, `WAIT_VBLANK`, CRTC sequences, `MODESET_CTL`) any primary file; none is allowed on render nodes (`EACCES`), as in Linux's `drm_ioctls[]`; `EOPNOTSUPP` for a device without `Driver::kms` |
| `PRIME_HANDLE_TO_FD` | all files, render too | see *File descriptors, PRIME and sync files*; flags other than `DRM_CLOEXEC`, `DRM_RDWR` `EINVAL`; unknown handle `ENOENT`; `ENOSYS` without `GEM` |
| `PRIME_FD_TO_HANDLE` | all files, render too | same device only; `flags` must be 0 |
| `SYNCOBJ_CREATE`, `DESTROY`, `HANDLE_TO_FD`, `FD_TO_HANDLE`, `WAIT`, `RESET`, `SIGNAL` | all files, render too | `EOPNOTSUPP` without `Features::SYNCOBJ`; Linux's argument checks and errnos (`drm_core::syncobj_ioctl`) |
| `SYNCOBJ_TIMELINE_WAIT`, `QUERY`, `TRANSFER`, `TIMELINE_SIGNAL` | all files, render too | also `EOPNOTSUPP` without `Features::SYNCOBJ_TIMELINE` |
| driver range `0x40`-`0x9f` | per `Driver::IOCTLS` | matched on the whole command value; the core checks the flags, then calls `Driver::ioctl` |

The first file opened on a card node while the device has no master
becomes master. Files of openers with `PRIV_SYS_DEVICES` (the illumos
counterpart of `CAP_SYS_ADMIN`) are authenticated from the start. As
Linux calls `capable()` lazily, the core calls `drv_priv(9F)` on the ioctl
path only for ioctls whose permission needs it (`SET_UNIQUE`, and
`SET_MASTER`/`DROP_MASTER` by a file that is not a former master of the
same process). Render
nodes never need authentication and cannot use the primary-only ioctls
(`EACCES`). A core command from a 32-bit process (recognised by
group and number, whatever its size) is `ENOTSUP`, because only the LP64
ABI exists; other commands from 32-bit processes are `ENOTTY` as usual.

**Argument sizes (G6).** Linux's `drm_ioctl()` (read at v7.3-rc5) finds a
core or driver ioctl by its number alone and copies the argument with
the caller's size `U` (from the command) and its own size `K`: it copies
`U` bytes in if both commands have `IOC_IN`, zeroes the rest of a buffer
of `max(U, K)` bytes, hands the handler the first `K`, and copies `U`
bytes out if both have `IOC_OUT`. A structure shorter than the kernel's
is therefore zero-extended and gets only its own bytes back. A structure
longer than the kernel's is not refused, whatever its extra bytes hold:
they are copied in (an unreadable byte is `EFAULT`) and copied back out
unchanged by an `_IOWR` command, come back as zeros from an `_IOR`
command, and are ignored by an `_IOW` command. Since G6 the core does
exactly this for the core and mode-setting ioctls
(`drm_core::ioctl::SizePlan`, checked against a restatement of Linux's code
for every table entry, direction and size; `drm_core::uaccess::CoreArg` applies
it), where it used to match the complete command value and answer any
other size with `ENOTTY`. The size field of an illumos command has eight
bits, so a size is known exactly below 256 bytes; every core and
mode-setting structure is smaller (the tests check each handler's
structure against the size its command carries). What stays
different from Linux: a command must have the table's direction (Linux
uses the intersection of both, here another direction is `ENOTTY`), an
unused number is `ENOTTY` (Linux: `EINVAL`), the handlers copy their
result out when they have one (Linux copies its buffer back after every
call, also a failed one; the handlers here undo work whose result cannot
reach the caller), and driver-private commands keep the exact match on
the complete value, each accepted size listed by the driver
(`virtio_gpu`'s `EXECBUFFER` and transfers, see *R4: uAPI*). The four
commands whose structures grew between the libdrm 2.4.109 headers this
uAPI comes from and libdrm 2.4.134 (`SYNCOBJ_HANDLE_TO_FD` and
`FD_TO_HANDLE`, 16 to 24 bytes, `SYNCOBJ_WAIT`, 32 to 40,
`SYNCOBJ_TIMELINE_WAIT`, 40 to 48) are served at both sizes with the
same results; the added `point` and `deadline_nsec` are carried through
unread, and the flags that would use them are unknown flags (`EINVAL`),
as on a Linux kernel from before them. libdrm 2.4.134's `drmIoctl()`
retries these four at the old size after `ENOTTY`; against this core the
retry never runs.

**Concurrency.** All mutable device state of R0 is one `DeviceState` behind one
`ddi::Mutex`. Each entry point performs its permission check and its state
change in one critical section; copies from and to user memory happen
outside it. `crates/drm-core/src/device.rs` gives the full argument. The state
machine is tested on the host from eight threads with an invariant check
after every step, and the kernel side by `tools/test/drm_r0_test.c`, whose
eight unprivileged threads open, authenticate and close files while the
root master drops and retakes master in another process. The GEM state of
R1 has its own locks (name table, then a file's handle table, then an
object's mapping list; the offset, file and pending tables as leaves) and
never drops a reference with one held; `crates/drm-core/src/gem.rs` and
`crates/drm-core/src/objects.rs` give the argument, the host tests drive the
handle and name rules and the reference counting from eight threads, and
`tools/test/drm_r1_test.c` runs eight threads of GEM ioctls and mappings
against the kernel. R2's state has its own locks, taken inside, never
around, the GEM and device locks: a file's sync object table and the
anonymous-file table (leaves), sync objects (whose chain points are built
under their lock), reservations (a wound/wait mutex, then an internal
mutex), and each fence's state mutex, taken from interrupt context at the
domain's priority. Fence callbacks run with no fence lock held. A file's
clone id is freed only after its GEM and sync object tables are gone, so
a concurrent open that reuses the id cannot find them (the R0 stress test
caught that race when R2 made `close(9E)` longer). The host tests drive
signalling against callback registration and removal, sync object waits
against submissions, and wound/wait locking from eight threads;
`tools/test/drm_r2_test.c` runs eight threads of sync object, fence, sync
file and PRIME round trips against the kernel.
Mode setting has two more locks, the mode-setting lock and the event
lock, described below.

**Mode setting (R3): structure.** KMS lives in `crates/drm-core/src/kms/`:
`modes` (the mode type, its uAPI conversion and validation, the VESA DMT
and CTA-861 tables, CVT, the mode list order), `edid` (base block and
CTA-861 extensions to a mode list), `formats` (fourcc codes, legacy
`ADDFB` formats, the `IN_FORMATS` blob), `props` (the standard properties
with Linux's names, types and flags), `objects` (one ID space for all mode
objects, the driver's `Layout`, blobs, framebuffers, `ADDFB2` checks),
`state` (object states, atomic updates, the `KmsDriver` hook trait),
`core` (the registry, the getters, framebuffer and blob lifetimes),
`atomic` (setting properties, check, commit), `legacy` (`SETCRTC`,
`PAGE_FLIP`, `SETPLANE`, `CURSOR`, `DPMS` and gamma as atomic updates),
`events` (vblank counters and per-file event queues), `ioctl` (the table
and its permissions), `dev` (the kernel layer: locks, argument copies,
`read(9E)`, `chpoll(9E)`, the `kms` kstat) and `soft` (a software vblank
source). Everything but `dev` and `soft` is plain data and host-tested
(EDID parsing against three synthesized blobs, CVT against the VESA
generator's values, atomic state duplication and rollback, every check's
errno, framebuffer and blob lifetimes, the event budget, with a mock
driver). A driver without display is unaffected: `Driver::kms` defaults
to `None`, and every mode-setting ioctl then fails with `EOPNOTSUPP`.

**Mode setting (R3): objects and state.** The driver describes its
pipeline once (`KmsDriver::layout`); the core gives every property,
plane, CRTC, encoder, connector, blob and framebuffer an ID from one
space, the lowest free number from 1, as Linux's `object_idr`. Each
CRTC, plane and connector has a current state; an atomic update copies an
object's state on first change and edits the copy, so dropping an update
is the rollback. The core checks what Linux's `drm_atomic_check_only` and
the atomic helpers check, with the same errnos (connector routing to an
encoder, `ACTIVE` without a mode, mode without connectors, events on a
CRTC that stays off, full mode sets without `ALLOW_MODESET`, plane CRTC,
format, modifier, `ERANGE` destinations and `ENOSPC` sources), then calls
the driver's check hooks per connector, CRTC and plane; the commit calls
its commit hooks (CRTCs that end off, planes, CRTCs that end on,
connectors) and swaps the states in. Framebuffers hold `GemRef`s; `RMFB`
and the close of the owning file turn off every plane that shows the
framebuffer and the CRTC whose primary plane that was, as Linux's
`atomic_remove_fb`, together with the other planes of that CRTC. Unlike
Linux, a file's close also turns off the legacy cursors it set (their
framebuffers belong to no file), because a cursor left on a dark CRTC
would keep its buffer, and so the device, referenced with no file open.
Blobs and framebuffers are counted; one that userland
removed stays findable until no state uses it, then its ID is freed.
Connectors are probed when the device attaches and when the master asks
`GETCONNECTOR` with `count_modes` 0; the core parses the EDID the driver
returns, publishes it as the `EDID` blob, and a connected connector without
modes gets the DMT modes up to 1024x768. Clients without the aspect-ratio
capability see modes without aspect flags and without the duplicates that
leaves.

**Mode setting (R3): events and vblanks.** Each file has an event queue
with Linux's 4096-byte budget (128 events); an event is allocated and
charged by the ioctl that asks for it, `ENOMEM` if it does not fit.
`read(2)` returns whole events only (a buffer too small for the next one
reads 0 bytes), blocks for the first unless the file is non-blocking
(`EAGAIN`); `poll(2)` reports `POLLIN` while events are queued, through
the new `chpoll(9E)` support of illumos-ddi and one pinned pollhead per
clone slot for the device's lifetime. A commit arms a flip on every active
CRTC it touches after the driver's hooks ran, and the next vblank completes
it and sends its events, so an event never precedes the hooks (it may come
a frame late if a vblank fell between). A non-blocking commit or
`PAGE_FLIP` on a CRTC with a pending flip is `EBUSY`; a blocking commit
waits for it first and for its own flip afterwards (one second at most
each). A driver reports vblanks through its `VblankHandle` with the
`gethrtime(9F)` time (`TIMESTAMP_MONOTONIC`). Waits are kept sorted by
their sequence and the event lists append in O(1), so a vblank costs only
the events it completes; concurrent readers of one file take turns so
that its events keep their order, without holding up other files. `rdrmnull` uses
`kms::soft::SoftVblank`, a 10 ms `ddi_periodic(9F)` callback that reports
every frame boundary of the mode with its exact time: 60 Hz on average,
each vblank delivered up to 10 ms late. Events of a file that closes are
unlinked and freed at once, so a flip or wait completing later finds
nothing of it. Lock order: the mode-setting lock, then the event lock; the
vblank path takes only the event lock and wakes pollers after dropping it.

**Mode setting (R3): deviations and open items.** Limits Linux does not
have, because blobs, framebuffers and events are locked kernel memory that
any open file may create: a blob is at most 1 MiB, a file holds at most
256 blobs and 4 MiB of blob data, the device 4096 blobs and 16 MiB; a file
holds at most 256 framebuffers, the device 4096; the device keeps at most
8192 events queued or pending over all files (each file's 4096-byte budget
is Linux's); all of these fail with `ENOMEM`. The current master is exempt
from the device-wide bounds (its per-file bounds still apply), so that
other clients cannot starve the display server. One `MODE_ATOMIC` names at
most 1024 objects and 16384 properties (`EINVAL`). The fence properties
`IN_FENCE_FD` and `OUT_FENCE_PTR` exist so that their IDs are known, but
no object carries them and setting them is `EINVAL`; connecting them to
R2's sync files is open. Not implemented: leases, asynchronous and targeted page flips, the
atomic colour-management properties (legacy `GETGAMMA`/`SETGAMMA` work),
`VRR_ENABLED`, writeback connectors, rotation, `zpos`, `TILE`, damage
clips. `DIRTYFB` goes to the driver (`KmsDriver::dirty_fb`, since R4)
with the planes that show the framebuffer and the clip rectangles; a
driver whose scanout reads the framebuffer itself keeps the default,
`ENOSYS`, as Linux answers for a driver without damage tracking. The event lock has no interrupt priority, so
`VblankHandle::vblank` demands the `Sleepable` token (thread, task queue or
level-0 periodic context); a driver with a hardware vblank interrupt needs a
priority-aware lock and an `Atomic` entry point, which come with the first
such driver. `detach` checks everything before it tears anything down
(open files, framebuffers still referenced, GEM objects left, since R2
fences and sync objects alive, after the
driver's `before_detach` has dropped what it holds for itself); a refused
device keeps its mode-setting state and its vblank source and works on.
Only when every check has passed does the driver stop its sources
(`before_teardown`) and the core release the KMS and GEM state. The refusal of `ATOMIC` to Xorg by process name stays open.

**R5a: module layout.** `misc/drm_core` (`modules/drm_core`) is the only module
that compiles `drm-core` with its `kernel` feature. It instantiates the
core's generic device once, over a facade driver (`CDriver`) that forwards
every hook of the core's `Driver` trait to a driver's C table
(`struct drm_driver`), with a facade object type (`FObj`) for GEM objects
and a facade KMS driver (`CKms`). A driver module registers its devices
with `drm_dev_alloc()` and `drm_dev_register()` and points its `cb_ops`
at the module's entry points (`drm_open` to `drm_segmap`, `DRM_CB_OPS`),
which find the device by its device number in the module's device table
and run illumos-ddi's character glue on it (`ddi::module::CharTable` and
`table_glue`, added on illumos-ddi's `r5a-char-table` branch). Everything
else a driver does is a call of a facade function. The module also owns
the DRM device numbers, the fence domain of fences that belong to no
device (`dma_fence_init()` on a driver's own structure; kstat
`drm:0:sync`), and a per-thread record of the ioctl in progress, through
which `drm_copy_from_user()`, `drm_copy_to_user()` and the
file-descriptor functions find the caller's argument. `_fini` refuses
while a device number is taken or a fence of the module's domain lives,
and the kernel keeps the module while a driver that depends on it is
loaded.

**R5a: the module's name, and never beside gfx-drm.** The module is
`misc/drm_core`, not `misc/drm`: OpenIndiana's gfx-drm delivers
`/kernel/misc/amd64/drm`, the C core of its legacy drivers, in the
package `driver/graphics/drm`, which libdrm requires. The two packages
(`driver/graphics/drm` and `driver/graphics/drm-core`) install together,
but the two stacks must never be loaded at the same time, and loading
both by accident must be impossible: `misc/drm_core`'s `_init` refuses
(`EBUSY`, with the warning "drm_core: the legacy DRM core misc/drm
(gfx-drm) is loaded; ...") while the legacy `misc/drm` is loaded. The
DDI has no way to ask whether a module is loaded without loading it
(`ddi_modopen(9F)` loads it), so the check walks the module list under
`mod_lock` (`modctl.h`, private, bound in illumos-ddi's private tier at
the pinned gate commit) and matches the module name `drm`, which is how
the framework itself identifies a module (a module's file name is
whatever path its latest load used, `misc/drm` or
`/kernel/misc/amd64/drm`); the wrapper is
`ddi::private::module_loaded(name, &Sleepable)` and takes no hold, so a
symmetric guard cannot deadlock against it. Its answer is a snapshot, so
the opposite guard belongs in gfx-drm's `misc/drm`, which refuses while
`misc/drm_core` is loaded since G7 (see *Running beside the legacy
stack*, which also describes `drmadm(8)` and the rule for a module left
in memory by a failed `_init`).
The guest test loads the legacy module from the installed
`driver/graphics/drm`, checks that `misc/drm_core` refuses with the
warning, unloads the legacy module and loads `misc/drm_core`. The facade
keeps Linux's symbol names (`drm_dev_register` and so on), some of which
gfx-drm's core exports too, so an unqualified fbt probe such as
`fbt::drm_dev_register:entry` names functions of both modules; name the
module (`fbt:drm_core:drm_dev_register:entry`). The fence domain's
kstat keeps the name `drm:0:sync` of R2. The name follows G6's
`misc/drm_minor`, whose table the module absorbed.

**R5a: one code path.** The Rust drivers use the same C interface as
`cdrmnull`: `drm-facade` turns a driver's `Driver` implementation into a
static `struct drm_driver` (generic associated constants, promoted to
`'static`), its ioctl table into `struct drm_ioctl_desc` entries with
one dispatch thunk, its `GemObject` type into `struct drm_gem_object_funcs`
and its KMS driver into the `drm_rkms_funcs` table, and wraps devices,
files, objects, fences, sync objects and sync files in owning handles
whose drops are the facade's put functions. There is no second path
into the core. A `GemRef` or `GemAny` drop never assumes that it may
block (a `timeout(9F)` handler looks like a thread to the kernel), so a
last reference is released by the core's task queue; `put(&Sleepable)`
releases it at once where the token proves that blocking is allowed. A
`Syncobj` handle is neither `Send` nor `Sync`, so its drop stays in the
thread that found it. An object's type is checked by a `TypeId` stored
after the C structure, behind the one non-generic `free` callback of the
driver module (generic functions may share addresses, so their
addresses do not tell types apart).

**R5a: concurrency.** A registered device is published for facade calls
from any context through `drm_core::published::Published`: a call counts
itself, then loads the pointer; `drm_dev_unregister()` first runs every
refusal check on the published device (open files, framebuffers, GEM
objects, fences, sync objects), so a refused device neither moves nor
stops working, and only when the detach is certain takes the device out
of the device table, then out of the facade, waiting for every call that
still uses it, and then tears it down and frees it. `getinfo(9E)`, which
the DDI does not serialise with `detach(9E)`, reads the node from the
table and never touches the device. The counting is sequentially
consistent and host-tested from eight threads; on the guest, `cdrmnull`'s
detach keeps calling the facade from a thread-level and a soft-interrupt
periodic handler while it unregisters (counts under *Where things stand*). The
callbacks added through one `struct dma_fence` form an intrusive list
through the driver's `struct dma_fence_cb` under a spin lock held with
interrupts blocked on its processor (`ddi_enter_critical(9F)`; the
critical sections only move pointers), so `dma_fence_add_callback()`
works up to low-level interrupts. A callback may drop the last reference
to its own structure while the fence signals; the release then skips
waiting for that callback, and callbacks queued behind it never run
(`cdrmnull`'s `FENCE_SELFTEST` checks these cases on the guest).

**R5a: how generic code stays inside the module.** The facade consists of
non-generic C functions over `#[repr(C)]` structures, so the core's
generics are instantiated only inside `misc/drm_core`, over its facade types,
and no Rust type, trait object or mangled symbol crosses the module
boundary. `drm-facade`'s own generics (`Gem<T>`, `Device<D>`) are
instantiated in each driver and only call C functions. A driver compiles
`drm-core` without the `kernel` feature, for value helpers only; the
test scripts and CI check that no driver module defines a symbol of the
core's stateful modules. The link of `misc/drm_core` reduces every symbol but
`_init`, `_fini`, `_info`, the 98 facade functions and `drm_debug` to
local scope (`ld -B reduce -M modules/drm_core/drm_core.mapfile`; 102 global
symbols), and CI and the test scripts compare the module's global
symbols with the mapfile. `tools/facade/gen_facade.py` generates
`drm_core.mapfile` and the drivers' `include/drm/drm_facade.mapfile` from the
headers, so neither can miss a function. Alternatives: exporting the
core's Rust interface would export mangled names of one compiler build,
and generic code cannot be shared across modules at all; trait objects
across modules depend on a vtable layout Rust does not fix; keeping a
static core in each Rust driver beside a C facade for C drivers would
have meant two code paths and no shared state, which is what made PRIME
between devices of different drivers fail with `ENOTSUP` in R2.

**R5a: facade version.** `drm_facade_version(struct drm_facade_version
*out)` reports the facade's major and minor version and the sizes of
its 28 public structures, in the order of the `DRM_FACADE_STRUCTS` list
of `include/drm/drm_facade_version.h` (append only). The major version
changes whenever a structure's size, a field's offset or meaning or a
function's meaning changes, or a function is removed; the minor version
changes when functions or callbacks are added without changing a size.
A driver loads only on a core with the same major version, the same or a
later minor version and the same size of every structure it was
compiled with: C drivers call the inline `drm_facade_check()` in
`_init(9E)`, the Rust drivers call `drm_facade::check_facade()` from
`ddi::Driver::load` (a hook added to illumos-ddi for this), and either
refuses to load with a warning naming the first difference.
`drm_dev_alloc()` checks the version recorded in the driver's
`struct drm_driver` again. Version 0 (0.1) was unstable; R5b made it
1.0 (see *R5b: facade version 1.0*), and the compatibility layer (R5c)
and mode setting (R5i) are expected to change the major version again.

**R5a: the facade structures.** The headers are MIT-licensed
reimplementations of the interfaces of Linux v7.2's headers, written from
the documented interface rather than copied (decision D7): the
structures and functions have Linux's names, and the public fields have
Linux's names and order, followed by an opaque tail (`__core`) that the
core owns. Each header records its origin. The public fields:
`struct drm_device` 22 (`if_version` to `debugfs_root`, without
`mode_config`, which waits for R5i; 296 bytes), `struct drm_minor` 4,
`struct drm_file` 6, `struct drm_gem_object` 15 (`refcount` to `lru`,
with Linux's `gpuva` and `lru` members; 256 bytes),
`struct drm_gem_object_funcs` 8 (`free`, `open`, `close` and five
illumos callbacks for memory and mappings), `struct dma_fence` 8
(Linux's two unions counted once; 96 bytes), `struct dma_fence_ops` 7,
`struct dma_fence_cb` 2, `struct dma_resv` 2, `struct dma_buf` 8,
`struct dma_buf_attachment` 4 and `struct drm_driver` 22 (Linux's hooks
the core calls, plus `illumos_*` limits and hooks), together with the
`drm_rkms_*` structures of the KMS interface for Rust drivers. What a
field holds is documented in the header; fields such as
`dma_fence.flags` are copies the core refreshes, not the state itself.
`xtask facade-check` compares the sizes and offsets of `drm-facade-sys`
with what gcc computes on illumos (`tools/facade/facade_layout.c`,
38 sizes and 233 offsets; the members of anonymous unions are checked
through their parents, not one by one), and `kernel-check` compares them
with the loaded module's CTF. `tools/facade/check-generated.sh` (CI)
checks that the bindings, both mapfiles, the layout program and xtask's
table are what the generators make of the headers.

**R5a: CTF.** `ctfconvert` does not read Rust's DWARF, so `misc/drm_core`
carries the CTF of a C shadow: `modules/drm_core/ctf/drm_facade_ctf.c`
includes every facade header and names every facade structure, and
`build-ctf.sh` compiles it with DWARF, converts it with the gate's
`ctfconvert -l drm`, drops the DWARF and leaves a 57 KB object that is
linked into the module beside the Rust archive. Its CTF is a standalone
container (2664 types, with the kernel types the facade uses), so
`mdb -k` prints `::print -t struct drm_device`, `::sizeof struct
dma_fence` and live structures such as a driver's `drm_device`. The
module's own Rust types stay without CTF. The shadow's variables are
local after the link, so the CTF's object section does not describe the
module's symbols; only its types matter.

**R5a: deviations from Linux.** `struct drm_device` has no `mode_config`
(R5i); the Rust drivers' KMS goes through the illumos `drm_rkms_*`
interface, and `cdrmnull` has no display yet (its port of `rdrmnull`'s
virtual display is deferred to R5i). `struct mutex` is
`struct lx_mutex`, because illumos has a `struct mutex` of its own.
`dma_fence_wait_timeout()` counts clock ticks; in facade 0.1 fence
callbacks other than `release` were not called, the fence lock was unused
and `cb_list` was not a list (R5b changed all three; see *R5b: fences*).
`dma_resv` functions
take only a NULL acquire context; the lock belongs to the thread that
took it (only it adds fences and unlocks; a relock by the holder fails
with `-EDEADLK` where Linux would deadlock), and a `dma_resv_usage`
outside Linux's four is refused. `drm_dev_unregister()` returns
`-EBUSY` when the core refuses (a file or an object is still in use)
instead of nothing. GEM memory and user mappings use illumos backings
and the devmap framework (`drm_illumos_*` in `include/drm/drm_illumos.h`),
and a reference dropped from a `timeout(9F)` handler goes through
`drm_illumos_gem_object_put_deferred()`, because the core cannot tell
that context from thread context; `dma_fence_init()`,
`drm_illumos_fence_create()` and `dma_fence_signal()` never block, in
any context. `dma_buf_fd()` takes over the caller's reference, as on
Linux. PRIME import between devices of
different drivers is supported and returns the exporter's object (one
handle per object per file), which R2 refused with `ENOTSUP`; a C
driver's import of another device's buffer exports that buffer again,
as on Linux. An object's `funcs->open` and `funcs->close` run once per
file and object (the file's first handle in, its last handle out), not
once per handle as on Linux; see *Driver hooks per file and object (G8)*.

**Driver hooks per file and object (G8).** Until G8,
`PRIME_FD_TO_HANDLE` of an object the calling file held already ran the
driver's open hook before looking for the file's handle and, on finding
it, undid the hook with the close hook. `virtio_gpu`'s hooks attach the
resource to the file's virgl context and detach it, and the host does not
count attachments, so the undo detached the resource from the context of
the handle the file already had: after Xorg's DRI3 `PixmapFromBuffer` of
its own front buffer, glamor's context lost the buffer (virglrenderer:
"Illegal resource") and the screen stayed black (G3 and G4 found it with
Mesa's dma-buf import). Linux's per-handle callbacks would have the same
effect for a second handle from `GEM_OPEN` or `MODE_GETFB` closed again
(Linux's virtio-gpu detaches on every close). Now each file's handle
table (`drm_core::objects::FileTable`) calls the open hook when the file
gets its first handle to an object and the close hook when its last one
goes, under a per-file hook lock held across the decision, the hook and
the change of the table (Linux's per-file PRIME lock). `PRIME_FD_TO_HANDLE`
looks for the file's handle under that lock first, for buffers of the
same device and for another driver's (whose import, `gem_prime_import`,
is now made under the lock too, so two threads of one file importing one
buffer get one import), and runs no hook when it finds one. `GEM_OPEN`,
`MODE_GETFB` and `drm_gem_handle_create()` still make a second handle when
the file holds the object, as Linux does (R1's test checks it, and
userland that opens a name, reads the size and closes the handle would
otherwise close its own buffer), but run no hook for it, so closing it
leaves the attachment alone. A handle creation that fails after the open
hook ran undoes only that hook; one that fails for an object the file
held runs none. Drivers that count their open and close calls, as
Linux's amdgpu, radeon and nouveau do, see the same balance. The `gem`
kstat counts the file and object pairs whose open hook ran (`attached`)
and the open hook's calls (`opens`). The host tests in
`crates/drm-core/src/objects.rs` drive the table from eight threads with
hooks that assert they alternate; they fail with the old behaviour and
without the hook lock.

**R5b: public-layout base objects.** Facade 1.0 makes the public
fields of the core's base objects the objects' state, so that a C driver
embeds them and uses them as Linux drivers do: the counted field sets of
i915's headless code (`dma_fence` flags, seqno, error, context,
cb_list, timestamp, refcount, ops and its lock; `drm_gem_object` dev,
size, resv, refcount, vma_node, import_attach, funcs; `drm_device` dev,
primary, ref, managed, open_count, ...; `drm_file` driver_priv, minor;
the `drm_printer` declared on the stack). The layouts, with Linux v7.2
as the origin of names and order (MIT reimplementations, D7):

| structure | public fields | size | private tail | change in 1.0 |
| --- | --- | --- | --- | --- |
| `drm_device` | 24 (27 with `managed.*`) | 336 B | 128 B | `managed` (resources, final_kfree, lock) after `dma_dev`, `open_count` after `unique`; `ref` and `open_count` live |
| `drm_file` | 6 | 88 B | 32 B | `object_idr`/`table_lock` replaced by `drm_illumos_gem_for_each_handle()` |
| `drm_gem_object` | 15 (17 with `gpuva.*`) | 256 B | 64 B | `refcount` is the object's only count |
| `dma_fence` | 8 (11 counting the union members) | 96 B | 32 B | every field is the state; flag bits renumbered to v7.2 |
| `dma_fence_ops` | 7 | 56 B | - | all callbacks called |
| `dma_fence_cb` | 2 | 24 B | - | `node` on the fence's `cb_list` |
| `dma_resv` | 2 | 48 B | 16 B | unchanged (`lock` remains a placeholder) |
| `dma_buf` | 8 | 88 B | 16 B | unchanged |
| `drm_printer` | 7 (9 with `line.*`) | 56 B | - | new |
| `va_format` | 2 | 16 B | - | new |

The private tails are recorded in the version handshake (below), and
`xtask facade-check` compares every size and every offset, now including
the members of anonymous unions and structures (`dma_fence`'s lock and
list unions, `drm_device.managed.*`, `drm_gem_object.gpuva.*`; 40 sizes
and 256 offsets), with gcc on illumos.

**R5b: fences.** `drm_core::cfence` implements Linux's `dma_fence` rules
on the public structure, for fences a driver embeds (`dma_fence_init`,
`dma_fence_init64`) and for the structures the core makes for its own
fences alike: the lock (`extern_lock`, or `inline_lock`, which the core
initialises and destroys) protects `cb_list`, a Linux list of
`struct dma_fence_cb`; signalling sets the signalled bit, replaces the
list by the timestamp and runs every callback with the lock held;
`ops->enable_signaling` runs once, when somebody first needs the fence
(a callback, a wait, `dma_fence_enable_sw_signaling`, or the core taking
the fence); `ops->signaled` is polled by `dma_fence_is_signaled` and
`dma_fence_get_status`; `ops->wait` replaces the default wait;
`ops->set_deadline` and the name callbacks are reachable. A driver may
add to `cb_list` and signal by hand, as i915's breadcrumbs and
`i915_active` do. The rest of the core (sync objects, reservations,
sync files, arrays, chains, waits) works on its own fence objects; each
structure carries one in its tail. A driver's fence has a *shadow*,
signalled by the *bridge*, a callback the core adds to the structure's
`cb_list` when it first takes the fence (which enables signalling); the
bridge holds a reference to the structure until it has run, as a Linux
sync object holds the fence it waits for. A core fence handed to a
driver gets a *proxy* structure whose ops are the core's: its
`enable_signaling` arms a *relay* on the core fence, its `signaled`
asks the core fence, and the relay takes a reference only while the
count is not zero; the release removes the relay first, waiting for one
that runs. The last `dma_fence_put` releases the structure (callbacks
still on the list of an unsignalled fence run with `-EDEADLK`, as on
Linux, with a warning; then the tail state, an inline lock, and
`ops->release`) at once only in thread context by a thread that does not
hold the fence's lock; from an interrupt handler, from a callback, from
the bridge and from the relay it is parked through a node in the tail on
the module domain's list and released by its task queue
(`FenceDomain::defer_raw`). That needs no memory, never runs a driver's
release in interrupt context, and removes R5a's open issue of a release
in an interrupt spinning on a callback it interrupted.
`drm_illumos_fence_put_deferred()` always parks (for `timeout(9F)`
handlers), and `drm_illumos_fence_barrier()` waits for every parked
release, which a driver calls before its ops tables go (`cdrmnull`'s
`_fini` and release). The logic is tested on the host with a mock
driver and threads (`crates/drm-core/src/cfence/tests.rs`: 14 tests).

**R5b: GEM reference counts and handle walks.** A facade object's
`refcount` is its only reference count (`GemObject::ext_count`): every
`GemRef` counts in it (handles, names, mappings, dma-bufs, the driver's
references), the core's allocation holds the object with one reference
of its own until the count reaches zero, and the count never comes back
from zero (`drm_core::refcount`). Drivers can therefore read it
(`kref_read()`) and take a reference with `kref_get_unless_zero()` (both
in `drm_facade_types.h`) from lists of their own that hold none (i915's
shrinker lists), as long as the list's lock keeps the memory valid, and
drop it with `drm_gem_object_put()`. The core frees objects with no RCU
grace period, so i915's lockless RCU lookups must become locked lookups.
`drm_illumos_gem_for_each_handle(file, fn, arg)` replaces walking
`drm_file.object_idr` under `table_lock`: it calls `fn` for every handle
and object of the file, in handle order, until `fn` returns non-zero,
with the file's hook lock (G8) held, over a snapshot of references taken
under the handle table's lock, so no handle comes or goes during the walk
and lookups from `fn` work; `fn` must not create or close handles of the
file. `drm_gem_object_lookup()` finds one handle's object with a
reference (Linux's lockless `idr_find()` under RCU has no counterpart).
No Rust driver walks handles yet, so `drm-facade` has no wrapper for it.

**R5b: managed resources.** `drm_managed.h` provides
`drmm_add_action()`, `drmm_add_action_or_reset()`,
`drmm_release_action()`, `drmm_kmalloc()`, `drmm_kzalloc()`,
`drmm_kmalloc_array()`, `drmm_kcalloc()`, `drmm_kstrdup()` and
`drmm_kfree()` over `drm_core::managed` (`drmm_kmalloc(dev, 0, ...)`
returns NULL, where Linux returns a non-NULL zero-size pointer): nodes on
`drm_device.managed.resources` under `managed.lock`, released newest
first when the device's last reference goes (`drm_dev_put()`), after
`drm_dev_unregister()` and the driver's `release` callback and before the
structure is freed, each action called and each allocation freed once,
with no lock held. `gfp_t` and its bits are the facade's
(`__GFP_DIRECT_RECLAIM` sleeps, `__GFP_ZERO` zeroes). `drmm_mutex_init()`
and `drmm_alloc_ordered_workqueue()` wait for the compatibility layer
(R5c), which provides Linux's mutexes and workqueues.

**R5b: printing.** `drm_print.h` follows Linux v7.2: the debug
categories are bit numbers, `struct drm_printer` and `struct va_format`
have Linux's layout, `drm_info_printer()`, `drm_dbg_printer()` and
`drm_err_printer()` make the core's printers, `drm_printf()`,
`drm_vprintf()`, `drm_puts()` and `drm_printf_indent()` print to any
printer, and the `drm_err()`/`drm_warn()`/`drm_notice()`/`drm_info()`
(and `_once`) and `drm_dbg_*()` families, `drm_dev_printk()`,
`DRM_DEV_*` and `DRM_*` log on the device's node. The variadic entry
points are static functions of the header that build a
`struct va_format`; everything ends in `drm_illumos_vprintk()`, which
formats with `vsnprintf(9F)` into a 512-byte message (cut with "...")
and logs it with `dev_err` or `cmn_err`: errors and warnings `CE_WARN`
to the console and the log, notices `CE_NOTE` to both, information and
debug `CE_NOTE` to the log only. A debug message of category C is
printed only while bit C of `drm_core`'s `drm_debug` is set: `set
drm_core:drm_debug = 0x6` in `/etc/system`, or `echo 'drm_debug/W 0x6' |
mdb -kw` (the bits are Linux's `drm.debug`). `drm_WARN*()`, the line,
coredump and seq_file printers and `drm_print_bits()` are not provided;
the `_ratelimited` variants print every message.

**R5b: facade version 1.0.** The major version changed (Linux's flag
numbering, the new `drm_device` fields, fences and GEM counts that are
state). `drm_facade_version` keeps its size (an older driver still reads
the refusal) and its list gains `drm_printer` and the sizes of the
private tails of `drm_device`, `drm_file`, `drm_gem_object`,
`dma_fence`, `dma_resv` and `dma_buf` (`DRM_FACADE_TAILS`), which
`drm_facade_check()` and `drm_facade::check_facade()` compare, so a change
of the core's private state is refused even when it leaves a structure's
size alone. `run-r5a-test.sh`'s mismatch probes cover another major
version, a larger `drm_device` and a same-size `drm_device` with a smaller
tail.

**R5b: deviations from Linux.** `dma_fence_put()` and the other fence
functions are out-of-line (Linux's are inline); a fence's lock must not
be a high-level spin mutex; `ops` is kept after signalling (Linux clears
it for fences without `release` or `wait`); `release == NULL` frees
nothing (Linux's `dma_fence_free()` needs Linux's `kfree`, R5c). The
core takes a driver's fence by enabling signalling and adding its bridge,
so the fence is enabled earlier than on Linux when it enters a sync
object, reservation or sync file. A sync object returns a proxy of the
core fence, not the driver's structure, so pointer identity with the
fence the driver added does not hold. `dma_fence_array_create()`, the
`dma_fence_chain_*` walkers and `dma_resv` iterators and acquire contexts
are not in the facade yet. A GEM object's `handle_count`, `name` and
`dma_buf` stay in the core. Messages logged in interrupt context are cut
by illumos at 200 bytes.

**R5e: carried helpers.** C drivers call Linux helper code beyond the
facade. For i915's headless build on the owner's integrated GPUs (gen7 and
gen9.5), measured on R5c's configuration of Linux v7.2 `8d3ae59288`
(163 headless translation units), it imports 63 symbols from Linux's
`drm.ko`, 3 from `drm_kms_helper`, 29 from TTM, 4 from the GPU buddy
allocator and 2 from `drm_buddy`, besides `dma-buf` and the kernel. Of the
`drm.ko` ones, the facade provides 19, and 12 are carried here: 10 are
`drm_mm`, the range allocator i915 manages its GTT address spaces, the
stolen memory and its eviction with (pure algorithm, no state outside the
caller's structure), and 2 are `drm_cache`, x86 cache write-back:
`drm_clflush_virt_range` is built now, `drm_clflush_sg` once R5c's
`struct page` and scatterlist models exist (see *unmodified Linux* below).
31 belong to the core (files, ioctls, GEM, PRIME, sync objects, mapping
offsets, printing, managed resources: carrying Linux's code would
duplicate the core's state; they are facade work for R5c's inventory and
the i915 port) and one (`drm_mode_config_reset`) is display. Not carried: `drm_suballoc` and
`drm_exec` (i915 does not use them), `gpu-sched` and `drm_gpuvm` (i915 has
its own scheduler and address spaces), the buddy allocator and
`drm_buddy` (compiled into i915 unconditionally but used only by
`i915_ttm_buddy_manager.c` for local memory, which integrated GPUs do not
have: `intel_memory_regions_hw_probe()` sets up system memory with shmem
and stolen memory with `drm_mm` unless `IS_DGFX()`; that object also
imports TTM's resource manager, so it cannot be linked without TTM
either: whichever way the i915 port links TTM decides it, and if TTM is
carried, `drivers/gpu/buddy.c` and `drm_buddy.c`, both MIT, go into a
`misc/gpu_buddy` with this machinery), TTM (R5d, deferred to amdgpu; i915
calls `ttm_device_init()` on every GPU, so the port must stub the
integrated path or carry TTM), `intel-gtt` from `drivers/char/agp` (only for
gen2 to gen5; R5c leaves `intel_ggtt_gmch.o` out) and the display helpers
(`drm_rect`, `drm_fourcc`, DP, atomic: the display phase).

**R5e: one module, `misc/drm_helpers`.** Linux builds `drm_mm` and `drm_cache` into
`drm.ko`, whose stateful part is our Rust `misc/drm_core`; C code cannot
go into that Rust module without making it depend on the compatibility
layer. So the C half of `drm.ko` becomes one C module, `misc/drm_helpers`,
which will also take the other `drm.ko` helpers C drivers need later
(`drm_rect` and `drm_fourcc` with the display); helpers that are separate
Linux modules (`drm_suballoc_helper`, `drm_exec`, the buddy allocator,
`gpu-sched`, TTM, the display helpers) get a module each when a driver
needs them, so the provider table of the R5 analysis maps one to one. The
module is linked `-z defs -B reduce` with `drm_helpers_scope.mapfile`: its
global symbols are `_init`, `_fini`, `_info`, the ten `drm_mm` functions
i915 imports and `drm_clflush_virt_range`, nothing else (`drm_mm_print()`,
which i915 does not import, the red-black tree and the processor facts
stay local); `build.sh` fails otherwise.
Dependents link with `-N misc/drm_helpers -M
carried/drm_helpers/drm_helpers.mapfile`. The module depends on nothing but
genunix and needs no guard against the legacy stack; its only state is
what `drm_cache` reads from CPUID in `_init` (CLFLUSH, CLFLUSHOPT, the
line size), read-only afterwards, and `_init` refuses a processor without
CLFLUSH;
gfx-drm's `misc/drm` exports `drm_mm` functions under the same names, and
krtld resolves each module against its own dependencies, so the stacks do
not mix.

**R5e: unmodified Linux, one patch.** `drm_mm.c`/`.h` (MIT notice of
Tungsten Graphics and Intel) and `drm_cache.c`/`.h` (Tungsten Graphics,
Red Hat), none with an SPDX tag, are vendored byte for byte
(`fetch-and-patch -f` compares them with git.kernel.org at the pin); MIT
and dual-licensed files may be vendored, GPL-only ones may not (the script
refuses them, and files `SOURCES` does not list). `drm_mm` builds without
a change: `CONFIG_DRM_DEBUG_MM` is off (its stack depot is GPL-2.0-only),
and every difference from Linux lives in the compatibility headers.
`drm_cache` has one patch, which only adds conditionals: its page,
scatterlist, swiotlb and write-combining-copy functions need R5c's
`struct page`, `sg_table` and `iosys_map`, so they and their headers are
built only with `CONFIG_LKPI_STRUCT_PAGE`, which R5c's layer defines once
those exist; then `drm_clflush_sg` (which i915 imports) compiles
unchanged, joins the mapfiles and the patch can go. Until then the module
builds `drm_clflush_virt_range` alone. Linux's fallback for processors
without CLFLUSH, `wbinvd_on_all_cpus()`, has no definition here: CLFLUSH
is a constant true after `_init`'s check, the compiler drops the call, and
the `-z defs` link would fail if it did not. The configuration of the
carried code is passed like Linux's, on the command line
(`-DCONFIG_X86 -DCONFIG_X86_64`). Linux's KUnit tests for `drm_mm`
(`drm_mm_test.c`) are GPL-2.0-only, so they were not copied: their six
cases are re-expressed (MIT) in `carried/drm_mm/tests/drm_mm_selftest.c`
and extended to what i915 relies on (reservations, the four insertion
modes and `DRM_MM_INSERT_ONCE`, ranges, alignments that are not powers of
two, colour adjustment with guard gaps as i915's GGTT uses it, interval
queries, the eviction scan with colour eviction as
`i915_gem_evict_something()` drives it, random sequences checked against
an exhaustive feasibility oracle, and four allocators on four threads at
once). After every change a test verifies the whole allocator: the node
list, every hole, the hole stack and the three red-black trees with their
colours, parent pointers, leftmost caches and augmented maxima.

**R5e: compatibility headers.** The carried code needs Linux's lists,
red-black and interval trees, bit operations, `min`/`max`, `WARN`, errno
x86 barriers, `clflushopt()`, feature tests and `boot_cpu_data`
(`linuxkpi/include/asm/`, filled from CPUID by `linuxkpi/lib/x86_cpu.c`),
and a few headers that only have to exist (`slab.h`, `seq_file.h`,
`stacktrace.h`, `mm_types.h`, `export.h`, `spinlock.h`, `scatterlist.h`). They are in
`linuxkpi/include/` and `linuxkpi/lib/`, written for R5e
before R5c's compatibility layer existed, MIT reimplementations (Linux's
are GPL; decision D7), each marked "to be reconciled with R5c": R5c builds
the full layer at the same paths, so the merge replaces these files, and
`misc/drm_helpers` then links R5c's trees instead of its local copy. The
illumos kernel headers shape them: `struct list_head` and the base types
come from the facade, `list_move_tail()` is left out (illumos has its own
with another signature in `sys/list.h`), `__maybe_unused` comes from
`sys/ccompile.h`, and `typeof` is defined because the gate's `-fno-asm`
removes the keyword. The red-black tree implements Linux's interface and
node layout (callers read `rb_left`, `rb_right` and `__rb_parent_color`)
with the textbook algorithms and Linux's augmentation protocol;
`carried/test/host/rbtree_test.c` tests it, plain, cached and augmented,
against a full verification after every operation, and the interval tree
against a brute-force scan.

**R5e: tests in the kernel.** `drm_helpers_test` is a pseudo driver in the
style of illumos-ddi's `rselftest`: at attach it runs every self-test
against the loaded `misc/drm_helpers`, logs each result (failures with
`CE_WARN`), publishes the totals as the kstat
`drm_helpers_test:0:selftest` and attaches even when a test fails. Its
`.conf` sets the seed, a scale factor and `inject_failure`, which adds one
failing check to prove the failure path. `drm_mm` takes no lock (its users
serialise calls on one allocator; i915 holds the address space's mutex),
so the concurrency test runs independent allocators on a DDI task queue
of four threads at once. Linux has no test for `drm_cache`, and a cache
write-back changes nothing a CPU can see on coherent memory: its two tests
flush every shape of range (empty, one byte, across lines and pages, the
end of a buffer, random), also from four threads at once, and check the
memory is unchanged; whether a GPU sees the data is the i915 port's test
on hardware.

**R4: the virtio framework.** The driver runs the device through the
gate's virtio framework (`misc/virtio`), a private interface, through
illumos-ddi's private crate `illumos-kernel-private-virtio-sys` and its
safe wrapper `ddi::private::virtio` (feature
`unstable-gate-cda5b9a49f-virtio`), whose phases (setup, negotiated,
running) make the framework's `VERIFY`s unreachable (illumos-ddi
`docs/DESIGN.md` section 5.13). The framework calls queue handlers as
hardware interrupt handlers; the wrapper records the queue and hands the
work to the device's task queue, so every completion of this driver runs
in thread context and no driver lock has an interrupt priority
(`Driver::fence_priority` stays `None`). Negotiated features: `VIRGL`,
`EDID`, `RESOURCE_BLOB`, `CONTEXT_INIT` when the device offers them; the
control queue carries every request, the cursor queue the cursor.

**R4: requests and fences.** A request is one DMA allocation (the request
and room for the response) and, for memory entries and command streams,
a second one; the queue owns a counted reference while the chain is in
flight. Waiters also poll the queue themselves, so a slow task queue
delays nothing. Fenced requests (`SUBMIT_3D`, the 3D transfers) carry a
fence id taken under the submission lock and complete, in the device's
order per fence context, the R2 `Fence` that the ioctl put into the
objects' reservations, a sync file or sync objects. A completion drops the
GEM references its request held through the interrupt-safe path, because
destroying an object waits for `RESOURCE_UNREF`, which must never be
awaited on the completing thread. `drivers/virtio-gpu/src/gpu.rs`
documents the lock order.

**R4: objects.** Every GEM object is R1 system memory bound for DMA
against the device's node; its DMA cookies are the resource's memory
entries (`RESOURCE_ATTACH_BACKING`, or the entries of
`RESOURCE_CREATE_BLOB`). Dumb buffers are 2D resources in XRGB8888
(32 bits per pixel only, rows exactly `width` pixels, as the device
copies 2D resources with their own stride), through the new
`Driver::dumb_object` hook. With virgl, `RESOURCE_CREATE` makes 3D
resources; a file's first handle to an object attaches the resource to the
file's rendering context and removing its last one detaches it (the new
`GemObject::handle_opened` and `handle_closed` hooks, Linux's GEM open and
close callbacks, called once per file and object since G8: see *Driver
hooks per file and object (G8)*), and a file's context is destroyed in the
new `Driver::file_close` hook. An object's
destruction (the new `GemObject::release` hook) sends `RESOURCE_UNREF` and
frees the memory only after the device answered; without an answer the
memory is leaked with a warning.

**R4: uAPI.** The ioctls are Linux 6.12's with its checks and errnos.
`virtgpu_drm.h` of libdrm 2.4.109 lacks `RESOURCE_CREATE_BLOB`,
`CONTEXT_INIT`, the later parameters and the sync object arrays, so the
bindings come from Linux v6.12's header (MIT, `tools/uapi/include`).
Because the illumos ioctl encoding carries the argument size, each older
size of a grown structure is its own command number, and the driver
accepts all of them, zero-extended as Linux does: `EXECBUFFER` in 32
bytes (libdrm 2.4.109), 40 bytes (Mesa 21.3's own header, with
`ring_idx`) and 64 bytes (sync object arrays), and the transfers in 36
(libdrm 2.4.109) and 44 bytes. The driver reports version 0.1.0, which
Mesa's virgl winsys requires.

**R4: display.** A connector, encoder, CRTC, primary plane and cursor
plane per scanout (up to 16), the connector connected when the device
reports the scanout enabled, with the device's EDID or, without one, a
CVT mode of the scanout's size plus the standard modes; `SET_SCANOUT`,
`TRANSFER_TO_HOST_2D` for dumb buffers, `RESOURCE_FLUSH`, the cursor queue
for the cursor plane, `DIRTYFB` as one transfer and flush of the damage's
bounding box, and a software vblank per active CRTC for page-flip events.
The bus ID is `pci:0000:BB:DD.F` from the node's `reg` property, which
Xorg's modesetting driver matches.

**R4: deviations from Linux.** Host-visible memory needs the shared
memory regions of the virtio PCI transport, which the gate's framework
does not implement: `HOST_VISIBLE` is 0 and `BLOB_MEM_HOST3D` blobs are
`EINVAL`. `CROSS_DEVICE` is 0 (no resource UUIDs), a polled ring mask
other than 0 is `EINVAL` (its fence events would need a per-file event
queue outside mode setting), blob framebuffers are refused for scanout
(no `SET_SCANOUT_BLOB`), and a command stream is limited to 1 MiB. In
`EXECBUFFER`, adding the fence to the objects, to the out sync objects
and to a new sync file can fail for lack of memory after the work was
queued (Linux reserves those first); the ioctl then reports the error for
work that runs anyway. A context is named after the process ID, because
the DDI offers no process name.

**R4: Mesa.** Mesa 21.3.9 with the virgl gallium driver comes from
oi-userland's `hephaistos/virgl-gallium` branch (`d69854c287`) plus the
local build-ID patch described below. Two things outside this
repository stand between it and the device. First, Mesa's virgl disk
cache used the driver's GNU build ID without checking for one, and
illumos' link-editor emits none, so it crashed; a Mesa patch uses Mesa's
common identifier, which falls back to the library's modification time.
Second, OpenIndiana's libdrm 2.4.109 finds DRM devices only among device
nodes named `display` (VGA-class PCI functions), and the virtio-gpu
function (class 0x0380) is `pci1af4,1100`, so the loader cannot identify
it and EGL finds no device. The fix belongs in libdrm (match minors of
node type `ddi_display:drm`); until then `tools/test/drm_devshim.c`, loaded
with `LD_PRELOAD_64`, answers `drmGetDevice2` for the test guest's device.
`tools/test/virgl_egl_test.c` checks rendering through EGL on GBM, and
`tools/test/virgl_kms_gears.c` renders to the screen with page flips.
Under Xorg, the modesetting driver works, but OpenIndiana's Xorg is
built without glamor, so it offers neither DRI2 nor DRI3 and `glxinfo`
reports softpipe; GLX with virgl needs an Xorg built with glamor.

## Running beside the legacy stack

OpenIndiana's DRM drivers come from gfx-drm: the core `misc/drm` and the
Intel driver `i915` in the package `driver/graphics/drm`, which also binds
the Intel GPUs it supports. This repository's stack is `misc/drm_core`
(`driver/graphics/drm-core`) and the drivers linked against it
(`virtio_gpu`, `cdrmnull`, `rdrmnull`, and later an Intel driver). The two
stacks install together on one machine; they must never be loaded at the
same time, and loading both by accident must be impossible. Three things
make it so.

**Each core refuses the other.** `misc/drm_core`'s `_init` returns `EBUSY`
with the warning "drm_core: the legacy DRM core misc/drm (gfx-drm) is
loaded; ..." while a module named `drm` is loaded, and gfx-drm's `misc/drm`
returns `EBUSY` with "drm: the DRM core misc/drm_core is loaded; ..." while
a module named `drm_core` is loaded. Every driver depends on its core
(`-N misc/drm` or `-N misc/drm_core`), and the kernel loads and
initialises a module's dependencies first, so a driver of one stack fails
to load while the other stack is loaded, whichever path or dependency
loads it. The reverse check is a change to gfx-drm's `drm_sunmod.c`
(branch `drm-coexist` of gfx-drm, from `master`); until a
`driver/graphics/drm` with it is installed, only `misc/drm_core` refuses,
and OpenIndiana's `misc/drm` still loads beside `misc/drm_core` (the G7
test shows both). Both checks walk the module list under `mod_lock` as
`mod_name_to_modid()` does and compare `mod_modname`, which is the same
whatever path loaded the module (illumos-ddi's
`ddi::private::module_loaded`, and the same loop in C). Neither takes a
hold: a hold (`mod_hold_by_name`) waits while a module is busy, which it
is during its `_init`, so two modules holding each other from their
`_init` could deadlock; the walk waits only for `mod_lock`, which the
module framework never holds while it calls `_init` or waits for a busy
module (`modinstall` asserts `MUTEX_NOT_HELD(&mod_lock)`; `mod_hold_by_modctl`
releases it in `cv_wait`). The other core counts as loaded when it is
installed (`mod_installed`), or in memory and held busy (`mod_loaded` and
`mod_busy`), as it is while its `_init` or `_fini` runs. A core that is
only in memory does not count: a dependency whose `_init` failed stays
that way, invisible to `modinfo(8)`, because `mod_install_requisites`
does not unload it and `modunload` cannot (`moduninstall` answers
`EALREADY`); the first guest run found that counting it kept the other
core out after a refused `modload drv/i915` until `misc/drm` was loaded
again. Two cores loading at the same time cannot both succeed: each is in
memory and busy before its own `_init` runs the check, `mod_busy` changes
under `mod_lock`, `mod_installed` is set before the module is released,
and each check's `mutex_enter` (a locked instruction) orders the earlier
stores before its reads, so at least one of them sees the other and
refuses (both may refuse; a retry then loads one). This rests on both
checks and the framework's busy and release steps taking the same
`mod_lock`: a check that takes the lock after the other core's check
released it sees every store the other core made before its check, on any
architecture.

**drmadm(8) selects the stack for boot.** `tools/drmadm/drmadm`, a shell
script in the package `driver/graphics/drmadm` (required by
`driver/graphics/drm-core`, so it stays when the new stack is removed),
with its manual page `tools/drmadm/drmadm.8`:

```sh
drmadm status               # selection, packages, loaded modules, Intel aliases, excludes
drmadm select legacy|new    # for the next boot
drmadm -R /mnt select legacy    # in a mounted boot environment
```

`select` refuses while any module of either stack is loaded and while
another `drmadm` holds its lock (`/etc/system.d/.drmadm.lock`), then:

1. writes `/etc/system.d/driver:graphics:drmadm` (named after the package,
   as `system(5)` recommends) with the selection, its time, the alias set
   and `exclude:` lines for the other stack's modules: `misc/drm`,
   `drv/i915`, `drv/radeon` when the new stack is selected, `misc/drm_core`,
   `drv/i915n`, `drv/virtio_gpu`, `drv/cdrmnull`, `drv/rdrmnull` when the
   legacy stack is. Each is written as `class/name` (the form of
   `system(5)`, which matches a load as a dependency or by class) and as
   the bare name, because `mod_load` checks the exclude list against both
   `mod_filename` and `mod_modname` (`os/modctl.c`), and only the bare name
   matches `modload /kernel/misc/amd64/drm`. Excluding the core alone would
   suffice, since every driver needs it; the drivers are listed so that
   their loads at boot fail on their own exclusion. The fragment is
   written first: it records the alias set, so a failure in the next step
   loses no alias.
2. moves the Intel GPU aliases to the driver of the selected stack: `i915`
   for the legacy stack, `i915n` for the new one. `i915n` is the name
   reserved for this stack's Intel driver (work item G5); until it exists,
   `select new` takes the aliases from `i915` and leaves them unbound, says
   so, and an Intel GPU falls back to `vgatext` (through the GPU's
   compatible name `pciclass,030000`, an alias of `vgatext`). The set
   moved is the aliases the installed `driver/graphics/drm` declares for
   `i915` (`pkg contents -m`), those
   bound to `i915` or `i915n`, and those recorded in the fragment by an
   earlier selection; `select legacy` gives `i915` at least its package's
   aliases when that package is installed. Aliases of the set that are bound
   to any other driver are reported and left alone. `update_drv -d -i`
   takes one alias per call (given a list, it matches no line of
   `driver_aliases` and still exits 0, because `delete_entry` in
   `drvsubr.c` compares the whole argument), so drmadm removes them one by
   one and reads the file back; `-a` takes the list. If a move fails,
   drmadm gives the aliases back and restores the previous fragment.
3. runs `bootadm update-archive`: the kernel reads `/etc/driver_aliases`
   and `/etc/system.d/.self-assembly` (which `bootadm` assembles from the
   fragments; `/etc/system.d` exists since illumos 2018) from the boot
   archive, and `exclude:` lines are read only at boot.

A selection therefore takes effect at the next boot; until then the
guards keep the running system safe. Selecting the selected stack again
changes nothing.

Why `update_drv` and why `-n`: `update_drv -a/-d -i` is the supported way
to change `/etc/driver_aliases` (it locks the file against `add_drv` and
`rem_drv`, checks that an alias is not bound to another driver and
rewrites the file atomically), where editing the file by hand would race
with them and skip those checks. Without `-n`, `update_drv -a` would bind
and attach the driver at once (loading its stack) and `update_drv -d`
would try to unconfigure and unload the bound devices; with `-n` it only
edits the file, which matches a selection for the next boot. Two IPS
consequences: while the new stack is selected, `pkg verify
driver/graphics/drm` reports the `i915` aliases as missing, and `pkg fix`
would give them back (run `drmadm select new` again; the same after a
`pkg update` of `driver/graphics/drm` that adds `i915` aliases, which IPS
adds with `update_drv`); and a package of
this stack must not declare a legacy alias in its driver action: IPS
refuses to install a driver action whose alias another driver action in
the image declares, and the operation fails (`pkg5`'s `driver.py`).
`i915n`'s package will therefore deliver its driver action without
aliases, and no package of
this repository binds an Intel GPU at installation (`virtio_gpu` binds only
`pci1af4,1050`, `cdrmnull` and `rdrmnull` are pseudo devices).

**Device links.** Both stacks create `ddi_display:drm` minor nodes, and
the devfsadm link module `SUNW_drm_link` makes `/dev/dri/<minor name>`
for either; since gfx-drm's G2 change it is the package
`driver/graphics/drm-devlink`, which libdrm requires instead of the
legacy kernel package. The `devlink=` attributes of this repository's
driver actions add the same links where that module is missing; both
make identical links. A switch leaves the links of the stack that is no
longer loaded pointing at minor nodes that no longer exist. `devfsadm -C`
removes dangling links only through the link modules' removal rules, and
`SUNW_drm_link`'s rule (`^dri/[a-z]D?[0-9]+$`) matched none of `card0`,
`controlD64` or `renderD128`; gfx-drm's branch `drm-coexist` fixes it
(`^dri/[a-z]+D?[0-9]+$`), so after the first boot with the new selection
`devfsadm -C` cleans `/dev/dri`; `drmadm select` says so, and the manual
page and `docs/x230.md` include the step. libdrm 2.4.134 enumerates `/dev/dri`, takes a link's node only
if `stat(2)` reaches it and libdevinfo shows a `ddi_display:drm` minor with
its device number, so it skips stale links (a node of an excluded driver
cannot attach) and lists only the devices of the stack that is attached.

**Recovery.** From the boot loader, boot the previous boot environment;
or boot single user (`-s`) and remove the fragment:

```sh
rm /etc/system.d/driver:graphics:drmadm
bootadm update-archive
```

then reboot and run `drmadm select legacy` (the Intel aliases still belong
to the driver of the removed selection). From another boot environment:
`beadm mount <be> /mnt; drmadm -R /mnt select legacy; beadm umount <be>`.
For one boot, `boot -a` and the answer `/etc/system` to "Name of system
file" skip every `/etc/system.d` fragment (`mod_askparams()` clears the
self-assembly file), so nothing is excluded. Before removing the new
stack's packages, run `drmadm select legacy`: the fragment is not a
packaged file, and a selection of the new stack would keep excluding the
legacy modules. A `pkg` operation that creates a boot environment copies
the running one, so a selection made afterwards in the running one does
not reach it (select after booting it, or with `-R`). `docs/x230.md` is the
operator note for the ThinkPad X230.

**Open issues.** The reverse guard and the link rule are in gfx-drm's
branch `drm-coexist`, which must be merged and a `driver/graphics/drm`
with it installed before a machine loads both stacks; OpenIndiana's
current `misc/drm` loads beside `misc/drm_core`. The nodes of the packaged
`virtio_gpu` and `cdrmnull`, attached at boot, are `0600 root root`
although `minor_perm` says `0666 root sys`, so unprivileged processes
cannot open them (seen on the G7 guest, as before for test drivers
registered at boot: `notes/hosts.md`); `devfsadm -i` and a reload do not
change them, `rem_drv -C` clears them; a packaging question beyond G7.
`drmadm select` refuses while a DRM module is loaded, so on a machine
whose display uses `i915` the display manager must stop first (or select
with `-R`). The busy part of the guard rule is tested deterministically
only with DTrace (`overlap`); the 100 concurrent loads never overlapped
their `_init` calls.

## Building

Type checking, linting and host tests work on any host with stable Rust
and the hosted illumos target:

```sh
rustup target add x86_64-unknown-illumos
cargo check --workspace --target x86_64-unknown-illumos
cargo clippy --workspace --target x86_64-unknown-illumos -- -D warnings
cargo fmt --all -- --check
cargo test -p drm-core -p drm-uapi -p drm-facade   # host target
```

The kernel modules need the pinned nightly with `rust-src`, because `core`
and `alloc` are rebuilt without SSE for `targets/x86_64-illumos-kmod.json`.
Compiling works on any host; linking needs the illumos linker:

```sh
cargo xtask build drm-module         # target/x86_64-illumos-kmod/release/drm_module.a
cargo xtask build rdrmnull           # target/x86_64-illumos-kmod/release/rdrmnull.a
# on illumos (or: cargo xtask link drm-module; cargo xtask link rdrmnull)
modules/drm_core/ctf/build-ctf.sh    # drm_facade_ctf.o, needs the gate's ctfconvert
ld -ztype=kmod -N fs/specfs -B reduce -M modules/drm_core/drm_core.mapfile \
    -z allextract drm_module.a -z defaultextract drm_facade_ctf.o -o drm_core
ld -ztype=kmod -N fs/specfs -N misc/drm_core -z allextract rdrmnull.a -o rdrmnull
cargo xtask symbols drm_core modules/drm_core/drm_core.mapfile   # its globals are the mapfile's
```

A C driver includes `<drm/drm_facade.h>` (the repository's `include`
directory before the system headers, which provide `sys/*.h` and libdrm's
`drm/drm.h`), calls `drm_facade_check()` in `_init(9E)` and is linked
with `-N misc/drm_core`; with `-z defs`, the gate's `ddi.mapfile` and
`include/drm/drm_facade.mapfile` declare what it may call.
`drivers/cdrmnull/build.sh` does this with the gate's kernel compiler
flags:

```sh
DDI_MAPFILE=<gate>/usr/src/uts/common/mapfiles/ddi.mapfile drivers/cdrmnull/build.sh
# ld -ztype=kmod -N misc/drm_core -z defs -M ddi.mapfile \
#     -M include/drm/drm_facade.mapfile -M drivers/cdrmnull/private.mapfile ...
```

`misc/drm_core` must be installed (`/kernel/misc/amd64/drm_core`, or the package
`driver/graphics/drm-core` of `packaging/drm-core.p5m`) before any driver
loads; the test scripts link it from `drm_module.a` and `drm_core.mapfile`
(and `drm_facade_ctf.o` if present) when it is not installed, and remove
it again. It is installed beside OpenIndiana's gfx-drm but never loaded
beside it (see *R5a: the module's name, and never beside gfx-drm*).

`tools/test/run-r1-test.sh` does the rest on a disposable illumos VM, as
root, in a directory with `rdrmnull.a`, `rdrmnull.conf`,
`tools/test/drm_r0_test.c`, `tools/test/drm_r1_test.c`,
`tools/test/drm_r2_test.c`, `tools/test/drm-test-lib.sh`, `drm_module.a`
and `drm_core.mapfile` and the patched headers as `drm/`: it records
the driver files in `/etc` and the `/devices` tree, links the module,
checks that the running kernel exports every symbol the module imports,
builds the test programs (64-bit with libdrm and libkstat, and 32-bit
probes), installs the module under `/usr/kernel/drv`, runs
`add_drv -m '* 0666 root sys' rdrmnull`, runs the R0, R1 and R2 tests,
records the `gem` and `sync` kstats and the device's vmem arenas, checks
that the module refuses to unload while a file is open and unloads once
it is closed, removes everything again with `rem_drv -C`, and fails
unless `/etc` and `/devices` are as before. `tools/test/run-r0-test.sh`
runs the R0 test alone. The scripts find the device's nodes through the
`/dev/dri` links (a device is `card<N>` for whatever `N` it got) and hand
them to the tests in `DRM_CARD`, `DRM_RENDER`, `DRM_CARD1` and
`DRM_INSTANCE`; without them the tests use `card0` and `renderD128`.

The virtio-gpu driver is built the same way and linked against the
virtio framework too:

```sh
cargo xtask build virtio_gpu
ld -ztype=kmod -N misc/virtio -N fs/specfs -N misc/drm_core -z allextract virtio_gpu.a -o virtio_gpu   # on illumos
```

`tools/test/run-r4-test.sh` links, installs (`add_drv -i
'"pci1af4,1050"'`), tests and removes it on a guest with a virtio-gpu
device. `tools/test/run-g6-test.sh` runs `virtio_gpu` and two `rdrmnull`
devices together (see its header for the files it needs), and
`tools/test/run-r5a-test.sh` runs `cdrmnull`, `rdrmnull` and
`virtio_gpu` together on `misc/drm_core` (see its header).

The carried helpers (R5e) build on illumos with the same kernel flags:

```sh
DDI_MAPFILE=<gate>/usr/src/uts/common/mapfiles/ddi.mapfile carried/build.sh
# OUT (./target/carried): drm_helpers, drm_helpers_test
carried/host-test.sh                 # the self-tests as a userland program
carried/fetch-and-patch -f           # vendored files = Linux at the pin
```

`tools/test/run-r5e-test.sh` installs, tests and removes both modules on a
disposable guest (see its header).

## Checks

```sh
# uAPI layouts and ioctl numbers against gcc on illumos
ILLUMOS_GATE=~/illumos-gate tools/uapi/generate.sh     # regenerate (bindgen)
gcc -m64 -I<include/drm> -o drm_layout tools/uapi/drm_layout.c && ./drm_layout > out   # on illumos
# the older virtio-gpu layouts, from libdrm 2.4.109's and Mesa 21.3's own headers
gcc -m64 -DV1 -I<libdrm 2.4.109 include/drm> -o c1 tools/uapi/virtgpu_compat_layout.c && ./c1 >> out
gcc -m64 -DV2 -I<mesa include/drm-uapi> -I<include/drm> -o c2 tools/uapi/virtgpu_compat_layout.c && ./c2 >> out
cargo xtask uapi-check out

# the facade's layouts against gcc on illumos
gcc -m64 -D_KERNEL -std=gnu99 -I include -S -o - tools/facade/facade_layout.c |
    sed -n 's/^.*@@ //p' | tr -d '"' > out     # on illumos
cargo xtask facade-check out            # committed: tools/facade/facade_layout.illumos.out
ILLUMOS_GATE=~/illumos-gate tools/facade/generate.sh   # regenerate bindings, mapfiles, tables

# DDI layouts and symbols of the pinned ddi-sys and the private crates,
# and drm-facade-sys's layouts (through misc/drm_core's CTF) and functions,
# against a running kernel (misc/drm_core must be loaded: modload -p misc/drm_core)
cargo xtask kernel-check --emit | ssh root@host 'mdb -k 2>&1' | cargo xtask kernel-check --verify -
```

## Continuous integration

`.forgejo/workflows/ci.yml` follows illumos-ddi's Solstice CI setup: one
job type-checks, lints, checks formatting, builds the documentation, runs
the host tests against the hosted illumos target and compares the uAPI
and facade layouts with the committed gcc output; a second job builds
`drm`, `rdrmnull` and `virtio_gpu` for the kernel target natively on
illumos and links them with `ld -ztype=kmod` (`misc/drm_core` with
`-B reduce` and `drm_core.mapfile`, whose global symbols it compares with the
mapfile, and with the facade's CTF where `ctfconvert` is installed; the
drivers with `-N misc/drm_core`, checking that neither carries a symbol of the
core's stateful modules), and builds `cdrmnull` with `-z defs` against
the gate's `ddi.mapfile`. Neither loads a module, which needs root on a
kernel that may panic (an R1 trial did panic the test machine, see
*System memory and DMA*). The same job builds `misc/drm_helpers` and
`drm_helpers_test` with `carried/build.sh` (vendored files against their
SHA-256, `-z defs`, the module's globals against its scope mapfile) and
runs `carried/host-test.sh` with gcc.

## Licence

MPL-2.0, see `LICENSE`. The generated uAPI bindings in `crates/drm-uapi`
and the patch in `tools/uapi/patches` are derived from MIT-licensed
headers and keep that licence; the notice is in `crates/drm-uapi/src/lib.rs`
and in the patch. The carried Linux files in `carried/*/linux/` keep their
own MIT notices (`carried/drm_mm/LICENSE`); `linuxkpi/` and the carried
helpers' tests are MIT, like the facade headers.
