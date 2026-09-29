// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! A DRM driver without hardware.
//!
//! `rdrmnull` registers one DRM device per instance of its pseudo node
//! (`rdrmnull.conf` asks for instance 0), with
//! the primary node `card0` and the render node `renderD128`. It declares
//! mode setting and atomic support so that every client capability and
//! every mode-setting capability path of the core can be exercised; it has
//! no CRTCs, planes or connectors, and the mode-setting ioctls that would
//! list them arrive with milestone R3.
//!
//! Since milestone R1 it has GEM objects and dumb buffers in system memory.
//! As a pseudo device it does no DMA (the pseudo nexus refuses DMA
//! handles), so its objects have no scatter/gather table
//! ([`drm::Driver::DMA_ATTR`] is `None`).
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
//! * `"root-node"`: stay a pseudo node but allocate the DMA handles against
//!   `ddi_root_node(9F)` and bind every object
//!   (`tools/test/rdrmnull-dma-root-node.conf`). The DDI does not allow
//!   this; it reproduces the kernel panic an R1 trial caused, so that a
//!   crash dump can be taken. Use a disposable VM with a dump device.
//!   illumos-ddi allocates DMA handles only from the driver's own node (an
//!   `OwnNode`) and refuses the root node, so this mode goes around it with
//!   raw `ddi-sys` calls (`drm::gem::GemDma::against`). It is only compiled
//!   with the `panic-drill` cargo feature, which is off by default; a module
//!   built without it refuses `root-node` at attach and cannot name another
//!   node at all.
//!
//! It also has three driver-private ioctls that exist only for the kernel
//! tests (`tools/test/drm_r1_test.c`) and are not an interface for anything
//! else:
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
//!
//! It is the kernel module the core is tested with, and the template for a
//! real driver: implement [`drm::Driver`], use [`drm::Device`] as the
//! instance, and map minor numbers with [`drm::minor::instance_of_minor`].
#![no_std]

use core::ffi::{CStr, c_int, c_void};
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicPtr, AtomicUsize, Ordering};

use ddi::DmaAttr;
use ddi::prelude::*;
use ddi::sys;
use drm::gem::{Fault, GemCore, GemDma, GemObject, GemRef, Mapping, SysMem, UmemCookie};
use drm::ioctl::DriverIoctl;
use drm::minor::CloneId;
use drm::state::Perm;
use drm::uaccess::{Plain, UserArg};
use drm::{Features, ModeConfig};
use drm_uapi::ioc;

ddi::module! {
    driver: RDrmNull,
    ops: [char, devmap],
}

/// The kernel module.
pub struct RDrmNull;

// --- the test-only ioctls ---------------------------------------------------

/// `RDRMNULL_IOCTL_CREATE_MOVABLE`: `size` in, `handle` out.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CreateMovable {
    /// The object size in bytes, rounded up to whole pages.
    pub size: u64,
    /// The new handle.
    pub handle: u32,
    /// Must be zero.
    pub pad: u32,
}

/// `RDRMNULL_IOCTL_MOVE`: `handle` and `flags` in, the rest out.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Move {
    /// A handle of a movable object.
    pub handle: u32,
    /// [`MOVE_FILL`] with a fill byte in the low eight bits, and/or
    /// [`MOVE_UNLOAD_ONLY`].
    pub flags: u32,
    /// The number of mapping pieces invalidated.
    pub pieces: u32,
    /// The backing now in use, 0 or 1.
    pub backing: u32,
}

/// `RDRMNULL_IOCTL_PUT_LATER`: `handle` in.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PutLater {
    /// A handle whose object gets an extra reference, dropped from a timer.
    pub handle: u32,
    /// Delay in milliseconds, 1 to 10000.
    pub delay_ms: u32,
}

// SAFETY: integer fields only, no padding.
unsafe impl Plain for CreateMovable {}
// SAFETY: as above.
unsafe impl Plain for Move {}
// SAFETY: as above.
unsafe impl Plain for PutLater {}

const _: () = assert!(size_of::<CreateMovable>() == 16 && size_of::<Move>() == 16 && size_of::<PutLater>() == 8);

/// Fill the new backing with the byte in bits 0 to 7 before switching.
pub const MOVE_FILL: u32 = 1 << 8;
/// Only invalidate the mappings; keep the backing.
pub const MOVE_UNLOAD_ONLY: u32 = 1 << 9;
const MOVE_FLAGS: u32 = MOVE_FILL | MOVE_UNLOAD_ONLY | 0xff;

/// `DRM_IOWR(DRM_COMMAND_BASE + 0, struct rdrmnull_create_movable)`.
pub const RDRMNULL_IOCTL_CREATE_MOVABLE: u32 = ioc::drm_iowr::<CreateMovable>(0x40);
/// `DRM_IOWR(DRM_COMMAND_BASE + 1, struct rdrmnull_move)`.
pub const RDRMNULL_IOCTL_MOVE: u32 = ioc::drm_iowr::<Move>(0x41);
/// `DRM_IOW(DRM_COMMAND_BASE + 2, struct rdrmnull_put_later)`.
pub const RDRMNULL_IOCTL_PUT_LATER: u32 = ioc::drm_iow::<PutLater>(0x42);

/// Primary node, no authentication: the tests run as root on `card0`.
const TEST_ONLY: Perm = Perm { auth: false, master: false, root_only: false, render_allow: false };

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

/// A buffer object of `rdrmnull`.
pub enum NullObject {
    /// A dumb buffer: plain system memory.
    Plain(SysMem),
    /// A test object with two backings (see the crate documentation).
    Movable(Movable),
}

/// Two backings of the same size and the index of the one in use.
pub struct Movable {
    dev: DevInfo,
    mem: [SysMem; 2],
    /// Changed only with the object's mapping list locked.
    cur: AtomicUsize,
}

impl Movable {
    fn backing(&self, i: usize) -> &SysMem {
        let [a, b] = &self.mem;
        if i & 1 == 0 { a } else { b }
    }

    fn current(&self) -> &SysMem {
        self.backing(self.cur.load(Ordering::Acquire))
    }

    /// Points a mapping piece at the current backing.
    fn remap(&self, m: &Mapping) -> Result<(), Errno> {
        let acc = sys::ddi_device_acc_attr_t {
            devacc_attr_version: sys::DDI_DEVICE_ATTR_V0,
            devacc_attr_endian_flags: sys::DDI_NEVERSWAP_ACC,
            devacc_attr_dataorder: sys::DDI_STRICTORDER_ACC,
            devacc_attr_access: 0,
        };
        // SAFETY: a live mapping piece of this object (the caller holds the
        // mapping list lock); the cookie belongs to the object, which the
        // mapping keeps alive; the range is the piece's own range within the
        // object; PROT_ALL as at setup; flags must be 0.
        let r = unsafe {
            devmap_umem_remap(
                m.raw(),
                self.dev.as_raw(),
                self.current().umem_cookie().as_raw(),
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

impl From<SysMem> for NullObject {
    fn from(m: SysMem) -> NullObject {
        NullObject::Plain(m)
    }
}

impl GemObject for NullObject {
    fn movable(&self) -> bool {
        matches!(self, NullObject::Movable(_))
    }
    fn size(&self) -> usize {
        match self {
            NullObject::Plain(m) => m.size(),
            NullObject::Movable(v) => v.mem[0].size(),
        }
    }
    fn memory_bytes(&self) -> usize {
        match self {
            NullObject::Plain(m) => m.size(),
            NullObject::Movable(v) => v.mem[0].size() + v.mem[1].size(),
        }
    }
    fn dma_segments(&self) -> usize {
        match self {
            NullObject::Plain(m) => m.sg_len(),
            NullObject::Movable(v) => v.mem[0].sg_len() + v.mem[1].sg_len(),
        }
    }
    fn umem(&self) -> UmemCookie {
        match self {
            NullObject::Plain(m) => m.umem_cookie(),
            NullObject::Movable(v) => v.current().umem_cookie(),
        }
    }
    fn mapped(&self, m: &Mapping, _ctx: &Sleepable) -> Result<(), Errno> {
        match self {
            NullObject::Plain(_) => Ok(()),
            // The piece was set up (or copied, on fork and split) from the
            // backing current at the time; follow a move since then.
            NullObject::Movable(v) => v.remap(m),
        }
    }
    fn access(&self, _m: &Mapping, f: &Fault) -> c_int {
        match self {
            NullObject::Plain(_) => f.default_access(),
            // The refault after a move: load the translation of the piece's
            // current backing explicitly, as a movable-object layer would
            // after waiting for the move to finish.
            NullObject::Movable(_) => f.load(),
        }
    }
}

// --- the driver -------------------------------------------------------------------

/// The reference `RDRMNULL_IOCTL_PUT_LATER` hands to its timer.
static LATER: AtomicPtr<c_void> = AtomicPtr::new(ptr::null_mut());

/// The timer of `RDRMNULL_IOCTL_PUT_LATER`. A plain function pointer, so
/// that the type can be named; the reference travels through [`LATER`].
type LaterTimer = Timeout<fn(&Atomic)>;

/// The timer callback: drops the parked reference in interrupt context.
fn put_later(a: &Atomic) {
    if let Some(p) = NonNull::new(LATER.swap(ptr::null_mut(), Ordering::AcqRel)) {
        // SAFETY: stored by the ioctl from GemRef::into_raw, taken once.
        let r = unsafe { GemRef::<NullObject>::from_raw(p) };
        r.put_atomic(a);
    }
}

/// The DMA drill (`dma-mode` in `rdrmnull.conf`; see the crate
/// documentation).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum DmaDrill {
    /// `"none"`: no DMA.
    Off,
    /// `"self"`: bind against the driver's own node.
    OwnNode,
    /// `"root-node"`: bind against `ddi_root_node(9F)` (outside the DDI).
    /// Only with the `panic-drill` feature.
    #[cfg(feature = "panic-drill")]
    RootNode,
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
            #[cfg(feature = "panic-drill")]
            b"root-node" => DmaDrill::RootNode,
            #[cfg(not(feature = "panic-drill"))]
            b"root-node" => {
                dev.warn(format_args!(
                    "dma-mode \"root-node\" panics the system on purpose and needs rdrmnull built with the \
                     panic-drill feature; this build refuses it"
                ));
                return Err(DdiError);
            }
            other => {
                dev.warn(format_args!("unknown dma-mode {:?}: none, self or root-node", core::str::from_utf8(other)));
                return Err(DdiError);
            }
        };
        if drill != DmaDrill::Off {
            dev.note(format_args!("DMA drill: dma-mode {:?}", mode.to_str().unwrap_or("?")));
        }
        Ok(drill)
    }
}

/// The DMA attributes of the drill: any 64-bit address, page-aligned, and
/// one cookie per page of the largest object (16 MiB).
const DRILL_DMA_ATTR: DmaAttr =
    DmaAttr::bits64().max_cookies((NullDriver::OBJECT_LIMIT as u64 / drm::offsets::PAGE_SIZE) as u32);

/// The DMA attributes of the drill from `rdrmnull.conf`: [`DRILL_DMA_ATTR`],
/// with the largest transfer lowered to `dma-max-transfer` bytes if that
/// integer property is set (a positive number). The property only applies
/// to `dma-mode="self"`, the mode that binds with these attributes against
/// the driver's own node; in every other mode it is not read.
fn drill_dma_attr(dev: &DevInfo, drill: DmaDrill, ctx: &Sleepable) -> Result<DmaAttr, DdiError> {
    if drill != DmaDrill::OwnNode {
        return Ok(DRILL_DMA_ATTR);
    }
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

/// The DRM driver: no hardware, only the test timer and the DMA drill.
pub struct NullDriver {
    timer: Mutex<Option<LaterTimer>>,
    dma: DmaDrill,
    /// The DMA attributes of the drill, from [`drill_dma_attr`].
    dma_attr: DmaAttr,
}

impl NullDriver {
    /// [`drm::Driver::MAX_OBJECT_SIZE`], named here for [`DRILL_DMA_ATTR`].
    const OBJECT_LIMIT: usize = 16 << 20;
}

impl NullDriver {
    fn create_movable(
        &self,
        dev: &drm::Device<NullDriver>,
        file: CloneId,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let mut a = UserArg::<CreateMovable>::read(arg)?;
        let g = dev.gem().ok_or(Errno::ENXIO)?;
        if a.get().pad != 0 {
            return Err(Errno::EINVAL);
        }
        let size = usize::try_from(a.get().size).map_err(|_| Errno::EINVAL)?;
        if size == 0 || size > <NullDriver as drm::Driver>::MAX_OBJECT_SIZE / 2 {
            return Err(if size == 0 { Errno::EINVAL } else { Errno::ENOMEM });
        }
        let reservation = g.reserve(size.next_multiple_of(drm::offsets::PAGE_SIZE as usize) * 2, ctx)?;
        let m0 = g.sys_mem(size, ctx)?;
        let m1 = g.sys_mem(size, ctx)?;
        let obj = NullObject::Movable(Movable { dev: g.dev_info(), mem: [m0, m1], cur: AtomicUsize::new(0) });
        let r = GemCore::create_reserved(g, obj, reservation, ctx)?;
        let handle = g.handle_create(file, r, ctx)?;
        // SAFETY: a field assignment.
        unsafe { a.get_mut() }.handle = handle;
        a.write(arg)
    }

    fn move_object(
        &self,
        dev: &drm::Device<NullDriver>,
        file: CloneId,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let mut a = UserArg::<Move>::read(arg)?;
        let g = dev.gem().ok_or(Errno::ENXIO)?;
        let flags = a.get().flags;
        if flags & !MOVE_FLAGS != 0 || (flags & MOVE_UNLOAD_ONLY != 0 && flags & MOVE_FILL != 0) {
            return Err(Errno::EINVAL);
        }
        let r = g.lookup(file, a.get().handle)?;
        let res = r.with_mappings(|obj, maps| {
            let NullObject::Movable(v) = obj else { return Err(Errno::EINVAL) };
            let mut next = v.cur.load(Ordering::Acquire) & 1;
            if flags & MOVE_UNLOAD_ONLY == 0 {
                next ^= 1;
                if flags & MOVE_FILL != 0 {
                    let m = v.backing(next);
                    // SAFETY: the backing not in use: every mapping piece
                    // points at the other one (the list is locked), so no
                    // process can reach this memory.
                    unsafe { ptr::write_bytes(m.kernel_ptr().as_ptr(), flags as u8, m.size()) };
                }
                v.cur.store(next, Ordering::Release);
            }
            let mut pieces = 0u32;
            for m in maps {
                // A piece that a partial munmap(2) split off may still be
                // finished by the framework and must not be remapped yet
                // (Mapping::is_settled); invalidating it is enough, because
                // its next fault settles it on the current backing.
                if flags & MOVE_UNLOAD_ONLY == 0 && m.is_settled() {
                    v.remap(&m)?;
                }
                m.unload(ctx).map_err(|_| Errno::EIO)?;
                pieces += 1;
            }
            Ok((pieces, next as u32))
        });
        r.put(ctx);
        let (pieces, backing) = res?;
        // SAFETY: only field assignments below.
        let m = unsafe { a.get_mut() };
        m.pieces = pieces;
        m.backing = backing;
        a.write(arg)
    }

    fn put_later(
        &self,
        dev: &drm::Device<NullDriver>,
        file: CloneId,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let a = UserArg::<PutLater>::read(arg)?;
        let g = dev.gem().ok_or(Errno::ENXIO)?;
        let ms = a.get().delay_ms;
        if !(1..=10_000).contains(&ms) {
            return Err(Errno::EINVAL);
        }
        let mut timer = self.timer.lock();
        if !LATER.load(Ordering::Acquire).is_null() {
            return Err(Errno::EBUSY);
        }
        let r = g.lookup(file, a.get().handle)?;
        LATER.store(r.into_raw().as_ptr(), Ordering::Release);
        let old = timer.replace(Timeout::new(Ticks::from_micros(u64::from(ms) * 1000), put_later as fn(&Atomic), ctx));
        drop(timer);
        // The previous timer has fired (LATER was empty); dropping it only
        // frees it.
        drop(old);
        Ok(())
    }
}

impl drm::Driver for NullDriver {
    const NAME: &'static CStr = c"rdrmnull";
    const DATE: &'static CStr = c"20260928";
    const DESC: &'static CStr = c"Rust DRM null driver (illumos-drm R1)";
    const MAJOR: i32 = 1;
    const MINOR: i32 = 0;
    const PATCHLEVEL: i32 = 0;
    const FEATURES: Features =
        Features::MODESET.union(Features::ATOMIC).union(Features::RENDER).union(Features::GEM).union(Features::DUMB);
    const MODE_CONFIG: ModeConfig = ModeConfig { preferred_depth: 24, prefer_shadow: true, ..ModeConfig::DEFAULT };
    // Small limits, so that the tests reach them without exhausting the
    // test machine: 16 MiB per object (a 2048 by 2048 dumb buffer of 32-bit
    // pixels), 64 MiB for the device.
    const MAX_OBJECT_SIZE: usize = NullDriver::OBJECT_LIMIT;
    const MAX_MEMORY: usize = 64 << 20;
    const IOCTLS: &'static [DriverIoctl] = &[
        DriverIoctl { cmd: RDRMNULL_IOCTL_CREATE_MOVABLE, perm: TEST_ONLY },
        DriverIoctl { cmd: RDRMNULL_IOCTL_MOVE, perm: TEST_ONLY },
        DriverIoctl { cmd: RDRMNULL_IOCTL_PUT_LATER, perm: TEST_ONLY },
    ];
    type Object = NullObject;

    fn ioctl(
        &self,
        dev: &drm::Device<Self>,
        file: CloneId,
        cmd: u32,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        match cmd {
            RDRMNULL_IOCTL_CREATE_MOVABLE => self.create_movable(dev, file, arg, ctx),
            RDRMNULL_IOCTL_MOVE => self.move_object(dev, file, arg, ctx),
            RDRMNULL_IOCTL_PUT_LATER => self.put_later(dev, file, arg, ctx),
            _ => Err(Errno::ENOTTY),
        }
    }

    fn gem_dma(&self, _dev: &DevInfo) -> Option<GemDma> {
        match self.dma {
            DmaDrill::Off => None,
            DmaDrill::OwnNode => Some(GemDma::new(self.dma_attr)),
            // SAFETY: none; this is the drill. ddi_root_node(9F) returns the
            // root node, which lives as long as the system; the DDI requires
            // the driver's own node, and the DMA framework's reaction to
            // another one is what the drill records.
            #[cfg(feature = "panic-drill")]
            DmaDrill::RootNode => {
                Some(unsafe { GemDma::against(DRILL_DMA_ATTR, DevInfo::from_raw(sys::ddi_root_node())) })
            }
        }
    }

    fn before_detach(&self, ctx: &Sleepable) {
        // Cancel or finish the timer, then drop a reference it never got to.
        let t = self.timer.lock().take();
        drop(t);
        if let Some(p) = NonNull::new(LATER.swap(ptr::null_mut(), Ordering::AcqRel)) {
            // SAFETY: stored from GemRef::into_raw, taken once.
            unsafe { GemRef::<NullObject>::from_raw(p) }.put(ctx);
        }
    }
}

impl Driver for RDrmNull {
    type Instance = drm::Device<NullDriver>;
    const LINKINFO: &'static CStr = c"Rust DRM null driver r1";

    fn attach(node: OwnNode, ctx: &Sleepable) -> Result<Self::Instance, DdiError> {
        let dma = DmaDrill::from_conf(&node, ctx)?;
        let dma_attr = drill_dma_attr(&node, dma, ctx)?;
        drm::Device::attach(node, NullDriver { timer: Mutex::new(None), dma, dma_attr }, ctx)
    }

    fn detach(inst: Self::Instance, _dev: DevInfo, ctx: &Sleepable) -> Result<(), (Self::Instance, DdiError)> {
        inst.detach(ctx)
    }

    fn instance_of_minor(minor: Minor) -> Option<Instance> {
        drm::minor::instance_of_minor(minor)
    }
}
