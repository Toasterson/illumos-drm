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
(virtio-gpu), all five done, and R5 (C interface).

## Where things stand

Milestones R0, R1, R2, R3 and R4 work.

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
turned off (a core fix), the driver held the fence domain so the module
never unloaded, and the test's rate check. Mesa's virgl driver (Mesa
21.3.9) renders on the host through `virtio_gpu`: through EGL on a GBM
device, `GL_RENDERER` is `virgl` (GL 3.1), a cleared framebuffer reads
back exactly, and 600 clear-and-read-back frames take 0.45 s; drawn to a
GBM scanout surface and page-flipped, 600 frames show at 74.8 per second
on the 75 Hz mode, and a host screendump shows the picture. The module
then unloads with no resources left. Mesa's virgl driver cannot run under
X yet, because OpenIndiana's Xorg 1.19.7 is built without glamor, and it
needs a workaround for libdrm's device enumeration (see *R4: Mesa*).
The guest's disk is a transitional virtio-blk device (`disable-modern=on`),
because illumos bug 18358, fixed after `cda5b9a49f`, makes the virtio
framework map the wrong BAR for the second modern device.

## What is in the repository

`crates/drm-uapi` is the DRM userland interface as Rust types: the
`#[repr(C)]` argument structures and constants of `drm.h`, `drm_mode.h` and
`virtgpu_drm.h` from libdrm 2.4.109, generated with bindgen, and the
`DRM_IOCTL_*` numbers in the illumos `_IOWR` encoding. It has no
dependencies and can serve userland programs as well as the kernel.

`crates/drm` is the core, a `no_std` library on top of the `ddi` crate. A
driver implements the `drm::Driver` trait (name, date, description,
version, feature flags, mode-setting properties, bus ID, buffer-object
type, dumb-buffer rules, DMA attributes, memory limits, private ioctls)
and uses `drm::Device<D>` as the instance type of its `ddi::Driver`. The
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
`prime`.

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
R0, R1 and R3 tests and `modetest`). `crates/drm/tests/edid` holds the
EDID test blobs and `gen_edid.py`, which synthesizes them.
`docs/anon-fd.md` records how file descriptors are made for dma-bufs,
sync objects and sync files.
`packaging/devlink.tab` is the `/dev/dri` link rule for packages.

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
(`drm::uaccess`, `drm::gem`).

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
before R5, static linking costs nothing.

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
`renderD<128+N>` in the low nine bits, as in gfx-drm. Every open is a clone
open with a clone id from 1 to 511 in bits 9 to 17, the bits the illumos
libdrm patch strips (`0x3fe00`) to map an open file back to its `/dev/dri`
node. A device therefore has at most 511 open files at a time; the next
open fails with `EMFILE`. The driver instance number is the device index,
up to 63.

**GEM objects and references (R1).** A driver's buffer type implements
`drm::gem::GemObject` (size, the `ddi_umem` cookie its mappings show,
optional hooks for movable objects); the core wraps it in a counted
`GemInner` with its handle count, FLINK name and mmap offset, and hands out
`GemRef`s. The count is `drm::karc::KArc`, not `alloc::sync::Arc`: its
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

**System memory and DMA (R1).** `drm::gem::SysMem` is locked kernel memory
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
`ddi::Driver::attach` receives. `drm::Device::attach` takes that node and
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
errno Linux's DRM gives a failed object allocation (`drm::dmaerr`):
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

**Fences (R2).** `drm::fence` is Linux's `dma_fence`: a fence on a
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
reservation (`drm::resv`, Linux's `dma_resv`) holding fences with the
usages kernel, write, read and bookkeep. Adding a fence needs the
reservation's wound/wait mutex (`drm::wwmutex`), and readers iterate
under a short internal mutex (no RCU). The wound/wait mutex follows the
wait-die rule of Linux's reservation class: a transaction waits for
younger holders and backs off with `EDEADLK` from older ones while it
holds other mutexes of the class, then sleeps on the contended one with
`lock_slow` and starts over; `lock_all` implements the loop, and a host
test has two threads lock two mutexes in opposite orders at the same
moment.

**Sync objects (R2).** `drm::syncobj` is Linux's `drm_syncobj`: a binary
fence slot or a timeline of chain points, with Linux's point lookup and
query. `SYNCOBJ_WAIT` and `SYNCOBJ_TIMELINE_WAIT` wait for all or any of
several objects, for submissions (`WAIT_FOR_SUBMIT`) or only for them
(`WAIT_AVAILABLE`), with fence callbacks and submission waiters pointing
at one waiter on the waiting thread's stack that the cleanup removes
before it goes. The ioctls check their arguments and answer with the
errnos Linux does (`drm::syncobj_ioctl` lists the rules: which `pad`
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
file's existing handle or creates one, for dma-bufs of the same device
(`ENOTSUP` for another device of the driver, because importing across
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
| `SYNCOBJ_CREATE`, `DESTROY`, `HANDLE_TO_FD`, `FD_TO_HANDLE`, `WAIT`, `RESET`, `SIGNAL` | all files, render too | `EOPNOTSUPP` without `Features::SYNCOBJ`; Linux's argument checks and errnos (`drm::syncobj_ioctl`) |
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
(`EACCES`). Commands are matched on their complete 32-bit value, because
the illumos encoding keeps only eight bits of the argument size; any other
command is `ENOTTY`. A core command from a 32-bit process (recognised by
group and number, whatever its size) is `ENOTSUP`, because only the LP64
ABI exists; other commands from 32-bit processes are `ENOTTY` as usual.

**Concurrency.** All mutable device state of R0 is one `DeviceState` behind one
`ddi::Mutex`. Each entry point performs its permission check and its state
change in one critical section; copies from and to user memory happen
outside it. `crates/drm/src/device.rs` gives the full argument. The state
machine is tested on the host from eight threads with an invariant check
after every step, and the kernel side by `tools/test/drm_r0_test.c`, whose
eight unprivileged threads open, authenticate and close files while the
root master drops and retakes master in another process. The GEM state of
R1 has its own locks (name table, then a file's handle table, then an
object's mapping list; the offset, file and pending tables as leaves) and
never drops a reference with one held; `crates/drm/src/gem.rs` and
`crates/drm/src/objects.rs` give the argument, the host tests drive the
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

**Mode setting (R3): structure.** KMS lives in `crates/drm/src/kms/`:
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
`atomic_remove_fb`. Blobs and framebuffers are counted; one that userland
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
resources; a handle attaches the resource to the file's rendering context
and removing it detaches it (the new `GemObject::handle_opened` and
`handle_closed` hooks, Linux's GEM open and close callbacks), and a file's
context is destroyed in the new `Driver::file_close` hook. An object's
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
oi-userland's `hephaistos/virgl-gallium` branch. Two things outside this
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

## Building

Type checking, linting and host tests work on any host with stable Rust
and the hosted illumos target:

```sh
rustup target add x86_64-unknown-illumos
cargo check --workspace --target x86_64-unknown-illumos
cargo clippy --workspace --target x86_64-unknown-illumos -- -D warnings
cargo fmt --all -- --check
cargo test -p drm -p drm-uapi        # host target
```

The kernel module needs the pinned nightly with `rust-src`, because `core`
and `alloc` are rebuilt without SSE for `targets/x86_64-illumos-kmod.json`.
Compiling works on any host; linking needs the illumos linker:

```sh
cargo xtask build rdrmnull           # target/x86_64-illumos-kmod/release/rdrmnull.a
ld -ztype=kmod -N fs/specfs -z allextract rdrmnull.a -o rdrmnull    # on illumos, or: cargo xtask link rdrmnull
```

`tools/test/run-r1-test.sh` does the rest on a disposable illumos VM, as
root, in a directory with `rdrmnull.a`, `rdrmnull.conf`,
`tools/test/drm_r0_test.c`, `tools/test/drm_r1_test.c`,
`tools/test/drm_r2_test.c` and the patched headers as `drm/`: it records
the driver files in `/etc` and the `/devices` tree, links the module,
checks that the running kernel exports every symbol the module imports,
builds the test programs (64-bit with libdrm and libkstat, and 32-bit
probes), installs the module under `/usr/kernel/drv`, runs
`add_drv -m '* 0666 root sys' rdrmnull`, runs the R0, R1 and R2 tests,
records the `gem` and `sync` kstats and the device's vmem arenas, checks
that the module refuses to unload while a file is open and unloads once
it is closed, removes everything again with `rem_drv -C`, and fails
unless `/etc` and `/devices` are as before. `tools/test/run-r0-test.sh`
runs the R0 test alone.

The virtio-gpu driver is built the same way and linked against the
virtio framework too:

```sh
cargo xtask build virtio_gpu
ld -ztype=kmod -N misc/virtio -N fs/specfs -z allextract virtio_gpu.a -o virtio_gpu   # on illumos
```

`tools/test/run-r4-test.sh` links, installs (`add_drv -i
'"pci1af4,1050"'`), tests and removes it on a guest with a virtio-gpu
device.

## Checks

```sh
# uAPI layouts and ioctl numbers against gcc on illumos
ILLUMOS_GATE=~/illumos-gate tools/uapi/generate.sh     # regenerate (bindgen)
gcc -m64 -I<include/drm> -o drm_layout tools/uapi/drm_layout.c && ./drm_layout > out   # on illumos
# the older virtio-gpu layouts, from libdrm 2.4.109's and Mesa 21.3's own headers
gcc -m64 -DV1 -I<libdrm 2.4.109 include/drm> -o c1 tools/uapi/virtgpu_compat_layout.c && ./c1 >> out
gcc -m64 -DV2 -I<mesa include/drm-uapi> -I<include/drm> -o c2 tools/uapi/virtgpu_compat_layout.c && ./c2 >> out
cargo xtask uapi-check out

# DDI layouts and symbols of the pinned ddi-sys and the private crates
# against a running kernel
cargo xtask kernel-check --emit | ssh root@host 'mdb -k 2>&1' | cargo xtask kernel-check --verify -
```

## Continuous integration

`.forgejo/workflows/ci.yml` follows illumos-ddi's Solstice CI setup: one
job type-checks, lints, checks formatting, builds the documentation and
runs the host tests against the hosted illumos target; a second job builds
`rdrmnull` and `virtio_gpu` for the kernel target natively on illumos and
links them with `ld -ztype=kmod`. Neither loads the module, which needs root on a kernel
that may panic (an R1 trial did panic the test machine, see *System memory
and DMA*).

## Licence

MPL-2.0, see `LICENSE`. The generated uAPI bindings in `crates/drm-uapi`
and the patch in `tools/uapi/patches` are derived from MIT-licensed
headers and keep that licence; the notice is in `crates/drm-uapi/src/lib.rs`
and in the patch.
