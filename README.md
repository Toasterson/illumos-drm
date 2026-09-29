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
buffers), both done, R2 (fences, syncobj, PRIME), R3 (KMS and atomic),
R4 (virtio-gpu) and R5 (C interface).

## Where things stand

Milestones R0 and R1 work.

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
layer.

`examples/rdrmnull` is a DRM driver without hardware. It registers one
device with a card and a render node, has dumb buffers in system memory
and three test-only private ioctls (movable objects for the refault
experiment, and a reference dropped from a timer), and is the kernel
module the core is tested with, as well as the template for a real driver.

`xtask` builds and links kernel modules and checks interfaces (see below).
`tools/uapi` regenerates `drm-uapi` and holds the C half of the layout
check. `tools/test` holds the libdrm test programs of R0 and R1 and the
scripts that link, load, test and unload `rdrmnull` on an illumos machine
(`run-r1-test.sh` runs both tests).
`packaging/devlink.tab` is the `/dev/dri` link rule for packages.

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
cannot outlive their device. Differences from Linux: every refused
`mmap(2)` is `ENXIO`, because `devmap_setup(9F)` reports all driver errors
as `ENXIO` (Linux: `EINVAL` for a bad offset, `EACCES` for a file without
a handle); mappings are cached, write-combining needs the private tier.

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
the driver's `ModeConfig`; `ADDFB2_MODIFIERS` stays 0 until R3.

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

**Ioctl semantics.** The permission flags and the behaviour follow Linux
(`drm_ioctls[]`, `drm_auth.c`, `drm_getcap`, `drm_setclientcap`):

| Ioctl | Allowed for | Notes |
| --- | --- | --- |
| `VERSION` | all files, render too | strings copied up to the caller's lengths, full lengths returned |
| `GET_CAP` | all files, render too | `TIMESTAMP_MONOTONIC` 1; `PRIME`, `SYNCOBJ`, `SYNCOBJ_TIMELINE` 0 until R2; `DUMB_BUFFER` 1 with `GEM` and `DUMB`; vblank and page-flip capabilities 0 until R3; `CURSOR_WIDTH/HEIGHT`, `DUMB_PREFERRED_DEPTH` and `DUMB_PREFER_SHADOW` from the driver's `ModeConfig`; `ADDFB2_MODIFIERS` 0 until `ADDFB2` exists (R3), whatever the `ModeConfig`; everything else `EOPNOTSUPP` for a driver without `MODESET`, unknown `EINVAL` otherwise |
| `SET_CLIENT_CAP` | primary | `STEREO_3D`, `UNIVERSAL_PLANES`, `ATOMIC` (needs `Features::ATOMIC`, also sets universal planes and aspect ratio, values 0 to 2), `ASPECT_RATIO`, `WRITEBACK_CONNECTORS` (needs `ATOMIC` first), values 0 or 1; Linux's refusal of `ATOMIC` = 1 to processes named `X*` needs the process name and comes with R3 |
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
against the kernel.

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
ld -ztype=kmod -z allextract rdrmnull.a -o rdrmnull    # on illumos, or: cargo xtask link rdrmnull
```

`tools/test/run-r1-test.sh` does the rest on a disposable illumos VM, as
root, in a directory with `rdrmnull.a`, `rdrmnull.conf`,
`tools/test/drm_r0_test.c`, `tools/test/drm_r1_test.c` and the patched
headers as `drm/`: it records the driver files in `/etc` and the `/devices`
tree, links the module, checks that the running kernel exports every
symbol the module imports, builds the test programs (64-bit with libdrm and
libkstat, and 32-bit probes), installs the module under `/usr/kernel/drv`,
runs `add_drv -m '* 0666 root sys' rdrmnull`, runs the R0 and R1 tests,
records the `gem` kstat and the device's vmem arenas, checks that the
module refuses to unload while a file is open and unloads once it is
closed, removes everything again with `rem_drv -C`, and fails unless `/etc`
and `/devices` are as before. `tools/test/run-r0-test.sh` runs the R0 test
alone.

## Checks

```sh
# uAPI layouts and ioctl numbers against gcc on illumos
ILLUMOS_GATE=~/illumos-gate tools/uapi/generate.sh     # regenerate (bindgen)
gcc -m64 -I<include/drm> -o drm_layout tools/uapi/drm_layout.c && ./drm_layout > out   # on illumos
cargo xtask uapi-check out

# DDI layouts and symbols of the pinned ddi-sys against a running kernel
cargo xtask kernel-check --emit | ssh root@host 'mdb -k 2>&1' | cargo xtask kernel-check --verify -
```

## Continuous integration

`.forgejo/workflows/ci.yml` follows illumos-ddi's Solstice CI setup: one
job type-checks, lints, checks formatting, builds the documentation and
runs the host tests against the hosted illumos target; a second job builds
`rdrmnull` for the kernel target natively on illumos and links it with
`ld -ztype=kmod`. Neither loads the module, which needs root on a kernel
that may panic (an R1 trial did panic the test machine, see *System memory
and DMA*).

## Licence

MPL-2.0, see `LICENSE`. The generated uAPI bindings in `crates/drm-uapi`
and the patch in `tools/uapi/patches` are derived from MIT-licensed
headers and keep that licence; the notice is in `crates/drm-uapi/src/lib.rs`
and in the patch.
