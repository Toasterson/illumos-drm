// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! A DRM driver without hardware.
//!
//! `rdrmnull` registers one DRM device per instance of its pseudo node
//! (`rdrmnull.conf` asks for instance 0), with
//! the primary node `card0` and the render node `renderD128`. It declares
//! mode setting and atomic support so that every client capability and
//! every mode-setting capability path of the core can be exercised. Since
//! milestone R3 it has a virtual display (the `kms` module): one connector
//! with a fixed EDID, one encoder, one CRTC with a primary and a cursor
//! plane, a software vblank and a software scanout.
//!
//! Since milestone R1 it has GEM objects and dumb buffers in system memory.
//! As a pseudo device it does no DMA (the pseudo nexus refuses DMA
//! handles), so its objects have no scatter/gather table.
//!
//! The string property `dma-mode` in `rdrmnull.conf` is a test aid, the DMA
//! drill; nothing but the kernel tests should set it:
//!
//! * `"none"` (or no property): no DMA, as above.
//! * `"self"`: bind every object for DMA against the driver's own node.
//!   This only works when the node's parent is a nexus that does DMA, so
//!   the `.conf` must attach it under the root nexus
//!   (`tools/test/rdrmnull-dma-self.conf`); such a node leaves its instance
//!   in `/etc/path_to_inst` after `rem_drv`. In this mode only, the
//!   integer property `dma-max-transfer` lowers the largest transfer of the
//!   DMA attributes (`dma_attr_maxxfer`) to that many bytes, so that the
//!   bind of any larger object fails (`tools/test/rdrmnull-dma-self-tiny.conf` and
//!   `drm_dma_errno_test.c` record the errno userland gets).
//! * `"root-node"`: the drill that reproduced the 2026-09-28 root-node
//!   panic. It needed the core to allocate DMA handles against a node the
//!   driver does not own; since R5a the core is the module `misc/drm_core`,
//!   whose facade takes DMA attributes only for the driver's own node
//!   (`drm_dev_illumos_set_dma_attr`), so the drill is gone and the mode is
//!   refused at attach. illumos-ddi makes the panic impossible by type
//!   (`OwnNode`) anyway.
//!
//! It also has five driver-private ioctls that exist only for the kernel
//! tests (`tools/test/drm_r1_test.c`, `drm_r3_test.c`) and are not an
//! interface for anything else:
//!
//! * `RDRMNULL_IOCTL_CREATE_MOVABLE` creates an object with two backings,
//!   the stand-in for an object that the memory manager of a real GPU moves
//!   between system memory and VRAM.
//! * `RDRMNULL_IOCTL_MOVE` invalidates every user mapping of such an object
//!   with `devmap_unload(9F)` and, unless asked not to, switches the object
//!   to its other backing with `devmap_umem_remap` first. The next access
//!   through an existing mapping faults into `devmap_access(9E)`, which
//!   loads the translation with `devmap_load(9F)`. This is the refault path
//!   that movable objects need (milestone R5), proven in R1.
//! * `RDRMNULL_IOCTL_PUT_LATER` takes a reference to an object and drops it
//!   from a `timeout(9F)` callback, in interrupt context, which exercises
//!   the deferred release of the core.
//! * `RDRMNULL_IOCTL_SCANOUT` reports the framebuffer the software scanout
//!   shows, the vblank and latch counts, and a checksum of the visible
//!   pixels.
//! * `RDRMNULL_IOCTL_HOLD` keeps a reference to an object in the driver
//!   until it is asked to drop it, also across the close of every file, the
//!   stand-in for a buffer a real driver still uses. `detach(9E)` is then
//!   refused, and the test checks that the device still works.
//!
//! Since milestone R2 it has a test timeline: a fence context of the
//! device whose fences are signalled from a soft interrupt, as a GPU
//! driver signals them from its completion interrupt. Two more test-only
//! ioctls use it (`tools/test/drm_r2_test.c`):
//!
//! * `RDRMNULL_IOCTL_FENCE` creates a fence on the timeline that signals
//!   after `delay_ms` milliseconds (optionally with an error), and puts it
//!   into a sync object at a point, into a buffer's reservation (as a
//!   writer or a reader), and/or into a new sync file whose descriptor it
//!   returns. A `timeout(9F)` callback triggers the soft interrupt, whose
//!   handler signals every fence that is due; the fence's callbacks
//!   (`pollwakeup(9F)` of a sync file, the wake-up of a sync object wait)
//!   run there, in interrupt context.
//! * `RDRMNULL_IOCTL_BO_WAIT` waits for a buffer's reservation, as a
//!   driver's wait ioctl (virtio-gpu's `WAIT`) does.
//!
//! A detach attempt cancels the timers and signals the fences that are
//! left with `ECANCELED` (`before_detach`), because every fence must
//! signal. The timeline itself stays connected until the detach is sure to
//! complete (`before_teardown`), so a refused detach leaves it working.
//!
//! It is the kernel module the core is tested with, and the template for a
//! Rust driver on the facade (milestone R5a): implement
//! [`drm_facade::Driver`], register a [`drm_facade::Device`] as the
//! instance, point `cb_ops` at the core (`drm_facade::CB_OPS`) and map
//! minor numbers with [`drm_facade::instance_of_minor`]. The core itself
//! is the module `misc/drm_core`, which every DRM driver links against
//! (`-N misc/drm_core`); the driver calls nothing of it but the facade's C
//! functions, through `drm-facade`.
#![no_std]

extern crate alloc;

mod kms;

use alloc::boxed::Box;

use core::ffi::{CStr, c_int};
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicPtr, AtomicU64, AtomicUsize, Ordering};

use ddi::DmaAttr;
use ddi::prelude::*;
use ddi::sys;
use drm_facade::device::DumbCreated;
use drm_facade::dumb::{self, DumbRequest};
use drm_facade::gem::{Fault, GemAny, Mapping};
use drm_facade::kvec::KVec;
use drm_facade::sync::sync_file_export;
use drm_facade::sys as fsys;
use drm_facade::{Dev, Device, Fence, File, Gem, GemObject, GemRef, Ioctl, IoctlData, Plain, Settings, Syncobj, Usage};
use drm_uapi::ioc;

ddi::module! {
    driver: RDrmNull,
    cb_ops: unsafe drm_facade::CB_OPS.get(),
}

/// The `ddi::Driver` of the module.
pub struct RDrmNull;

// --- the test-only ioctls ---------------------------------------------------

/// The argument of `RDRMNULL_IOCTL_CREATE_MOVABLE`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CreateMovable {
    /// The size in bytes, rounded up to the page size.
    pub size: u64,
    /// The new handle (out).
    pub handle: u32,
    /// Must be 0.
    pub pad: u32,
}

/// The argument of `RDRMNULL_IOCTL_MOVE`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Move {
    /// The object.
    pub handle: u32,
    /// `MOVE_FILL` (with the fill byte in the low eight bits),
    /// `MOVE_UNLOAD_ONLY`.
    pub flags: u32,
    /// The mapping pieces handled (out).
    pub pieces: u32,
    /// The backing in use afterwards (out).
    pub backing: u32,
}

/// The argument of `RDRMNULL_IOCTL_PUT_LATER`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PutLater {
    /// The object.
    pub handle: u32,
    /// The delay, 1 to 10000 ms.
    pub delay_ms: u32,
}

/// The argument of `RDRMNULL_IOCTL_HOLD`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Hold {
    /// The object (for `hold` 1).
    pub handle: u32,
    /// 1 to hold, 0 to release.
    pub hold: u32,
}

/// The argument of `RDRMNULL_IOCTL_FENCE`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NullFence {
    /// When the fence signals, 0 to 10000 ms from now.
    pub delay_ms: u32,
    /// `FENCE_*`.
    pub flags: u32,
    /// The sync object (with `FENCE_SYNCOBJ`).
    pub syncobj: u32,
    /// The buffer (with `FENCE_BO_WRITE` or `FENCE_BO_READ`).
    pub bo: u32,
    /// The timeline point (with `FENCE_SYNCOBJ`; 0 replaces the fence).
    pub point: u64,
    /// The sync file (out, with `FENCE_SYNC_FILE`).
    pub fd: i32,
    /// A positive errno to signal with, or 0.
    pub error: i32,
    /// The fence's number (out).
    pub seqno: u64,
}

/// The argument of `RDRMNULL_IOCTL_BO_WAIT`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct BoWait {
    /// The buffer.
    pub handle: u32,
    /// `BO_WAIT_WRITERS`.
    pub flags: u32,
    /// The timeout; negative waits for ever.
    pub timeout_ns: i64,
}

/// `FENCE`: put the fence into a sync object.
pub const FENCE_SYNCOBJ: u32 = 1 << 0;
/// `FENCE`: put it into a buffer's reservation as a writer.
pub const FENCE_BO_WRITE: u32 = 1 << 1;
/// `FENCE`: as a reader.
pub const FENCE_BO_READ: u32 = 1 << 2;
/// `FENCE`: hand out a sync file of it.
pub const FENCE_SYNC_FILE: u32 = 1 << 3;
const FENCE_FLAGS: u32 = FENCE_SYNCOBJ | FENCE_BO_WRITE | FENCE_BO_READ | FENCE_SYNC_FILE;
/// `BO_WAIT`: wait for the writers only.
pub const BO_WAIT_WRITERS: u32 = 1 << 0;

// SAFETY: integer fields only, no padding.
unsafe impl Plain for NullFence {}
// SAFETY: as above.
unsafe impl Plain for BoWait {}
const _: () = assert!(size_of::<NullFence>() == 40 && size_of::<BoWait>() == 16);
// SAFETY: integer fields only, no padding.
unsafe impl Plain for CreateMovable {}
// SAFETY: as above.
unsafe impl Plain for Move {}
// SAFETY: as above.
unsafe impl Plain for PutLater {}
// SAFETY: as above.
unsafe impl Plain for Hold {}
const _: () = assert!(size_of::<Hold>() == 8);
const _: () = assert!(size_of::<CreateMovable>() == 16 && size_of::<Move>() == 16 && size_of::<PutLater>() == 8);

/// `MOVE`: fill the new backing with the low byte of the flags first.
pub const MOVE_FILL: u32 = 1 << 8;
/// `MOVE`: only unload the mappings.
pub const MOVE_UNLOAD_ONLY: u32 = 1 << 9;
const MOVE_FLAGS: u32 = MOVE_FILL | MOVE_UNLOAD_ONLY | 0xff;

/// `DRM_IOWR(DRM_COMMAND_BASE + 0, struct rdrmnull_create_movable)`.
pub const RDRMNULL_IOCTL_CREATE_MOVABLE: u32 = ioc::drm_iowr::<CreateMovable>(0x40);
/// `DRM_IOWR(DRM_COMMAND_BASE + 1, struct rdrmnull_move)`.
pub const RDRMNULL_IOCTL_MOVE: u32 = ioc::drm_iowr::<Move>(0x41);
/// `DRM_IOW(DRM_COMMAND_BASE + 2, struct rdrmnull_put_later)`.
pub const RDRMNULL_IOCTL_PUT_LATER: u32 = ioc::drm_iow::<PutLater>(0x42);
/// `DRM_IOR(DRM_COMMAND_BASE + 3, struct rdrmnull_scanout)`: what the
/// software scanout shows.
pub const RDRMNULL_IOCTL_SCANOUT: u32 = ioc::drm_ior::<kms::ScanoutInfo>(0x43);
/// `DRM_IOW(DRM_COMMAND_BASE + 4, struct rdrmnull_hold)`.
pub const RDRMNULL_IOCTL_HOLD: u32 = ioc::drm_iow::<Hold>(0x44);
/// `DRM_IOWR(DRM_COMMAND_BASE + 5, struct rdrmnull_fence)`.
pub const RDRMNULL_IOCTL_FENCE: u32 = ioc::drm_iowr::<NullFence>(0x45);
/// `DRM_IOW(DRM_COMMAND_BASE + 6, struct rdrmnull_bo_wait)`.
pub const RDRMNULL_IOCTL_BO_WAIT: u32 = ioc::drm_iow::<BoWait>(0x46);

// SAFETY: integer fields only, no padding (asserted below).
unsafe impl Plain for kms::ScanoutInfo {}
const _: () = assert!(size_of::<kms::ScanoutInfo>() == 56);

// --- objects ------------------------------------------------------------------

// `devmap_umem_remap`: points an existing mapping at other kernel memory
// and unloads its translations.
//
// It is prototyped in `sys/sunddi.h`, implemented in `vm/seg_dev.c` and
// exported by `genunix` (the `rsm` and InfiniBand drivers use it), but it
// has no manual page and is not in the `ddi` mapfile, so `ddi-sys` does
// not bind it. It is declared here for the test-only movable objects; the
// DRM core does not use it. The movable-object layer of milestone R5
// needs it bound, in the private tier or after a manual page upstream.
unsafe extern "C" {
    fn devmap_umem_remap(
        dhp: sys::devmap_cookie_t,
        dip: sys::dev_info_t,
        cookie: sys::ddi_umem_cookie_t,
        off: sys::offset_t,
        len: sys::size_t,
        maxprot: sys::uint_t,
        flags: sys::uint_t,
        accattrp: *const sys::ddi_device_acc_attr_t,
    ) -> c_int;
}

/// A dumb buffer: system memory (backing 0 of the core).
pub struct NullBo;

impl GemObject for NullBo {}

/// A movable object: two backings (0 and 1), the stand-in for an object
/// that a memory manager moves between system memory and VRAM.
pub struct Movable {
    dev: DevInfo,
    cur: AtomicUsize,
}

impl Movable {
    fn remap(gem: &Gem<Self>, m: &Mapping<'_>) -> Result<(), Errno> {
        let acc = sys::ddi_device_acc_attr_t {
            devacc_attr_version: sys::DDI_DEVICE_ATTR_V0,
            devacc_attr_endian_flags: sys::DDI_NEVERSWAP_ACC,
            devacc_attr_dataorder: sys::DDI_STRICTORDER_ACC,
            devacc_attr_access: 0,
        };
        let cookie = gem.umem(gem.cur.load(Ordering::Acquire) as u32);
        // SAFETY: a live mapping piece of this object (the caller holds the
        // mapping list lock); the cookie belongs to the object, which the
        // mapping keeps alive; the range is the piece's own range within the
        // object; PROT_ALL as at setup; flags must be 0.
        let r = unsafe {
            devmap_umem_remap(
                m.cookie(),
                gem.dev.as_raw(),
                cookie,
                m.object_offset() as sys::offset_t,
                m.len(),
                0xf,
                0,
                &acc,
            )
        };
        if r == sys::DDI_SUCCESS { Ok(()) } else { Err(Errno::EIO) }
    }
}

impl GemObject for Movable {
    const MOVABLE: bool = true;
    const CUSTOM_UMEM: bool = true;

    fn umem(gem: &Gem<Self>) -> fsys::ddi_umem_cookie_t {
        gem.umem(gem.cur.load(Ordering::Acquire) as u32)
    }

    fn mapped(gem: &Gem<Self>, m: &Mapping<'_>) -> Result<(), Errno> {
        // The piece was set up (or copied, on fork and split) from the
        // backing current at the time; follow a move since then.
        Movable::remap(gem, m)
    }

    fn access(_gem: &Gem<Self>, _m: &Mapping<'_>, f: &Fault<'_>) -> c_int {
        // The refault after a move: load the translation of the piece's
        // current backing explicitly, as a movable-object layer would after
        // waiting for the move to finish.
        f.load()
    }
}

// --- the test timeline ------------------------------------------------------------

struct Due {
    fence: Fence,
    at: i64,
}

/// The test timeline: fences due at a time, signalled from a soft
/// interrupt.
pub struct Timeline {
    due: Mutex<KVec<Due>>,
    signaled: AtomicU64,
}

impl SoftInterruptHandler for Timeline {
    fn handle(&self, _ctx: &Atomic) -> Claimed {
        let now = HrTime::now().as_nanos();
        let mut any = false;
        loop {
            let d = {
                let mut l = self.due.lock();
                let i = l.iter().position(|d| d.at <= now);
                i.and_then(|i| l.swap_remove(i))
            };
            let Some(d) = d else { break };
            // Interrupt context: the facade signals and drops fences here.
            d.fence.signal();
            drop(d.fence);
            self.signaled.fetch_add(1, Ordering::Relaxed);
            any = true;
        }
        if any { Claimed::Yes } else { Claimed::No }
    }
}

/// The soft interrupt of each instance, for the fence timers.
static SOFTS: [AtomicPtr<SoftInterrupt<Timeline>>; 64] = [const { AtomicPtr::new(ptr::null_mut()) }; 64];

type FenceTimer = Timeout<Box<dyn FnOnce(&Atomic) + Send>>;

fn fence_timer(index: usize) {
    let Some(s) = SOFTS.get(index) else { return };
    let p = s.load(Ordering::Acquire);
    if !p.is_null() {
        // SAFETY: set in attach from the driver's boxed soft interrupt and
        // cleared in before_teardown after every timer is cancelled.
        let _ = unsafe { &*p }.trigger();
    }
}

fn dev_note_cancelled(index: usize, n: u32) {
    ddi::knote!("rdrmnull{index}: detach cancelled {n} pending test fences (ECANCELED)");
}

// --- the driver -------------------------------------------------------------------

/// The reference of `PUT_LATER`, until its timer drops it.
static LATER: AtomicPtr<fsys::drm_gem_object> = AtomicPtr::new(ptr::null_mut());

type LaterTimer = Timeout<fn(&Atomic)>;

fn put_later(_a: &Atomic) {
    if let Some(p) = NonNull::new(LATER.swap(ptr::null_mut(), Ordering::AcqRel)) {
        // SAFETY: stored by the ioctl from GemAny::into_raw, taken once.
        // A timeout(9F) handler must not release the object itself: the
        // last reference is parked for the core's task queue.
        unsafe { GemAny::from_raw(p) }.put_deferred();
    }
}

/// The DMA drill of `rdrmnull.conf`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum DmaDrill {
    Off,
    OwnNode,
}

impl DmaDrill {
    fn from_conf(dev: &DevInfo, ctx: &Sleepable) -> Result<DmaDrill, DdiError> {
        let mode = match dev.props().string(c"dma-mode", ctx) {
            Ok(None) => return Ok(DmaDrill::Off),
            Ok(Some(s)) => s,
            Err(_) => {
                dev.warn(format_args!("dma-mode must be a string property"));
                return Err(DdiError);
            }
        };
        let drill = match mode.to_bytes() {
            b"none" => DmaDrill::Off,
            b"self" => DmaDrill::OwnNode,
            b"root-node" => {
                dev.warn(format_args!(
                    "dma-mode \"root-node\" is gone: the facade binds GEM memory only against the driver's own node"
                ));
                return Err(DdiError);
            }
            other => {
                dev.warn(format_args!("unknown dma-mode {:?}: none or self", core::str::from_utf8(other)));
                return Err(DdiError);
            }
        };
        if drill != DmaDrill::Off {
            dev.note(format_args!("DMA drill: dma-mode {:?}", mode.to_str().unwrap_or("?")));
        }
        Ok(drill)
    }
}

const DRILL_DMA_ATTR: DmaAttr =
    DmaAttr::bits64().max_cookies((NullDriver::OBJECT_LIMIT as u64 / drm_facade::offsets::PAGE_SIZE) as u32);

fn drill_dma_attr(dev: &DevInfo, ctx: &Sleepable) -> Result<DmaAttr, DdiError> {
    match dev.props().int(c"dma-max-transfer", ctx) {
        Ok(None) => Ok(DRILL_DMA_ATTR),
        Ok(Some(n)) if n > 0 => {
            dev.note(format_args!("DMA drill: dma-max-transfer {n} bytes"));
            Ok(DRILL_DMA_ATTR.max_transfer(n as u64))
        }
        Ok(Some(n)) => {
            dev.warn(format_args!("dma-max-transfer must be positive, not {n}"));
            Err(DdiError)
        }
        Err(e) => {
            dev.warn(format_args!("dma-max-transfer must be an integer property: {e}"));
            Err(DdiError)
        }
    }
}

/// The driver's state of one device.
pub struct NullDriver {
    kms: kms::NullKms,
    timer: Mutex<Option<LaterTimer>>,
    held: Mutex<Option<GemAny>>,
    soft: KBox<SoftInterrupt<Timeline>>,
    context: AtomicU64,
    seqno: AtomicU64,
    fence_timers: Mutex<KVec<FenceTimer>>,
    index: usize,
}

impl NullDriver {
    /// The largest object (16 MiB, a 2048 by 2048 dumb buffer of 32-bit
    /// pixels).
    const OBJECT_LIMIT: usize = 16 << 20;
}

fn ctx() -> Sleepable {
    // SAFETY: the facade calls the driver's ioctl handlers and hooks in
    // thread context.
    unsafe { Sleepable::assume() }
}

fn create_movable(dev: &Dev<NullDriver>, a: &mut IoctlData<'_>, file: &File) -> Result<(), Errno> {
    let mut v: CreateMovable = a.read()?;
    if v.pad != 0 {
        return Err(Errno::EINVAL);
    }
    let size = usize::try_from(v.size).map_err(|_| Errno::EINVAL)?;
    if size == 0 || size > NullDriver::OBJECT_LIMIT / 2 {
        return Err(if size == 0 { Errno::EINVAL } else { Errno::ENOMEM });
    }
    let size = size.next_multiple_of(drm_facade::offsets::PAGE_SIZE as usize);
    let m = Movable { dev: dev.dev_info(), cur: AtomicUsize::new(0) };
    let r = GemRef::new(dev, size, m, true)?;
    r.add_backing()?;
    v.handle = r.handle_create(file)?;
    a.write(&v)
}

fn move_object(_dev: &Dev<NullDriver>, a: &mut IoctlData<'_>, file: &File) -> Result<(), Errno> {
    let mut v: Move = a.read()?;
    let flags = v.flags;
    if flags & !MOVE_FLAGS != 0 || (flags & MOVE_UNLOAD_ONLY != 0 && flags & MOVE_FILL != 0) {
        return Err(Errno::EINVAL);
    }
    // EINVAL for an object that is not movable.
    let r =
        GemRef::<Movable>::lookup(file, v.handle).map_err(|e| if e == Errno::ENOENT { e } else { Errno::EINVAL })?;
    let mut next = r.cur.load(Ordering::Acquire) & 1;
    if flags & MOVE_UNLOAD_ONLY == 0 {
        next ^= 1;
        if flags & MOVE_FILL != 0 {
            let p = r.vaddr(next as u32);
            // SAFETY: the backing not in use: every mapping piece points
            // at the other one, and none is pointed at this one before the
            // switch below.
            unsafe { ptr::write_bytes(p, flags as u8, r.size()) };
        }
        // From here new pieces are set up on the new backing (umem), and
        // the existing ones are moved below.
        r.cur.store(next, Ordering::Release);
    }
    let mut res = Ok(0u32);
    r.with_mappings(|m| {
        let Ok(pieces) = res else { return };
        // A piece that a partial munmap(2) split off may still be finished
        // by the framework and must not be remapped yet; invalidating it is
        // enough, because its next fault settles it on the current backing.
        if flags & MOVE_UNLOAD_ONLY == 0
            && m.is_settled()
            && let Err(e) = Movable::remap(&r, m)
        {
            res = Err(e);
            return;
        }
        res = match m.unload() {
            Ok(()) => Ok(pieces + 1),
            Err(_) => Err(Errno::EIO),
        };
    });
    v.pieces = res?;
    v.backing = next as u32;
    a.write(&v)
}

fn put_later_ioctl(dev: &Dev<NullDriver>, a: &mut IoctlData<'_>, file: &File) -> Result<(), Errno> {
    let v: PutLater = a.read()?;
    let ms = v.delay_ms;
    if !(1..=10_000).contains(&ms) {
        return Err(Errno::EINVAL);
    }
    let ctx = ctx();
    let mut timer = dev.driver().timer.lock();
    if !LATER.load(Ordering::Acquire).is_null() {
        return Err(Errno::EBUSY);
    }
    let r = GemAny::lookup(file, v.handle)?;
    LATER.store(r.into_raw().as_ptr(), Ordering::Release);
    let old = timer.replace(Timeout::new(Ticks::from_micros(u64::from(ms) * 1000), put_later as fn(&Atomic), &ctx));
    drop(timer);
    // The previous timer has fired (LATER was empty); dropping it only
    // frees it.
    drop(old);
    Ok(())
}

fn hold(dev: &Dev<NullDriver>, a: &mut IoctlData<'_>, file: &File) -> Result<(), Errno> {
    let v: Hold = a.read()?;
    match v.hold {
        1 => {
            let mut held = dev.driver().held.lock();
            if held.is_some() {
                return Err(Errno::EBUSY);
            }
            *held = Some(GemAny::lookup(file, v.handle)?);
            Ok(())
        }
        0 => {
            let r = dev.driver().held.lock().take().ok_or(Errno::ENOENT)?;
            drop(r);
            Ok(())
        }
        _ => Err(Errno::EINVAL),
    }
}

fn scanout(dev: &Dev<NullDriver>, a: &mut IoctlData<'_>, _file: &File) -> Result<(), Errno> {
    a.write(&dev.driver().kms.scanout())
}

fn fence(dev: &Dev<NullDriver>, a: &mut IoctlData<'_>, file: &File) -> Result<(), Errno> {
    let mut v: NullFence = a.read()?;
    if v.flags & !FENCE_FLAGS != 0
        || v.delay_ms > 10_000
        || v.error < 0
        || (v.flags & FENCE_BO_WRITE != 0 && v.flags & FENCE_BO_READ != 0)
    {
        return Err(Errno::EINVAL);
    }
    let err = if v.error != 0 { Some(Errno::new(v.error).ok_or(Errno::EINVAL)?) } else { None };
    let d = dev.driver();
    let ctx = ctx();
    if d.context.load(Ordering::Acquire) == 0 {
        let _ = d.context.compare_exchange(0, Fence::alloc_contexts(dev, 1), Ordering::AcqRel, Ordering::Acquire);
    }
    // Everything that can fail on the caller's input is checked before the
    // fence exists.
    let so = if v.flags & FENCE_SYNCOBJ != 0 { Some(Syncobj::find(file, v.syncobj)?) } else { None };
    let bo = if v.flags & (FENCE_BO_WRITE | FENCE_BO_READ) != 0 { Some(GemAny::lookup(file, v.bo)?) } else { None };
    let seqno = d.seqno.fetch_add(1, Ordering::AcqRel) + 1;
    let f = Fence::new(dev, d.context.load(Ordering::Acquire), seqno)?;
    if let Some(e) = err {
        f.set_error(e);
    }
    let r = fence_attach(&f, so, bo, &mut v);
    // Arm the fence whatever happened, so that it signals.
    let at = HrTime::now().as_nanos().saturating_add(i64::from(v.delay_ms) * 1_000_000);
    let pushed = d.soft.state().due.lock().push(Due { fence: f.clone(), at }, &ctx);
    if pushed.is_err() {
        f.signal();
    }
    drop(f);
    // timeout(9F) counts from the current tick, so it can fire up to one
    // tick before `delay_ms` has passed on the monotonic clock; one tick
    // more makes sure the fence is due when the soft interrupt runs.
    let tick_us = Ticks::from_micros(1).as_micros();
    let after = Ticks::from_micros(u64::from(v.delay_ms) * 1000 + tick_us);
    let index = d.index;
    let cb: Box<dyn FnOnce(&Atomic) + Send> = Box::new(move |_: &Atomic| fence_timer(index));
    let t = Timeout::new(after, cb, &ctx);
    let old = {
        let mut ts = d.fence_timers.lock();
        // Timers that have fired are only freed here.
        let mut done = KVec::new();
        while let Some(i) = ts.iter().position(Timeout::has_fired) {
            if let Some(t) = ts.swap_remove(i) {
                let _ = done.push(t, &ctx);
            }
        }
        ts.push(t, &ctx)?;
        done
    };
    drop(old);
    r?;
    v.seqno = seqno;
    a.write(&v)
}

/// One step of the fence ioctl: the sync object, the reservation, the sync
/// file.
fn fence_attach(f: &Fence, so: Option<Syncobj>, bo: Option<GemAny>, v: &mut NullFence) -> Result<(), Errno> {
    let mut r = Ok(());
    if let Some(s) = so {
        s.add_point(f, v.point);
    }
    if let Some(b) = bo {
        let usage = if v.flags & FENCE_BO_WRITE != 0 { Usage::Write } else { Usage::Read };
        r = b.add_fence(f, usage);
    }
    if r.is_ok() && v.flags & FENCE_SYNC_FILE != 0 {
        match sync_file_export(f, true) {
            Ok(fd) => v.fd = fd,
            Err(e) => r = Err(e),
        }
    }
    r
}

fn bo_wait(_dev: &Dev<NullDriver>, a: &mut IoctlData<'_>, file: &File) -> Result<(), Errno> {
    let v: BoWait = a.read()?;
    if v.flags & !BO_WAIT_WRITERS != 0 {
        return Err(Errno::EINVAL);
    }
    let b = GemAny::lookup(file, v.handle)?;
    let usage = if v.flags & BO_WAIT_WRITERS != 0 { Usage::Write } else { Usage::Read };
    b.wait(usage, true, v.timeout_ns)
}

const TEST_ONLY: u32 = 0;
const TEST_ONLY_RENDER: u32 = fsys::DRM_RENDER_ALLOW;

impl drm_facade::Driver for NullDriver {
    const NAME: &'static CStr = c"rdrmnull";
    const DATE: &'static CStr = c"20260928";
    const DESC: &'static CStr = c"Rust DRM null driver (illumos-drm R2)";
    const MAJOR: i32 = 1;
    const MINOR: i32 = 0;
    const PATCHLEVEL: i32 = 0;
    const FEATURES: u32 = fsys::DRIVER_MODESET
        | fsys::DRIVER_ATOMIC
        | fsys::DRIVER_RENDER
        | fsys::DRIVER_GEM
        | fsys::DRIVER_SYNCOBJ
        | fsys::DRIVER_SYNCOBJ_TIMELINE;
    const DUMB: bool = true;
    const KMS: bool = true;
    // Small limits, so that the tests reach them without exhausting the
    // test machine: 16 MiB per object, 64 MiB for the device.
    const MAX_OBJECT_SIZE: u64 = NullDriver::OBJECT_LIMIT as u64;
    const MAX_MEMORY: u64 = 64 << 20;
    const IOCTLS: &'static [Ioctl<Self>] = &[
        Ioctl { cmd: RDRMNULL_IOCTL_CREATE_MOVABLE, flags: TEST_ONLY, func: create_movable },
        Ioctl { cmd: RDRMNULL_IOCTL_MOVE, flags: TEST_ONLY, func: move_object },
        Ioctl { cmd: RDRMNULL_IOCTL_PUT_LATER, flags: TEST_ONLY, func: put_later_ioctl },
        Ioctl { cmd: RDRMNULL_IOCTL_SCANOUT, flags: TEST_ONLY, func: scanout },
        Ioctl { cmd: RDRMNULL_IOCTL_HOLD, flags: TEST_ONLY, func: hold },
        Ioctl { cmd: RDRMNULL_IOCTL_FENCE, flags: TEST_ONLY_RENDER, func: fence },
        Ioctl { cmd: RDRMNULL_IOCTL_BO_WAIT, flags: TEST_ONLY_RENDER, func: bo_wait },
    ];

    fn dumb_create(&self, dev: &Dev<Self>, file: &File, req: &DumbRequest) -> Result<DumbCreated, Errno> {
        let layout = dumb::standard_layout(req)?;
        let size = usize::try_from(layout.size).map_err(|_| Errno::EINVAL)?;
        let r = GemRef::new(dev, size, NullBo, true)?;
        let handle = r.handle_create(file)?;
        Ok(DumbCreated { handle, pitch: layout.pitch, size: r.size() as u64 })
    }

    fn kms(&self) -> Option<&dyn drm_facade::kms::KmsDriver> {
        Some(&self.kms)
    }

    fn before_detach(&self, _dev: &Dev<Self>) {
        // Only references and work the driver holds for itself go here; the
        // display and the test timeline keep running, because the detach
        // may still be refused. The reference of RDRMNULL_IOCTL_HOLD stays,
        // so that it is.
        //
        // Cancel the fence timers and signal the fences that are left: every
        // fence must signal, and pending ones would keep the fence domain
        // busy.
        let ts = core::mem::take(&mut *self.fence_timers.lock());
        drop(ts);
        let mut cancelled = 0u32;
        loop {
            let d = self.soft.state().due.lock().swap_remove(0);
            let Some(d) = d else { break };
            d.fence.signal_with_error(Errno::ECANCELED);
            drop(d.fence);
            cancelled += 1;
        }
        if cancelled != 0 {
            // The kernel test looks for this line.
            dev_note_cancelled(self.index, cancelled);
        }
        // Cancel or finish the timer, then drop a reference it never got to.
        let t = self.timer.lock().take();
        drop(t);
        if let Some(p) = NonNull::new(LATER.swap(ptr::null_mut(), Ordering::AcqRel)) {
            // SAFETY: stored from GemAny::into_raw, taken once.
            drop(unsafe { GemAny::from_raw(p) });
        }
    }

    fn before_teardown(&self, _dev: &Dev<Self>) {
        self.kms.shutdown();
        // The detach completes: no timer may reach the soft interrupt any
        // more, which goes with the driver.
        if let Some(s) = SOFTS.get(self.index) {
            s.store(ptr::null_mut(), Ordering::Release);
        }
    }
}

impl Driver for RDrmNull {
    type Instance = Device<NullDriver>;
    const LINKINFO: &'static CStr = c"Rust DRM null driver r5a";

    /// Refuses to load on a `misc/drm_core` with another facade version or
    /// other structure sizes than this driver was built against.
    fn load(_ctx: &Sleepable) -> Result<(), Errno> {
        drm_facade::check_facade("rdrmnull")
    }

    fn attach(node: OwnNode, ctx: &Sleepable) -> Result<Self::Instance, DdiError> {
        let dma = DmaDrill::from_conf(&node, ctx)?;
        let dma_attr = if dma == DmaDrill::OwnNode { Some(drill_dma_attr(&node, ctx)?) } else { None };
        let kms = kms::NullKms::new(ctx).map_err(|_| DdiError)?;
        let index = usize::try_from(node.instance().0).map_err(|_| DdiError)?;
        let slot = SOFTS.get(index).ok_or(DdiError)?;
        let soft = SoftInterrupt::add(
            &node,
            SoftInterrupt::<Timeline>::DEFAULT_PRIORITY,
            |pri| Timeline { due: Mutex::with_priority(KVec::new(), pri), signaled: AtomicU64::new(0) },
            ctx,
        )
        .map_err(|e| {
            node.warn(format_args!("cannot add the timeline soft interrupt: {e:?}"));
            DdiError
        })?;
        let soft = KBox::new(soft, ctx);
        let p = &*soft as *const SoftInterrupt<Timeline> as *mut SoftInterrupt<Timeline>;
        let pri = soft.priority();
        let driver = NullDriver {
            kms,
            timer: Mutex::new(None),
            held: Mutex::new(None),
            soft,
            context: AtomicU64::new(0),
            seqno: AtomicU64::new(0),
            fence_timers: Mutex::new(KVec::new()),
            index,
        };
        slot.store(p, Ordering::Release);
        let settings = Settings {
            dma: dma_attr,
            fence_priority: Some(pri),
            mode_config: Some(fsys::drm_illumos_mode_config {
                cursor_width: 0,
                cursor_height: 0,
                preferred_depth: 24,
                prefer_shadow: true,
                fb_modifiers: true,
                // R3 has no asynchronous page flips.
                async_page_flip: false,
            }),
            unique: None,
        };
        let dev = match Device::register(&node, driver, &settings, ctx) {
            Ok(dev) => dev,
            Err(e) => {
                slot.store(ptr::null_mut(), Ordering::Release);
                return Err(e);
            }
        };
        dev.driver().kms.set_vblank(dev.handle());
        Ok(dev)
    }

    fn detach(inst: Self::Instance, _dev: DevInfo, ctx: &Sleepable) -> Result<(), (Self::Instance, DdiError)> {
        inst.unregister(ctx)
    }

    fn instance_of_minor(minor: Minor) -> Option<Instance> {
        drm_facade::instance_of_minor(minor)
    }
}
