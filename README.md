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
(this state: uAPI, device registration, per-file state, core ioctls), R1
(GEM and dumb buffers), R2 (fences, syncobj, PRIME), R3 (KMS and atomic),
R4 (virtio-gpu) and R5 (C interface).

## Where things stand

Milestone R0 works. On 28 September 2026 the test driver `rdrmnull` was
built for the kernel target, linked with `ld -ztype=kmod`, loaded with
`add_drv` on OpenIndiana Hipster 2026.04 (illumos `cda5b9a49f`), and
exercised through the installed libdrm 2.4.109: `drmOpen("rdrmnull",
NULL)` finds and opens `/dev/dri/card0`, `drmGetVersion`, `drmGetCap`,
`drmSetClientCap`, `drmSetInterfaceVersion`, `drmGetBusid`/`drmSetBusid`,
`drmGetMagic` with `drmAuthMagic` between an unprivileged client and a root
master, `drmGetClient`, `drmSetMaster`/`drmDropMaster`, and
`drmOpenRender(128)` as root and as `nobody` all behave as on Linux. Eight
unprivileged threads opening, authenticating and closing concurrently while
the root master drops and retakes master, the 511-file limit, a 32-bit
caller (refused with `ENOTSUP`) and the error paths were tested too, and
the module unloaded cleanly with `rem_drv`. Every size, offset and
ioctl number of the Rust uAPI matches what gcc computes on illumos from the
same headers.

## What is in the repository

`crates/drm-uapi` is the DRM userland interface as Rust types: the
`#[repr(C)]` argument structures and constants of `drm.h`, `drm_mode.h` and
`virtgpu_drm.h` from libdrm 2.4.109, generated with bindgen, and the
`DRM_IOCTL_*` numbers in the illumos `_IOWR` encoding. It has no
dependencies and can serve userland programs as well as the kernel.

`crates/drm` is the core, a `no_std` library on top of the `ddi` crate. A
driver implements the `drm::Driver` trait (name, date, description,
version, feature flags, mode-setting properties, bus ID) and uses
`drm::Device<D>` as the instance type of its `ddi::Driver`. The device
creates the minor nodes `card<N>` and `renderD<128+N>`, keeps per-file
state through clone opens, and answers the core ioctls. The decision
logic (`state`, `caps`, `ioctl`, `minor`) is plain data and is unit-tested
on the host; `device` and `uaccess` are the thin kernel layer.

`examples/rdrmnull` is a DRM driver without hardware. It registers one
device with a card and a render node and is the kernel module the core is
tested with, as well as the template for a real driver.

`xtask` builds and links kernel modules and checks interfaces (see below).
`tools/uapi` regenerates `drm-uapi` and holds the C half of the layout
check. `tools/test` holds the libdrm test program and the script that
links, loads, tests and unloads `rdrmnull` on an illumos machine.
`packaging/devlink.tab` is the `/dev/dri` link rule for packages.

## Decisions

**Dependency on illumos-ddi.** `ddi-sys` and `ddi` are a git dependency on
<https://code.aopc.cloud/illumos/illumos-ddi.git>, pinned by revision in
the workspace `Cargo.toml`. The pinned revision is the `drm-r0` branch:
illumos-ddi `main` (`eb18ab93b7`) plus clone-open support in the
`CharDevice` glue (`CharDevice::clone_open`, commits `8463d9da92` and
`f36d47958e`). Per-file DRM
state is impossible without clone opens, because a character device
otherwise sees only the last `close(9E)` of a device number, and the
change belongs in the framework glue rather than in a copy of it. Anything
else the safe `ddi` crate does not wrap yet (copying ioctl structures that
contain pointers, and the buffers they point to) is done with `ddi-sys`
inside the core (`drm::uaccess`).

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

**Ioctl semantics.** The permission flags and the behaviour follow Linux
(`drm_ioctls[]`, `drm_auth.c`, `drm_getcap`, `drm_setclientcap`):

| Ioctl | Allowed for | Notes |
| --- | --- | --- |
| `VERSION` | all files, render too | strings copied up to the caller's lengths, full lengths returned |
| `GET_CAP` | all files, render too | `TIMESTAMP_MONOTONIC` 1; `PRIME`, `SYNCOBJ`, `SYNCOBJ_TIMELINE` 0 until R2; `DUMB_BUFFER` 0 until R1; vblank and page-flip capabilities 0 until R3; `CURSOR_WIDTH/HEIGHT` and `ADDFB2_MODIFIERS` from the driver's `ModeConfig`; everything else `EOPNOTSUPP` for a driver without `MODESET`, unknown `EINVAL` otherwise |
| `SET_CLIENT_CAP` | primary | `STEREO_3D`, `UNIVERSAL_PLANES`, `ATOMIC` (needs `Features::ATOMIC`, also sets universal planes and aspect ratio, values 0 to 2), `ASPECT_RATIO`, `WRITEBACK_CONNECTORS` (needs `ATOMIC` first), values 0 or 1; Linux's refusal of `ATOMIC` = 1 to processes named `X*` needs the process name and comes with R3 |
| `GET_UNIQUE` | primary | empty until the master sets interface 1.1 or later or calls `SET_UNIQUE`, as `drmOpenByName` expects |
| `SET_UNIQUE` | authenticated master with `PRIV_SYS_DEVICES` | only the device's own bus ID, once (`EBUSY` after) |
| `SET_VERSION` | master | interface 1.0 to 1.4, driver version checked; versions reported back in every case |
| `GET_MAGIC` | primary | per-master namespace; the number stays with the file |
| `AUTH_MAGIC` | authenticated master | consumes the magic; unknown or consumed magic `EINVAL` |
| `GET_CLIENT` | primary | index 0 only (the caller): authentication state, the caller's pid, the anonymous uid, as on Linux |
| `SET_MASTER` | primary | needs `PRIV_SYS_DEVICES`, or a former master used by its opening process |
| `DROP_MASTER` | primary | same permission, only by the current master |

The first file opened on a card node while the device has no master
becomes master. Files of openers with `PRIV_SYS_DEVICES` (the illumos
counterpart of `CAP_SYS_ADMIN`) are authenticated from the start. Render
nodes never need authentication and cannot use the primary-only ioctls
(`EACCES`). Commands are matched on their complete 32-bit value, because
the illumos encoding keeps only eight bits of the argument size; any other
command is `ENOTTY`. A core command from a 32-bit process (recognised by
group and number, whatever its size) is `ENOTSUP`, because only the LP64
ABI exists; other commands from 32-bit processes are `ENOTTY` as usual.

**Concurrency.** All mutable device state is one `DeviceState` behind one
`ddi::Mutex`. Each entry point performs its permission check and its state
change in one critical section; copies from and to user memory happen
outside it. `crates/drm/src/device.rs` gives the full argument. The state
machine is tested on the host from eight threads with an invariant check
after every step, and the kernel side by `tools/test/drm_r0_test.c`, whose
eight unprivileged threads open, authenticate and close files while the
root master drops and retakes master in another process.

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

`tools/test/run-r0-test.sh` does the rest on a disposable illumos VM, as
root, in a directory with `rdrmnull.a`, `rdrmnull.conf`,
`tools/test/drm_r0_test.c` and the patched headers as `drm/`: it links the
module, checks that the running kernel exports every symbol the module
imports, builds the test programs, installs the module under
`/usr/kernel/drv`, runs `add_drv -m '* 0666 root sys' rdrmnull`, runs the
tests, and removes everything again.

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
that may panic.

## Licence

MPL-2.0, see `LICENSE`. The generated uAPI bindings in `crates/drm-uapi`
and the patch in `tools/uapi/patches` are derived from MIT-licensed
headers and keep that licence; the notice is in `crates/drm-uapi/src/lib.rs`
and in the patch.
