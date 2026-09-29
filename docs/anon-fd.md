# Anonymous file descriptors for dma-bufs, sync objects and sync files

Decided and implemented for milestone R2 (29 September 2026, work item
WI-454). Code: `crates/drm/src/anon_fd.rs` in this repository and
`ddi::private::fd` over `illumos-kernel-private-sys` in
[illumos-ddi](https://code.aopc.cloud/illumos/illumos-ddi).

## The problem

Linux hands three kinds of kernel object to user space as file
descriptors that an ioctl returns: a dma-buf (`PRIME_HANDLE_TO_FD`), a
sync object (`SYNCOBJ_HANDLE_TO_FD`) and a sync file (a fence,
`SYNCOBJ_HANDLE_TO_FD` with `EXPORT_SYNC_FILE`, and `SYNC_IOC_MERGE`).
Linux makes them with `anon_inode_getfile`. illumos has no anonymous
inodes and no DDI interface that creates a file descriptor from a driver.
The descriptor must

* survive `SCM_RIGHTS` and `fork(2)`, as any descriptor does;
* drop the object reference exactly once, when the last descriptor that
  refers to the file is closed;
* support `poll(2)` on sync files, woken from the context that signals the
  fence (an interrupt);
* support `mmap(2)` on dma-bufs, because Mesa maps dma-bufs;
* never let anything reach into driver code or data after the driver has
  detached;
* be recognisable when user space passes it back (`PRIME_FD_TO_HANDLE`,
  `SYNCOBJ_FD_TO_HANDLE`, `IMPORT_SYNC_FILE`).

## The mechanism the gate offers

`pipe(2)` (`uts/common/syscall/pipe.c`) shows how kernel code gives the
calling process a new descriptor for a vnode: `falloc()` reserves a
descriptor and allocates a `file_t` that takes over a vnode reference and
returns locked; after the vnode is opened, the lock is dropped and
`setf()` makes the descriptor visible; `f_setfd_or()` sets close-on-exec.
The undo path before `setf()` is `setf(fd, NULL)` and `unfalloc()`. On the
way back, `getf()` holds a descriptor of the calling process until
`releasef()`. None of these has a manual page or a mapfile entry; they are
the first items of illumos-ddi's private tier (`illumos-kernel-private-sys`,
pinned to gate `cda5b9a49f`, usable only with the
`unstable-gate-cda5b9a49f` feature).

The question is which vnode the file gets.

## Alternatives

**A. A specfs vnode for a private minor of the DRM driver (chosen).** Each
exported object gets an *anonymous minor* of the driver itself, and the
ioctl opens that device number the way the layered driver interface does
(`ldi_vp_from_dev` in `uts/common/os/driver_lyr.c`): `makespecvp()`, then
`VOP_OPEN()`, which is specfs's `spec_open()` and calls the driver's own
`open(9E)` for the minor. From then on every operation on the file reaches
the driver's `cb_ops` entry points with that minor: `close(9E)` on the last
close of the device number (specfs counts opens per device number in the
common snode, so `dup(2)`, `fork(2)`, `SCM_RIGHTS` and a second export of
the same dma-buf all share one count), `chpoll(9E)` for `poll(2)`,
`segmap(9E)`/`devmap(9E)` for `mmap(2)`, `ioctl(9E)` for the sync file and
dma-buf ioctls. specfs holds the device node while any such file is open,
so the driver cannot detach (the same mechanism that keeps a driver
loaded while `/dev/dri/card0` is open), and no vnode operation of ours
exists that could outlive the module: the vnode operations are specfs's.
An import checks that the descriptor is a character device with the
driver's major number and looks the object up by minor.

**B. A small clone-open pseudo driver.** `eventfd`, `timerfd` and
`signalfd` (`uts/common/io/*.c`) are pseudo drivers whose objects live
behind clone opens of `/dev/eventfd` and friends, but their descriptors
come from `open(2)` in libc, not from an ioctl. Using the pattern for DRM
would mean a second driver (a separate module that DRM drivers depend on
for the object's code, or one per DRM driver), a way to pass the object
from the ioctl to the clone open (a per-thread slot), and cross-module
calls into the DRM core for close, poll and mmap. It gains nothing over A,
which uses the DRM driver's own minor space and entry points.

**C. A private vnode with vnode operations of our own** (`vn_alloc`,
`vn_make_ops`), the way sockfs and fifofs make theirs. This gives full
control but no `mmap(2)` for free (`VOP_MAP` would have to build the
devmap segment itself), needs our own reference counting against module
unload (a vnode whose operations are in an unloaded module is a use after
free), and binds much more private interface (`vnodeops_t`, the
`fs_operation_def_t` templates, `vn_*`).

A keeps the private surface to a dozen functions and two structure
prefixes, and gets close, poll, mmap, `fstat(2)` and detach safety from
specfs.

## Details of A

**Minor numbers.** Bit 30 set, the device index (instance) in bits 24 to
29, an object number from 1 to 2^24 - 1 below (`crates/drm/src/anon_fd.rs`).
DRM file minors use the low 18 bits, so the ranges are disjoint;
`drm::minor::instance_of_minor` maps both to the instance, which
`getinfo(9E)` and `spec_open()` need. The numbers exceed `L_MAXMIN32`, so a
32-bit process that receives such a descriptor gets `EOVERFLOW` from
`fstat(2)`; the DRM core only supports LP64 callers anyway.

**Opens.** An anonymous minor can only be opened by the export path: its
table entry counts booked opens, and `open(9E)` succeeds only while one
is booked. `close(9E)` keeps an entry, and its number, while an open is
booked, so that an export racing with the last close of the same dma-buf
opens the object it booked (`AnonTable` in `crates/drm/src/anon_fd.rs`,
with a host stress test of exports against last closes). A device node made with `mknod(2)` for the number cannot be
opened. The export first creates the entry, then opens, then copies the
descriptor number out to the ioctl argument, and only then installs the
descriptor with `setf()`; if the copy fails, the open is undone
(`VOP_CLOSE`, which ends in `close(9E)`, `setf(fd, NULL)`, `unfalloc()`,
`VN_RELE`), so a failed ioctl leaves no descriptor behind.

**One dma-buf per object.** While a GEM object's dma-buf file is open,
exporting the object again opens the same minor again (Linux keeps
`obj->dma_buf`), so both descriptors have the same `st_rdev`. If that
races with the last close, the booked open keeps the entry (see *Opens*)
and the export reopens the same dma-buf.

**Paths.** `makespecvp()` gives the vnode no path, and the devmap
framework cannot map such a vnode: `segdev_create()` looks the device up
with `specfind()`, which only returns vnodes whose path is visible in the
caller's zone, and dereferences the NULL it gets. The first R2 run
panicked the test machine exactly there (29 September 2026, see
illumos-ddi `docs/DESIGN.md` section 5.12). `ddi::private::fd` therefore
copies the path of the caller's own file (found with `specfind()` for the
device number of the ioctl, which only the `ddi` glue can record in the
`IoctlArg`) to the new vnode, and refuses when there is none; the `ddi`
`devmap(9E)` glue refuses to map a device `specfind()` cannot find (a
descriptor passed into another zone). A dma-buf descriptor therefore
shows the path of the DRM node it was exported from in `pfiles(1)`.

**Mappings.** A dma-buf maps from offset 0 (Linux's
`dma_buf_mmap_internal`), shared only, within the object; `PROT_WRITE`
needs `DRM_RDWR`, which the kernel's `mmap(2)` checks against the file's
mode. The mapping machinery is the one of DRM files (`crates/drm/src/gem.rs`),
with the object's base offset 0 instead of its fake offset. Refusals
happen in `segmap(9E)` (illumos-ddi's `CharDevice::segmap_check`), because
a mapping that `devmap_map(9E)` refuses stays in the process until it
exits (`devmap_setup()` does not unmap it) and keeps the file open.

**Poll.** A sync file registers a fence callback when it is created; the
callback calls `pollwakeup(9F)` in the signaller's context (a soft
interrupt in `rdrmnull`). `chpoll(9E)` reports `POLLIN` once the fence has
signalled and registers the file's `pollhead` otherwise. The sync file
removes the callback, waiting for a running one, before its `pollhead` is
cleaned and freed in `close(9E)`. Polling a dma-buf (implicit
synchronisation through its reservation, `POLLIN` for the write fences,
`POLLOUT` for all) is not implemented yet and fails with `ENOTSUP`; sync
object files are always ready, as on Linux.

**Imports.** `getf()`, check `v_type == VCHR` and the major number,
decode the minor, look the entry up in the table of the device the minor
names (it exists while the held file is open, and the held file keeps
that device attached), `releasef()`. Sync object files and sync files of
any device of the driver are accepted; a dma-buf of another device is
`ENOTSUP` (cross-device import needs attach and a scatter/gather mapping,
which no driver has yet); any other file is `EINVAL`, including files of
other drivers built on this core, which link their own copy of it.

**Context.** Descriptors belong to the calling process, so they are only
created and looked up inside an ioctl of a user process:
`ddi::private::fd` takes the `IoctlArg` of the ioctl being served and
refuses LDI callers (`FKIOCTL`) and kernel threads (`ddi_get_pid(9F)` 0).

## Verification

`tools/test/drm_r2_test.c`, run by `tools/test/run-r1-test.sh`: a dma-buf
passed over `SCM_RIGHTS` to an exec'd child, imported there on a card and
a render file and mapped through the handle and the descriptor, with the
contents shared both ways; one device number per dma-buf; close-on-exec
honoured across `exec`; read-only exports; refused mappings; a sync object
and a sync file passed to another process; `poll(2)` of sync files woken
by the soft interrupt (DTrace showed every `pollwakeup(9F)` of a
signalled sync file in an interrupt thread at PIL 1); the `sync` kstat's
`dmabufs` and `anon_files` back at zero after every step; and the module
unloading afterwards. illumos-ddi's `rselftest`, built with its
`private-gate` feature, checks the guard (an argument not from the glue
is refused) and the descriptor it creates.
