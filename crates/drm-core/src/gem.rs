// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! GEM objects in the kernel: the object and its references, the
//! system-memory backend, the per-device tables and the `devmap(9E)`
//! callbacks that map objects into processes.
//!
//! # Objects and references
//!
//! A driver's buffer type implements [`GemObject`]. The core wraps it in a
//! reference-counted [`GemInner`], together with the core's bookkeeping
//! ([`ObjCommon`]: size, handle count, FLINK name, mmap offset) and the list
//! of the object's user mappings. A [`GemRef`] is one counted reference.
//! References are held by handles (one per handle, in the file's table), by
//! the name table (one per named object), by user mappings (one per
//! mapping piece), by `mmap(2)` calls in progress, and by whatever a driver
//! keeps for in-flight work.
//!
//! Dropping a reference follows the rule of [`crate::defer`]:
//! [`GemRef::put`] with the [`Sleepable`] token destroys the object at once
//! if it was the last reference, [`GemRef::put_atomic`] with the [`Atomic`]
//! token parks it for the device's task queue. A `GemRef` that is dropped
//! implicitly is treated as if it were dropped in interrupt context, so an
//! object is never destroyed in a context that may not block. Destroying an
//! object removes its mmap offset, frees its backing (for [`SysMem`]:
//! `ddi_dma_unbind_handle(9F)` and `ddi_umem_free(9F)`) and updates the
//! statistics.
//!
//! # Locks
//!
//! [`GemCore`] holds the device-wide state, each behind its own mutex:
//! the name table (`names`), the offset table (`offsets`), the per-file
//! handle tables (`files`, each file's table behind a mutex of its own),
//! and the `mmap(2)` calls in progress (`pending`). Each object has a mutex
//! for its list of mappings. The order is: `names`, then a file's handle
//! table, then an object's mappings; `offsets`, `files` and `pending` are
//! leaves that are never held while taking another lock. No reference is
//! dropped with a lock held, because destroying an object takes `offsets`.
//! [`crate::objects`] states the rules for handles and names and is tested
//! on the host from several threads with the same order.
//!
//! # Mappings
//!
//! `mmap(2)` on a DRM file runs `devmap_setup(9F)`, which calls the
//! device's `devmap(9E)` entry point (`GemCore::devmap`). It finds the
//! object whose offset range starts exactly at the requested offset, takes
//! a reference, and describes the object's memory with
//! `devmap_umem_setup(9F)`, registering the callbacks below. The reference
//! is parked in `pending`, keyed by the mapping handle, because the
//! framework may free the handle without any further callback if the
//! `mmap(2)` fails later (it does for `MAP_PRIVATE` requests, for one).
//! Both entry points run in the thread that called `mmap(2)`, and the core
//! validates the whole range in one `devmap(9E)` call, so an entry that a
//! thread left behind belongs to a failed call: the next GEM entry point of
//! that thread drops it, and `close(9E)` drops whatever the file's threads
//! left. `devmap_map(9E)` then takes the reference out of `pending`,
//! checks that the mapping is shared and that the file holds a handle to
//! the object (Linux's `drm_vma_node_is_allowed`), and records the mapping.
//! `devmap_dup(9E)` (fork) and `devmap_unmap(9E)` (munmap, exit, and the
//! split of a mapping that is partly unmapped) take and drop references for
//! each piece; a piece that a split leaves always gets private data with its
//! own reference, and stays unsettled until its next fault
//! ([`Mapping::is_settled`]). `devmap_access(9E)` settles such a piece and
//! then asks the object; the default loads the translations with
//! `devmap_default_access(9F)`.
//!
//! A mapping keeps its file open (specfs does not call `close(9E)` while
//! the file is mapped), and the device cannot detach while a file is open,
//! so the device outlives every mapping and every object.
//!
//! The callbacks find the device through a table indexed by device number
//! (`registry`), because `devmap_map(9E)` receives only the device
//! number. Each kernel module links one DRM driver, so each index belongs
//! to one object type; the table also records the type and refuses a
//! mismatch.

use core::ffi::{CStr, c_int, c_void};
use core::fmt;
use core::iter::FusedIterator;
use core::marker::PhantomData;
use core::mem::ManuallyDrop;
use core::num::NonZeroU32;
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

use ddi::prelude::*;
use ddi::sys;
use ddi::{DmaAttr, DmaCookie, OwnNode};

use crate::defer::{self, Ctx, DeferList, Deferrable};
use crate::dmaerr;
use crate::idmap::{IdAlloc, IdMap};
use crate::karc::{KArc, KArcLast};
use crate::kvec::KVec;
use crate::minor::{self, CloneId};
use crate::objects::{self, ObjCommon, ObjRef};
use crate::offsets::{self, OFFSET_SIZE, OFFSET_START, OffsetMap, PAGE_SIZE, RangeAlloc};
use crate::resv::Resv;

// From sys/mman.h; stable ABI values that ddi-sys does not declare.
/// `PROT_ALL`: every protection a mapping may ever be given.
const PROT_ALL: sys::uint_t = 0xf;
/// `MAP_TYPE`: the mask of the sharing type in `mmap(2)` flags.
const MAP_TYPE: sys::uint_t = 0xf;
/// `MAP_SHARED`.
const MAP_SHARED: sys::uint_t = 1;

// --- names ------------------------------------------------------------------

/// A NUL-terminated name built at run time, for `id_space_create(9F)` and
/// `vmem_create(9F)`, which copy it. A name that does not fit is an error,
/// never truncated.
pub(crate) struct NameBuf {
    buf: [u8; 32],
    len: usize,
}

impl NameBuf {
    pub(crate) fn new(args: fmt::Arguments<'_>) -> Result<NameBuf, Errno> {
        let mut n = NameBuf { buf: [0; 32], len: 0 };
        // Only a driver with an absurdly long name can overflow the buffer.
        fmt::write(&mut n, args).map_err(|_| Errno::EINVAL)?;
        Ok(n)
    }
    pub(crate) fn as_cstr(&self) -> &CStr {
        // The buffer is zeroed beyond `len` and `len` < 32.
        CStr::from_bytes_until_nul(&self.buf).unwrap_or(c"drm")
    }
}

impl fmt::Write for NameBuf {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let b = s.as_bytes();
        let end = self.len.checked_add(b.len()).ok_or(fmt::Error)?;
        // Keep one byte for the terminating NUL.
        if end >= self.buf.len() || b.contains(&0) {
            return Err(fmt::Error);
        }
        self.buf.get_mut(self.len..end).ok_or(fmt::Error)?.copy_from_slice(b);
        self.len = end;
        Ok(())
    }
}

// --- identifier spaces and the offset arena -----------------------------------

/// Numbers from `id_space(9F)`, allocated first-fit without blocking
/// (`id_allocff_nosleep`), from 1 to `INT_MAX - 1`.
pub(crate) struct KIdSpace(NonNull<sys::id_space_t>);

// SAFETY: an identifier space is a vmem arena with its own lock; every
// function may be called from any thread.
unsafe impl Send for KIdSpace {}
// SAFETY: as above.
unsafe impl Sync for KIdSpace {}

impl KIdSpace {
    fn new(name: &CStr, ctx: &Sleepable) -> Result<KIdSpace, Errno> {
        Self::with_range(name, 1, i32::MAX, ctx)
    }

    /// A space of the numbers from `low` to `high - 1`; `low` at least 1.
    pub(crate) fn with_range(name: &CStr, low: i32, high: i32, _ctx: &Sleepable) -> Result<KIdSpace, Errno> {
        if low < 1 || high <= low {
            return Err(Errno::EINVAL);
        }
        // SAFETY: may block, which the token allows; the name is copied.
        let p = unsafe { sys::id_space_create(name.as_ptr(), low, high) };
        NonNull::new(p).map(KIdSpace).ok_or(Errno::ENOMEM)
    }
}

impl IdAlloc for KIdSpace {
    fn alloc(&mut self) -> Option<u32> {
        // SAFETY: a live space; the nosleep variant may be called anywhere.
        let id = unsafe { sys::id_allocff_nosleep(self.0.as_ptr()) };
        u32::try_from(id).ok().filter(|i| *i >= 1)
    }
    fn free(&mut self, id: u32) {
        if let Ok(id) = i32::try_from(id) {
            // SAFETY: the id came from alloc on this space (IdMap's contract).
            unsafe { sys::id_free(self.0.as_ptr(), id) }
        }
    }
}

impl Drop for KIdSpace {
    fn drop(&mut self) {
        // SAFETY: created by id_space_create; every id was freed by the
        // owning IdMap. The owner drops it in thread context (close(9E),
        // detach(9E)).
        unsafe { sys::id_space_destroy(self.0.as_ptr()) }
    }
}

/// The GEM offset space as a `vmem(9F)` identifier arena (`VMC_IDENTIFIER`,
/// quantum one page). Allocation never blocks (`VM_NOSLEEP`, best fit): an
/// exhausted space is an error, not a wait.
pub(crate) struct VmemArena(NonNull<sys::vmem_t>);

// SAFETY: a vmem arena has its own lock; every function may be called from
// any thread.
unsafe impl Send for VmemArena {}
// SAFETY: as above.
unsafe impl Sync for VmemArena {}

impl VmemArena {
    fn new(name: &CStr, _ctx: &Sleepable) -> Result<VmemArena, Errno> {
        // SAFETY: may block, which the token allows; the name is copied. The
        // base is an integer, not memory: VMC_IDENTIFIER.
        let p = unsafe {
            sys::vmem_create(
                name.as_ptr(),
                OFFSET_START as usize as *mut c_void,
                OFFSET_SIZE as usize,
                PAGE_SIZE as usize,
                None,
                None,
                ptr::null_mut(),
                0,
                sys::VM_SLEEP | sys::VMC_IDENTIFIER,
            )
        };
        NonNull::new(p).map(VmemArena).ok_or(Errno::ENOMEM)
    }
}

impl RangeAlloc for VmemArena {
    fn alloc(&mut self, len: u64) -> Option<u64> {
        let len = usize::try_from(len).ok()?;
        // SAFETY: a live arena; VM_NOSLEEP may be used anywhere.
        let p = unsafe { sys::vmem_alloc(self.0.as_ptr(), len, sys::VM_NOSLEEP | sys::VM_BESTFIT) };
        (!p.is_null()).then_some(p as usize as u64)
    }
    fn free(&mut self, start: u64, len: u64) {
        // SAFETY: the range came from alloc on this arena (OffsetMap's
        // contract).
        unsafe { sys::vmem_free(self.0.as_ptr(), start as usize as *mut c_void, len as usize) }
    }
}

impl Drop for VmemArena {
    fn drop(&mut self) {
        // SAFETY: created by vmem_create; every range was freed when its
        // object was destroyed. Dropped in detach(9E), thread context.
        unsafe { sys::vmem_destroy(self.0.as_ptr()) }
    }
}

// --- the system-memory backend ------------------------------------------------

/// Kernel memory that can be mapped into user processes: the cookie of a
/// `ddi_umem_alloc(9F)` allocation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct UmemCookie(NonNull<c_void>);

impl UmemCookie {
    /// Wraps a raw cookie.
    ///
    /// # Safety
    /// `cookie` must come from `ddi_umem_alloc(9F)` and stay allocated for
    /// as long as any object that reports it exists.
    pub unsafe fn from_raw(cookie: NonNull<c_void>) -> UmemCookie {
        UmemCookie(cookie)
    }
    /// Returns the raw cookie, for `devmap_umem_setup(9F)` and friends.
    pub fn as_raw(self) -> sys::ddi_umem_cookie_t {
        self.0.as_ptr()
    }
}

/// System memory for a GEM object: page-aligned, locked (non-pageable),
/// zero-filled kernel memory from `ddi_umem_alloc(9F)` with `DDI_UMEM_SLEEP`,
/// mapped cached, and, if the driver gives DMA attributes, bound with
/// `ddi_dma_addr_bind_handle(9F)` so that its cookie list serves as the
/// scatter/gather table ([`SysMem::sg_iter`]).
///
/// `ddi_umem_alloc(9F)` zero-fills the memory (it `bzero`s every
/// non-pageable allocation), so no kernel data reaches a process that maps
/// a new object. Write-combining mappings need the private tier and are
/// not offered.
pub struct SysMem {
    cookie: NonNull<c_void>,
    kaddr: NonNull<u8>,
    size: usize,
    sg: Option<SgTable>,
}

struct SgTable {
    handle: SgHandle,
    ncookies: usize,
}

/// The DMA handle of a binding.
enum SgHandle {
    /// Allocated through illumos-ddi against the device's own node.
    Own(ddi::DmaHandle),
    /// Allocated with a raw `ddi_dma_alloc_handle(9F)` against a foreign
    /// node, for the DMA drill only (`GemDma::against`); freed with
    /// `ddi_dma_free_handle(9F)` when dropped.
    #[cfg(feature = "panic-drill")]
    Raw(sys::ddi_dma_handle_t),
}

impl SgHandle {
    fn as_raw(&self) -> sys::ddi_dma_handle_t {
        match self {
            SgHandle::Own(h) => h.as_raw(),
            #[cfg(feature = "panic-drill")]
            SgHandle::Raw(h) => *h,
        }
    }
}

#[cfg(feature = "panic-drill")]
impl Drop for SgHandle {
    fn drop(&mut self) {
        if let SgHandle::Raw(h) = self {
            // SAFETY: allocated in SysMem::bind, unbound by SysMem's Drop
            // before the table is dropped, and freed only here.
            unsafe { sys::ddi_dma_free_handle(h) }
        }
    }
}

/// The node the handles of a binding are allocated against.
#[derive(Clone, Copy)]
pub(crate) enum DmaTarget<'a> {
    /// The device's own node, the only one the DDI allows.
    Own(&'a OwnNode),
    /// Another node, for the DMA drill only (`GemDma::against`).
    #[cfg(feature = "panic-drill")]
    Foreign(DevInfo),
}

// SAFETY: kernel memory with no thread affinity; the DMA handle may be used
// and freed from any thread. The memory itself is shared with user
// processes, so the type hands out no references to it.
unsafe impl Send for SysMem {}
// SAFETY: as above; &SysMem only reads the fixed fields.
unsafe impl Sync for SysMem {}

impl SysMem {
    /// Allocates `size` bytes, rounded up to whole pages, and binds them for
    /// DMA with `dma` if given, against `node`, the device's own node that
    /// the driver received in attach (the DMA handle "must only be used by
    /// the device for which it was allocated", `ddi_dma_alloc_handle(9F)`).
    /// Fails with `EINVAL` for a zero size, `ENOMEM` if memory is short, and
    /// with the errno [`dmaerr::object_errno`]
    /// gives for a failed DMA allocation or binding: `ENOMEM` when DMA
    /// resources are short or the memory cannot be mapped for the device,
    /// `EINVAL` for attributes the framework can never satisfy (which is what
    /// a pseudo node gets), `ENXIO` for a node the own-node guard refuses.
    pub fn new(node: &OwnNode, size: usize, dma: Option<&DmaAttr>, ctx: &Sleepable) -> Result<SysMem, Errno> {
        Self::alloc(DmaTarget::Own(node), size, dma, ctx)
    }

    pub(crate) fn alloc(
        target: DmaTarget<'_>,
        size: usize,
        dma: Option<&DmaAttr>,
        ctx: &Sleepable,
    ) -> Result<SysMem, Errno> {
        let size = offsets::page_round(size as u64).and_then(|s| usize::try_from(s).ok()).ok_or(Errno::EINVAL)?;
        let mut cookie: sys::ddi_umem_cookie_t = ptr::null_mut();
        // SAFETY: DDI_UMEM_SLEEP from thread context, which the token
        // proves; the cookie pointer is writable.
        let k = unsafe { sys::ddi_umem_alloc(size, sys::DDI_UMEM_SLEEP, &mut cookie) };
        let (kaddr, cookie) = match (NonNull::new(k as *mut u8), NonNull::new(cookie)) {
            (Some(k), Some(c)) => (k, c),
            (_, c) => {
                if let Some(c) = c {
                    // SAFETY: a cookie from ddi_umem_alloc, freed once.
                    unsafe { sys::ddi_umem_free(c.as_ptr()) }
                }
                return Err(Errno::ENOMEM);
            }
        };
        let mut mem = SysMem { cookie, kaddr, size, sg: None };
        if let Some(attr) = dma {
            // On failure `mem` is dropped, which frees the memory.
            mem.sg = Some(mem.bind(target, attr, ctx)?);
            // The table must describe exactly the object: a binding that
            // covered less (or more) would be a framework bug that a device
            // would turn into memory corruption.
            let n = mem.sg_iter().len();
            let total = mem.sg_iter().try_fold(0usize, |t, c| t.checked_add(c.size));
            if n != mem.sg_len() || total != Some(size) {
                target.dev().warn(format_args!("DMA binding of {size} bytes has {n} cookies totalling {total:?}"));
                return Err(Errno::EIO);
            }
        }
        Ok(mem)
    }

    fn bind(&self, target: DmaTarget<'_>, attr: &DmaAttr, ctx: &Sleepable) -> Result<SgTable, Errno> {
        let handle = match target {
            DmaTarget::Own(node) => SgHandle::Own(ddi::DmaHandle::new(node, attr, ctx).map_err(dmaerr::object_errno)?),
            #[cfg(feature = "panic-drill")]
            DmaTarget::Foreign(dev) => {
                // The drill bypasses illumos-ddi, whose guard would refuse
                // the node with DmaError::NotOwnNode before the kernel sees
                // it.
                let mut raw: sys::ddi_dma_handle_t = ptr::null_mut();
                // SAFETY (in the Rust sense only; the DDI forbids this): the
                // node is live by the contract of GemDma::against, the
                // attributes are a valid structure the kernel copies,
                // DDI_DMA_SLEEP from thread context, and `raw` is writable.
                let r = unsafe {
                    sys::ddi_dma_alloc_handle(
                        dev.as_raw(),
                        attr.as_raw(),
                        sys::DDI_DMA_SLEEP,
                        ptr::null_mut(),
                        &mut raw,
                    )
                };
                if raw.is_null() || r != sys::DDI_SUCCESS {
                    return Err(dmaerr::code_errno(r));
                }
                SgHandle::Raw(raw)
            }
        };
        // SAFETY: a fresh handle; the address and length describe memory we
        // own in the kernel address space (NULL `as`); DDI_DMA_SLEEP from
        // thread context; NULL cookie and count pointers as the manual asks
        // of new callers.
        let r = unsafe {
            sys::ddi_dma_addr_bind_handle(
                handle.as_raw(),
                ptr::null_mut(),
                self.kaddr.as_ptr() as sys::caddr_t,
                self.size,
                sys::DDI_DMA_RDWR | sys::DDI_DMA_CONSISTENT,
                sys::DDI_DMA_SLEEP,
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
            )
        };
        match r {
            sys::DDI_DMA_MAPPED => {}
            sys::DDI_DMA_PARTIAL_MAP => {
                // Never requested (no DDI_DMA_PARTIAL); release and refuse.
                // SAFETY: the handle was bound by the call above.
                unsafe { sys::ddi_dma_unbind_handle(handle.as_raw()) };
                return Err(dmaerr::object_errno(DmaError::PartialMap));
            }
            // ENOMEM for a shortage or no mapping (not EFAULT, which in a
            // create ioctl reads as a bad user pointer), EIO for codes a
            // fresh handle cannot return.
            _ => return Err(dmaerr::code_errno(r)),
        }
        // SAFETY: the handle is bound.
        let n = unsafe { sys::ddi_dma_ncookies(handle.as_raw()) };
        Ok(SgTable { handle, ncookies: usize::try_from(n).unwrap_or(0) })
    }

    /// Returns the size in bytes, a multiple of the page size.
    pub fn size(&self) -> usize {
        self.size
    }

    /// Returns the cookie that `devmap_umem_setup(9F)` maps.
    pub fn umem_cookie(&self) -> UmemCookie {
        UmemCookie(self.cookie)
    }

    /// Returns the kernel address of the memory. The memory is shared with
    /// every process that maps the object, so access through the pointer
    /// races with them; a driver uses it for device-visible data under its
    /// own protocol.
    pub fn kernel_ptr(&self) -> NonNull<u8> {
        self.kaddr
    }

    /// Returns the number of scatter/gather entries, 0 without DMA.
    pub fn sg_len(&self) -> usize {
        self.sg.as_ref().map_or(0, |s| s.ncookies)
    }

    /// Iterates over the scatter/gather table: the DMA cookies of the
    /// binding, each a bus address and a length, in order. Empty if the
    /// memory is not bound for DMA.
    pub fn sg_iter(&self) -> SgIter<'_> {
        match &self.sg {
            Some(sg) => {
                SgIter { handle: sg.handle.as_raw(), cur: ptr::null(), remaining: sg.ncookies, _mem: PhantomData }
            }
            None => SgIter { handle: ptr::null_mut(), cur: ptr::null(), remaining: 0, _mem: PhantomData },
        }
    }
}

impl Drop for SysMem {
    fn drop(&mut self) {
        if let Some(sg) = self.sg.take() {
            // SAFETY: bound in `bind`, unbound once, before the handle is
            // freed by its own Drop and before the memory goes.
            unsafe { sys::ddi_dma_unbind_handle(sg.handle.as_raw()) };
            drop(sg);
        }
        // SAFETY: the cookie came from ddi_umem_alloc and is freed once; no
        // mapping refers to it any more, because every mapping holds a
        // reference to the object that owns this memory.
        unsafe { sys::ddi_umem_free(self.cookie.as_ptr()) }
    }
}

/// How the object memory of a device is bound for DMA
/// ([`Driver::gem_dma`](crate::Driver::gem_dma)): the attributes.
///
/// [`GemDma::new`] binds against the device's own node, the
/// [`OwnNode`] that [`Device::attach`](crate::Device::attach) received and
/// hands to the GEM state, the only node `ddi_dma_alloc_handle(9F)`
/// accepts. Only with the `panic-drill` cargo feature, a test aid that is
/// off by default, does `GemDma::against` exist, which names another node;
/// it is for the `rdrmnull` DMA drill, which reproduces the kernel panic that
/// binding against `ddi_root_node(9F)` from a pseudo node caused (see
/// `tools/test/rdrmnull-dma-root-node.conf`). Without the feature a driver
/// cannot name another node at all.
#[derive(Clone, Copy)]
pub struct GemDma {
    attr: DmaAttr,
    #[cfg(feature = "panic-drill")]
    foreign: Option<DevInfo>,
}

impl GemDma {
    /// Binds with `attr` against the device's own node.
    pub const fn new(attr: DmaAttr) -> GemDma {
        GemDma {
            attr,
            #[cfg(feature = "panic-drill")]
            foreign: None,
        }
    }

    /// Binds with `attr` against `node` instead of the device's own node,
    /// with raw `ddi_dma_alloc_handle(9F)` calls that bypass illumos-ddi's
    /// own-node rule and its guard. Only with the `panic-drill` feature.
    ///
    /// # Safety
    /// `ddi_dma_alloc_handle(9F)` requires the driver's own node. Any other
    /// node is outside the DDI: the caller must keep `node` valid for as long
    /// as the device exists and accepts whatever the DMA framework does with
    /// it, up to a panic (for `ddi_root_node(9F)`, a panic at the first
    /// bind). Only the DMA drill of `rdrmnull` uses this.
    #[cfg(feature = "panic-drill")]
    pub const unsafe fn against(attr: DmaAttr, node: DevInfo) -> GemDma {
        GemDma { attr, foreign: Some(node) }
    }
}

impl DmaTarget<'_> {
    fn dev(self) -> DevInfo {
        match self {
            DmaTarget::Own(node) => node.dev(),
            #[cfg(feature = "panic-drill")]
            DmaTarget::Foreign(dev) => dev,
        }
    }
}

/// The scatter/gather table of a [`SysMem`], from `ddi_dma_cookie_iter(9F)`.
pub struct SgIter<'a> {
    handle: sys::ddi_dma_handle_t,
    cur: *const sys::ddi_dma_cookie_t,
    remaining: usize,
    _mem: PhantomData<&'a SysMem>,
}

impl Iterator for SgIter<'_> {
    type Item = DmaCookie;
    fn next(&mut self) -> Option<DmaCookie> {
        if self.remaining == 0 {
            return None;
        }
        // SAFETY: the handle stays bound while the SysMem it borrows lives;
        // `cur` is NULL or the previous cookie, as the manual requires.
        let next = unsafe { sys::ddi_dma_cookie_iter(self.handle, self.cur) };
        // SAFETY: a non-null result is a cookie owned by the bound handle.
        let Some(c) = (unsafe { next.as_ref() }) else {
            self.remaining = 0;
            return None;
        };
        self.cur = next;
        self.remaining -= 1;
        // SAFETY: both members of the union are integers over the same
        // eight bytes.
        let address = unsafe { c._dmu._dmac_ll };
        Some(DmaCookie { address, size: c.dmac_size })
    }
    fn size_hint(&self) -> (usize, Option<usize>) {
        (self.remaining, Some(self.remaining))
    }
}

impl ExactSizeIterator for SgIter<'_> {}
impl FusedIterator for SgIter<'_> {}

// --- the object trait -----------------------------------------------------------

/// A piece of a user mapping of an object, as the devmap framework sees it.
#[derive(Clone, Copy, Debug)]
pub struct Mapping {
    dhp: sys::devmap_cookie_t,
    uoff: u64,
    len: usize,
    obj_off: u64,
    settled: bool,
}

impl Mapping {
    /// Returns the mapping handle, for devmap functions the core does not
    /// wrap. It is valid until the piece is unmapped, which cannot happen
    /// while the object's mapping list is locked (see
    /// [`GemRef::with_mappings`]).
    pub fn raw(&self) -> sys::devmap_cookie_t {
        self.dhp
    }
    /// Returns the `mmap(2)` offset at which the piece starts.
    pub fn user_offset(&self) -> u64 {
        self.uoff
    }
    /// Returns the offset of the piece within the object.
    pub fn object_offset(&self) -> u64 {
        self.obj_off
    }
    /// Returns the length of the piece in bytes.
    pub fn len(&self) -> usize {
        self.len
    }
    /// Reports whether the piece is empty (never, for a live mapping).
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
    /// Reports whether the framework has finished setting up the piece.
    ///
    /// A piece that a partial `munmap(2)` split off is recorded from inside
    /// `devmap_unmap(9E)`, but the framework adjusts its offset into the
    /// memory (`devmap_handle_reduce_len`) only after that callback returns,
    /// and nothing tells the driver when. Until the piece's next fault,
    /// which runs after the `munmap(2)` has finished, the piece is not
    /// settled: its translations may be invalidated with
    /// [`Mapping::unload`], but it must not be pointed at other memory,
    /// because the framework would then add the offset a second time.
    /// [`GemObject::mapped`] runs for it at that fault, with the final
    /// geometry, before [`GemObject::access`].
    pub fn is_settled(&self) -> bool {
        self.settled
    }
    /// Invalidates the translations of the whole piece with
    /// `devmap_unload(9F)`, so that the next access calls
    /// [`GemObject::access`] again. Works on the mappings of any process.
    /// Returns the framework's error code on failure.
    pub fn unload(&self, _ctx: &Sleepable) -> Result<(), c_int> {
        // SAFETY: a live mapping handle (see `raw`); user or kernel context,
        // which the token proves. The offset is the piece's own user offset:
        // devmap_unload counts from dh_uoff, so 0 would not mean "the whole
        // mapping" as the manual suggests.
        let r = unsafe { sys::devmap_unload(self.dhp, self.uoff as sys::offset_t, self.len) };
        if r == 0 { Ok(()) } else { Err(r) }
    }
}

/// A `devmap_access(9E)` call: an access to a mapping without a valid
/// translation.
pub struct Fault {
    dhp: sys::devmap_cookie_t,
    pvtp: *mut c_void,
    off: sys::offset_t,
    len: usize,
    type_: sys::uint_t,
    rw: sys::uint_t,
}

impl Fault {
    /// Loads the translations with `devmap_default_access(9F)` and returns
    /// its result, which the callback returns to the framework.
    pub fn default_access(&self) -> c_int {
        // SAFETY: called from devmap_access(9E) with its unmodified
        // arguments, as the manual requires.
        unsafe { sys::devmap_default_access(self.dhp, self.pvtp, self.off, self.len, self.type_, self.rw) }
    }
    /// Loads the translations of the faulting range with `devmap_load(9F)`
    /// (the faulting process's own mapping, as the manual requires) and
    /// returns its result.
    pub fn load(&self) -> c_int {
        // SAFETY: as for default_access; devmap_load may be called from the
        // devmap_access(9E) of the faulting process.
        unsafe { sys::devmap_load(self.dhp, self.off, self.len, self.type_, self.rw) }
    }
    /// Returns the user offset of the access.
    pub fn offset(&self) -> u64 {
        self.off as u64
    }
    /// Returns the length of the access.
    pub fn len(&self) -> usize {
        self.len
    }
    /// Reports whether the length is zero.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
    /// Returns the access type (`DEVMAP_ACCESS`, `DEVMAP_LOCK`,
    /// `DEVMAP_UNLOCK`).
    pub fn kind(&self) -> sys::uint_t {
        self.type_
    }
}

/// A driver's buffer object: what the core needs from it.
pub trait GemObject: Send + Sync + Sized + 'static {
    /// Whether the memory behind the object can change while it is mapped.
    /// Mappings of such objects are set up with `DEVMAP_ALLOW_REMAP`, which
    /// the devmap framework needs for `devmap_umem_remap`; the framework
    /// then refuses to soft-lock them (`FC_NOSUPPORT`), so raw I/O straight
    /// from such a mapping fails. Fixed for the object's life.
    fn movable(&self) -> bool {
        false
    }
    /// The size in bytes: a non-zero multiple of the page size, fixed for
    /// the object's life.
    fn size(&self) -> usize;
    /// The kernel memory the object holds, for the statistics.
    fn memory_bytes(&self) -> usize {
        self.size()
    }
    /// The number of scatter/gather entries of the object's DMA binding,
    /// for the statistics; 0 if it is not bound.
    fn dma_segments(&self) -> usize {
        0
    }
    /// The memory that user mappings show now. For a movable object this
    /// is read with the mapping list locked (see
    /// [`GemRef::with_mappings`]).
    fn umem(&self) -> UmemCookie;
    /// Called with the mapping list locked once a mapping piece is set up:
    /// in `mmap(2)` and `fork(2)`, and for a piece split off by a partial
    /// `munmap(2)` at its first fault afterwards (see
    /// [`Mapping::is_settled`]). A movable object points the piece at its
    /// current memory here, in case it moved after the piece was copied. An
    /// error fails the `mmap(2)` or `fork(2)`, or the faulting access.
    fn mapped(&self, _m: &Mapping, _ctx: &Sleepable) -> Result<(), Errno> {
        Ok(())
    }
    /// Handles `devmap_access(9E)`. The default loads the translations
    /// with `devmap_default_access(9F)`.
    fn access(&self, _m: &Mapping, fault: &Fault) -> c_int {
        fault.default_access()
    }
    /// Called before a handle to the object is created in file `file`
    /// (Linux's `drm_gem_object_funcs.open`), with no GEM lock held; an
    /// error fails the handle's creation. Once per handle: a file may hold
    /// several handles to one object.
    fn handle_opened(&self, file: CloneId, ctx: &Sleepable) -> Result<(), Errno> {
        let _ = (file, ctx);
        Ok(())
    }
    /// Called after a handle of file `file` to the object was removed
    /// (Linux's `drm_gem_object_funcs.close`), with no GEM lock held, and
    /// for a handle whose creation failed after
    /// [`GemObject::handle_opened`] succeeded.
    fn handle_closed(&self, file: CloneId, ctx: &Sleepable) {
        let _ = (file, ctx);
    }
    /// Called when the object is destroyed, in thread context, before its
    /// memory is freed: a device driver tells the device to stop using the
    /// memory here (virtio-gpu's `RESOURCE_UNREF`) and waits for it.
    fn release(&mut self, ctx: &Sleepable) {
        let _ = ctx;
    }
    /// For an object a driver made from another device's dma-buf
    /// ([`crate::Driver::prime_import`]): the exporting object, so that a
    /// file importing the same buffer again gets its handle again.
    fn imported_from(&self) -> Option<&GemRef<Self>> {
        None
    }
}

impl GemObject for SysMem {
    fn size(&self) -> usize {
        self.size
    }
    fn dma_segments(&self) -> usize {
        self.sg_len()
    }
    fn umem(&self) -> UmemCookie {
        self.umem_cookie()
    }
}

// --- the object and its references -----------------------------------------------

/// One mapping piece; the devmap private data (`pvtp`). Every piece holds
/// a reference to its object for as long as the framework has the pointer.
struct MapPriv<O: GemObject> {
    /// The piece's reference; `None` only while it is being torn down.
    obj: Option<GemRef<O>>,
    dhp: sys::devmap_cookie_t,
    uoff: u64,
    /// The user offset of the object's first byte in this mapping: the
    /// fake offset for a mapping of a DRM file, 0 for a dma-buf file.
    base: u64,
    len: usize,
    /// See [`Mapping::is_settled`]. Set only with the mapping list locked.
    settled: AtomicBool,
}

impl<O: GemObject> MapPriv<O> {
    fn mapping(&self, _obj: &GemRef<O>) -> Mapping {
        Mapping {
            dhp: self.dhp,
            uoff: self.uoff,
            len: self.len,
            obj_off: self.uoff.wrapping_sub(self.base),
            settled: self.settled.load(Ordering::Acquire),
        }
    }
}

/// The mapping pieces of an object.
struct MapList<O: GemObject>(KVec<NonNull<MapPriv<O>>>);

// SAFETY: the pointers are owned by the devmap framework (as pvtp) and only
// dereferenced with the list's mutex held or by the callbacks that own them.
unsafe impl<O: GemObject> Send for MapList<O> {}

/// A GEM object: the core's part, the driver's part and the mapping list.
pub struct GemInner<O: GemObject> {
    common: ObjCommon,
    core: KArc<GemCore<O>>,
    maps: Mutex<MapList<O>>,
    /// The fences of the work using the object.
    resv: Resv,
    /// The anonymous-file number of the object's dma-buf, 0 for none; set
    /// and cleared with the device's dma-buf table locked
    /// ([`crate::anon_fd`]).
    dmabuf: AtomicU32,
    obj: O,
}

impl<O: GemObject> Deferrable for GemInner<O> {
    fn defer_link(&self) -> &defer::DeferLink {
        &self.common.link
    }
}

impl<O: GemObject> Drop for GemInner<O> {
    /// Destroys the object; runs in thread context only (see the module
    /// documentation). No handle, name or mapping refers to the object any
    /// more, since each holds a reference; only the offset table can still
    /// see it, and it cannot take a reference from a count of zero.
    fn drop(&mut self) {
        let off = self.common.offset();
        if off != 0 {
            let removed = self.core.offsets.lock().remove(off);
            if removed.is_some() {
                self.core.stats.offsets.fetch_sub(1, Ordering::Relaxed);
            }
        }
        self.core.stats.objects.fetch_sub(1, Ordering::Relaxed);
        self.core.stats.memory.fetch_sub(self.obj.memory_bytes() as u64, Ordering::Relaxed);
        self.core.stats.segments.fetch_sub(self.obj.dma_segments() as u64, Ordering::Relaxed);
        self.core.publish();
        // SAFETY: objects are destroyed in thread context only: the last
        // reference goes through GemRef::put (Sleepable), or is parked and
        // destroyed by the task queue or another thread-context entry point
        // (see the module documentation); a creation that fails drops the
        // new object in its creator's thread context.
        let ctx = unsafe { Sleepable::assume() };
        self.obj.release(&ctx);
        // `obj` is dropped after this, which frees its memory.
    }
}

/// A counted reference to a GEM object. See the module documentation for
/// how to drop it.
#[must_use = "drop GEM references with put() or put_atomic()"]
pub struct GemRef<O: GemObject> {
    arc: ManuallyDrop<KArc<GemInner<O>>>,
}

impl<O: GemObject> GemRef<O> {
    fn from_arc(arc: KArc<GemInner<O>>) -> GemRef<O> {
        GemRef { arc: ManuallyDrop::new(arc) }
    }

    fn into_arc(self) -> KArc<GemInner<O>> {
        let mut me = ManuallyDrop::new(self);
        // SAFETY: taken once; `me` is never dropped.
        unsafe { ManuallyDrop::take(&mut me.arc) }
    }

    fn inner(&self) -> &GemInner<O> {
        &self.arc
    }

    /// Returns the driver's object.
    pub fn object(&self) -> &O {
        &self.inner().obj
    }

    /// Returns the core's part of the object.
    pub fn common(&self) -> &ObjCommon {
        &self.inner().common
    }

    /// Returns the size in bytes.
    pub fn size(&self) -> usize {
        self.common().size()
    }

    /// Returns the object's reservation: the fences of the work using it.
    pub fn resv(&self) -> &Resv {
        &self.inner().resv
    }

    /// The object's dma-buf number, 0 for none ([`crate::anon_fd`]).
    pub(crate) fn dmabuf(&self) -> &AtomicU32 {
        &self.inner().dmabuf
    }

    /// Returns the device's GEM state.
    pub fn core(&self) -> &KArc<GemCore<O>> {
        &self.inner().core
    }

    /// Drops the reference in thread context; the object is destroyed now
    /// if it was the last one.
    pub fn put(self, _ctx: &Sleepable) {
        if let Some(last) = KArc::release(self.into_arc()) {
            drop(last);
        }
    }

    /// Drops the reference in the context `ctx`: [`GemRef::put`] or
    /// [`GemRef::put_atomic`].
    pub fn put_in(self, ctx: Ctx<'_>) {
        match ctx {
            Ctx::Sleep(s) => self.put(s),
            Ctx::Atomic(a) => self.put_atomic(a),
        }
    }

    /// Drops the reference in interrupt context; if it was the last one the
    /// object is parked and destroyed later by the device's task queue.
    pub fn put_atomic(self, _ctx: &Atomic) {
        Self::defer_put(self.into_arc());
    }

    fn defer_put(arc: KArc<GemInner<O>>) {
        if let Some(last) = KArc::release(arc) {
            // Keep the device state alive until the drain is scheduled; see
            // GemCore::quiesce for why this reference cannot be the last.
            let core = KArc::clone(&last.core);
            core.defer(last);
            drop(core);
        }
    }

    /// The object's address, for a structure that refers to the object
    /// without holding a reference of its own (the facade's
    /// `drm_gem_object`, whose references are counted here).
    pub fn as_raw(&self) -> NonNull<c_void> {
        KArc::as_ptr(&self.arc).cast()
    }

    /// A new reference to the object at `p`.
    ///
    /// # Safety
    /// `p` came from [`GemRef::as_raw`] or [`GemRef::into_raw`] of a
    /// `GemRef<O>` of the same `O`, and the object is alive: the caller
    /// holds a reference to it, counted or borrowed.
    pub unsafe fn clone_from_raw(p: NonNull<c_void>) -> GemRef<O> {
        // SAFETY: the caller's contract; the borrowed reference is given
        // back unchanged.
        let borrowed = ManuallyDrop::new(unsafe { GemRef::<O>::from_raw(p) });
        GemRef::clone(&borrowed)
    }

    /// Gives up the reference as a pointer, for storage where a Rust value
    /// cannot go (a static read by a timer callback);
    /// [`GemRef::from_raw`] takes it back.
    pub fn into_raw(self) -> NonNull<c_void> {
        KArc::into_raw(self.into_arc()).cast()
    }

    /// Takes back a reference given away with [`GemRef::into_raw`].
    ///
    /// # Safety
    /// `p` must come from [`GemRef::into_raw`] of the same object type,
    /// once.
    pub unsafe fn from_raw(p: NonNull<c_void>) -> GemRef<O> {
        // SAFETY: the caller's contract.
        GemRef::from_arc(unsafe { KArc::from_raw(p.cast()) })
    }

    /// Calls `f` with the object and its mapping pieces, with the mapping
    /// list locked: no piece is added or removed while `f` runs, and the
    /// handles stay valid. A movable object changes its memory in here.
    pub fn with_mappings<R>(&self, f: impl FnOnce(&O, &mut dyn Iterator<Item = Mapping>) -> R) -> R {
        let maps = self.inner().maps.lock();
        let mut it = maps.0.iter().map(|p| {
            // SAFETY: recorded pieces stay allocated until unrecorded, which
            // needs this lock.
            let m = unsafe { p.as_ref() };
            m.mapping(self)
        });
        f(&self.inner().obj, &mut it)
    }

    /// Returns the number of recorded mapping pieces.
    pub fn mapping_count(&self) -> usize {
        self.inner().maps.lock().0.len()
    }
}

impl<O: GemObject> Clone for GemRef<O> {
    fn clone(&self) -> GemRef<O> {
        GemRef::from_arc(KArc::clone(&self.arc))
    }
}

impl<O: GemObject> Drop for GemRef<O> {
    /// An implicit drop does not know its context, so it takes the
    /// interrupt-safe path: the last reference parks the object.
    fn drop(&mut self) {
        // SAFETY: taken once, in drop.
        let arc = unsafe { ManuallyDrop::take(&mut self.arc) };
        Self::defer_put(arc);
    }
}

impl<O: GemObject> ObjRef for GemRef<O> {
    fn common(&self) -> &ObjCommon {
        GemRef::common(self)
    }
    fn clone_ref(&self) -> Self {
        self.clone()
    }
    fn same(&self, other: &Self) -> bool {
        KArc::ptr_eq(&self.arc, &other.arc)
    }
}

/// Drops an optional reference in thread context.
fn put_opt<O: GemObject>(r: Option<GemRef<O>>, ctx: &Sleepable) {
    if let Some(r) = r {
        r.put(ctx);
    }
}

// --- statistics -------------------------------------------------------------------

ddi::named_kstat! {
    /// The `gem` kstat of a DRM device (`kstat -m <driver> -n gem`).
    pub struct GemKstat {
        /// Live objects.
        pub objects: u64,
        /// Bytes of object memory (`ddi_umem_alloc`).
        pub umem_bytes: u64,
        /// DMA cookies (scatter/gather entries) of the live objects.
        pub dma_segments: u64,
        /// Handles in all files.
        pub handles: u64,
        /// FLINK names.
        pub names: u64,
        /// Objects with an mmap offset.
        pub offsets: u64,
        /// Recorded mapping pieces.
        pub mappings: u64,
        /// mmap(2) calls between devmap(9E) and devmap_map(9E).
        pub pending_maps: u64,
        /// Objects whose last reference was dropped in interrupt context.
        pub deferred_frees: u64,
        /// Runs of the deferred-release task.
        pub drain_tasks: u64,
        /// devmap_access(9E) calls.
        pub faults: u64,
    }
}

/// The counters behind the kstat, updated with atomics from any context
/// and copied to the kstat from thread context ([`GemCore::publish`]).
#[derive(Default)]
struct Stats {
    objects: AtomicU64,
    memory: AtomicU64,
    segments: AtomicU64,
    handles: AtomicU64,
    names: AtomicU64,
    offsets: AtomicU64,
    mappings: AtomicU64,
    pending: AtomicU64,
    deferred: AtomicU64,
    drains: AtomicU64,
    faults: AtomicU64,
}

/// A snapshot of a device's GEM statistics.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GemStats {
    /// Live objects.
    pub objects: u64,
    /// Bytes of object memory.
    pub umem_bytes: u64,
    /// DMA cookies of the live objects.
    pub dma_segments: u64,
    /// Handles in all files.
    pub handles: u64,
    /// FLINK names.
    pub names: u64,
    /// Objects with an mmap offset.
    pub offsets: u64,
    /// Recorded mapping pieces.
    pub mappings: u64,
    /// mmap(2) calls in progress.
    pub pending_maps: u64,
    /// Deferred releases so far.
    pub deferred_frees: u64,
    /// Deferred-release task runs so far.
    pub drain_tasks: u64,
    /// devmap_access(9E) calls so far.
    pub faults: u64,
}

// --- per-file state ---------------------------------------------------------------

/// The GEM state of one open file: its handle table.
pub(crate) struct FileGem<O: GemObject> {
    handles: Mutex<IdMap<GemRef<O>, KIdSpace>>,
}

/// An `mmap(2)` between `devmap(9E)` and `devmap_map(9E)`.
struct Pending<O: GemObject> {
    dhp: usize,
    owner: MapOwner,
    /// The thread in `mmap(2)` (`ddi_get_kt_did(9F)`): both entry points
    /// run in it, one after the other, so an entry that the same thread
    /// left behind belongs to an `mmap(2)` that failed in between.
    thread: sys::kt_did_t,
    obj: GemRef<O>,
}

/// The file an `mmap(2)` maps through: a DRM file (by clone id) or a
/// dma-buf file (by its anonymous-file number).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum MapOwner {
    /// A DRM file, which must hold a handle to the object.
    File(CloneId),
    /// A dma-buf file, which stands for the object itself.
    DmaBuf(u32),
}

fn current_thread() -> sys::kt_did_t {
    // SAFETY: ddi_get_kt_did(9F) may be called from any context.
    unsafe { sys::ddi_get_kt_did() }
}

/// A pointer from the offset table to an object; holds no reference.
struct ObjPtr<O: GemObject>(NonNull<GemInner<O>>);

// SAFETY: only dereferenced through KArc::upgrade under the offset lock.
unsafe impl<O: GemObject> Send for ObjPtr<O> {}

// --- the device's GEM state -------------------------------------------------------

/// The GEM state of one device. See the module documentation.
pub struct GemCore<O: GemObject> {
    // Declared first so that it is dropped first: destroying the queue
    // waits for a drain task that may still run, and everything such a
    // task touches is declared below.
    tq: TaskQueue,
    dev: DevInfo,
    /// The device's own node, which object memory is bound against for DMA.
    node: OwnNode,
    dma: Option<DmaAttr>,
    /// The node the DMA drill binds against instead (`GemDma::against`).
    #[cfg(feature = "panic-drill")]
    foreign: Option<DevInfo>,
    max_memory: u64,
    names: Mutex<IdMap<GemRef<O>, KIdSpace>>,
    offsets: Mutex<OffsetMap<ObjPtr<O>, VmemArena>>,
    files: Mutex<KVec<Option<KArc<FileGem<O>>>>>,
    pending: Mutex<KVec<Pending<O>>>,
    deferred: DeferList<GemInner<O>>,
    drain_scheduled: AtomicBool,
    stats: Stats,
    kstat: Option<TypedKstat<GemKstat>>,
}

/// Object memory charged to a device's budget ahead of its allocation
/// ([`GemCore::reserve`]); returned when dropped.
#[must_use = "a reservation is returned as soon as it is dropped"]
pub struct Reservation<'a, O: GemObject> {
    core: &'a GemCore<O>,
    bytes: u64,
}

impl<O: GemObject> Reservation<'_, O> {
    /// Returns the number of bytes reserved.
    pub fn bytes(&self) -> usize {
        self.bytes as usize
    }
}

impl<O: GemObject> Drop for Reservation<'_, O> {
    fn drop(&mut self) {
        self.core.stats.memory.fetch_sub(self.bytes, Ordering::Relaxed);
    }
}

/// A raw pointer to the device's GEM state, for the drain task.
struct CorePtr<O: GemObject>(*const GemCore<O>);
// SAFETY: the task only runs while the state exists (GemCore::quiesce and
// the field order of GemCore).
unsafe impl<O: GemObject> Send for CorePtr<O> {}

impl<O: GemObject> GemCore<O> {
    /// Creates the GEM state of a device: the deferred-release task
    /// queue, the name space, the offset arena and the `gem` kstat. `node`
    /// is the device's own node from attach. `dma` says how every object's
    /// memory is bound (against `node`), or is `None` for devices that do no
    /// DMA; `max_memory` limits the memory of all objects together.
    pub(crate) fn new(
        node: OwnNode,
        dma: Option<GemDma>,
        max_memory: usize,
        ctx: &Sleepable,
    ) -> Result<KArc<GemCore<O>>, Errno> {
        let dev = node.dev();
        let drv = dev.driver_name().to_str().map_err(|_| Errno::EINVAL)?;
        let inst = dev.instance().0;
        let tq = TaskQueue::new(dev, c"gemfree", NonZeroU32::MIN, ctx).map_err(|_| Errno::ENOMEM)?;
        let names = KIdSpace::new(NameBuf::new(format_args!("{drv}{inst}.names"))?.as_cstr(), ctx)?;
        let arena = VmemArena::new(NameBuf::new(format_args!("{drv}{inst}.mmap"))?.as_cstr(), ctx)?;
        let kstat = TypedKstat::<GemKstat>::new(
            ddi::KstatId { module: dev.driver_name(), instance: dev.instance(), name: c"gem", class: c"misc" },
            ctx,
        );
        if kstat.is_err() {
            dev.warn(format_args!("cannot create the gem kstat; statistics are unavailable"));
        }
        KArc::new(
            GemCore {
                tq,
                dev,
                node,
                dma: dma.map(|d| d.attr),
                #[cfg(feature = "panic-drill")]
                foreign: dma.and_then(|d| d.foreign),
                max_memory: max_memory as u64,
                names: Mutex::new(IdMap::new(names)),
                offsets: Mutex::new(OffsetMap::new(arena)),
                files: Mutex::new(KVec::new()),
                pending: Mutex::new(KVec::new()),
                deferred: DeferList::new(),
                drain_scheduled: AtomicBool::new(false),
                stats: Stats::default(),
                kstat: kstat.ok(),
            },
            ctx,
        )
    }

    /// Returns the device node.
    pub fn dev_info(&self) -> DevInfo {
        self.dev
    }

    /// Returns the attributes object memory is bound for DMA with, if any.
    pub fn dma_attr(&self) -> Option<&DmaAttr> {
        self.dma.as_ref()
    }

    /// Allocates system memory for an object of this device, bound for DMA
    /// if the driver asked for it ([`SysMem::new`]).
    pub fn sys_mem(&self, size: usize, ctx: &Sleepable) -> Result<SysMem, Errno> {
        SysMem::alloc(self.dma_target(), size, self.dma.as_ref(), ctx)
    }

    fn dma_target(&self) -> DmaTarget<'_> {
        #[cfg(feature = "panic-drill")]
        if let Some(dev) = self.foreign {
            return DmaTarget::Foreign(dev);
        }
        DmaTarget::Own(&self.node)
    }

    /// Returns a snapshot of the statistics.
    pub fn stats(&self) -> GemStats {
        let s = &self.stats;
        let l = |a: &AtomicU64| a.load(Ordering::Relaxed);
        GemStats {
            objects: l(&s.objects),
            umem_bytes: l(&s.memory),
            dma_segments: l(&s.segments),
            handles: l(&s.handles),
            names: l(&s.names),
            offsets: l(&s.offsets),
            mappings: l(&s.mappings),
            pending_maps: l(&s.pending),
            deferred_frees: l(&s.deferred),
            drain_tasks: l(&s.drains),
            faults: l(&s.faults),
        }
    }

    /// Copies the counters to the kstat. Thread context only (the kstat
    /// mutex is adaptive).
    fn publish(&self) {
        let Some(k) = &self.kstat else { return };
        let s = self.stats();
        let mut g = k.lock();
        g.objects.set(s.objects);
        g.umem_bytes.set(s.umem_bytes);
        g.dma_segments.set(s.dma_segments);
        g.handles.set(s.handles);
        g.names.set(s.names);
        g.offsets.set(s.offsets);
        g.mappings.set(s.mappings);
        g.pending_maps.set(s.pending_maps);
        g.deferred_frees.set(s.deferred_frees);
        g.drain_tasks.set(s.drain_tasks);
        g.faults.set(s.faults);
    }

    // --- deferred release ---

    /// Parks an object whose last reference was dropped where blocking is
    /// not allowed, and schedules the drain. Any context.
    fn defer(&self, last: KArcLast<GemInner<O>>) {
        self.stats.deferred.fetch_add(1, Ordering::Relaxed);
        self.deferred.push(last);
        if self.drain_scheduled.swap(true, Ordering::AcqRel) {
            return;
        }
        let me = CorePtr(self as *const GemCore<O>);
        let task = move |ctx: &Sleepable| {
            let me = me;
            // SAFETY: see CorePtr.
            let core = unsafe { &*me.0 };
            core.stats.drains.fetch_add(1, Ordering::Relaxed);
            core.drain_scheduled.store(false, Ordering::Release);
            core.drain(ctx);
        };
        if self.tq.try_dispatch(task).is_err() {
            // No memory for the task right now. The objects stay parked; the
            // next thread-context entry point or detach drains them.
            self.drain_scheduled.store(false, Ordering::Release);
        }
    }

    /// Destroys every parked object. Thread context; cheap when nothing is
    /// parked, so every entry point calls it.
    pub(crate) fn drain(&self, ctx: &Sleepable) {
        if self.deferred.is_empty() {
            return;
        }
        drop(self.deferred.take_all(ctx));
        self.publish();
    }

    /// The work every thread-context entry point does first: destroys
    /// parked objects, and drops the references of `mmap(2)` calls of the
    /// calling thread that failed after `devmap(9E)` (see [`Pending`]).
    fn housekeeping(&self, ctx: &Sleepable) {
        self.drain(ctx);
        let me = current_thread();
        loop {
            let p = {
                let mut l = self.pending.lock();
                let i = l.iter().position(|p| p.thread == me);
                i.and_then(|i| l.swap_remove(i))
            };
            let Some(p) = p else { break };
            self.stats.pending.fetch_sub(1, Ordering::Relaxed);
            p.obj.put(ctx);
            self.publish();
        }
    }

    /// Prepares for detach: waits for the drain task, destroys whatever is
    /// still parked, and checks that nothing is left. Fails, and the device
    /// must stay attached, if any object or handle remains or if another
    /// reference to this state exists (an interrupt-context release that is
    /// still scheduling its drain).
    pub(crate) fn quiesce(this: &KArc<GemCore<O>>, ctx: &Sleepable) -> Result<(), GemStats> {
        this.tq.wait(ctx);
        this.drain(ctx);
        let s = this.stats();
        let busy =
            s.objects + s.umem_bytes + s.dma_segments + s.handles + s.names + s.mappings + s.pending_maps + s.offsets
                != 0;
        if busy || KArc::strong_count(this) != 1 {
            return Err(s);
        }
        Ok(())
    }

    // --- files ---

    /// Creates the GEM state of a newly opened file.
    pub(crate) fn file_open(&self, clone: CloneId, ctx: &Sleepable) -> Result<(), Errno> {
        let drv = self.dev.driver_name().to_str().map_err(|_| Errno::EINVAL)?;
        let name = NameBuf::new(format_args!("{drv}{}.f{}", self.dev.instance().0, clone.get()))?;
        let ids = KIdSpace::new(name.as_cstr(), ctx)?;
        let fg = KArc::new(FileGem { handles: Mutex::new(IdMap::new(ids)) }, ctx)?;
        let mut files = self.files.lock();
        while files.len() <= clone.index() {
            files.push(None, ctx)?;
        }
        match files.get_mut(clone.index()) {
            Some(slot @ None) => {
                *slot = Some(fg);
                Ok(())
            }
            // A clone id is handed out once until its close(9E).
            _ => Err(Errno::EBUSY),
        }
    }

    /// Releases the GEM state of a closed file: every handle, and every
    /// `mmap(2)` reference parked for it.
    pub(crate) fn file_close(&self, clone: CloneId, ctx: &Sleepable) {
        self.drain(ctx);
        let fg = self.files.lock().get_mut(clone.index()).and_then(Option::take);
        loop {
            let p = {
                let mut l = self.pending.lock();
                let i = l.iter().position(|p| p.owner == MapOwner::File(clone));
                i.and_then(|i| l.swap_remove(i))
            };
            let Some(p) = p else { break };
            self.stats.pending.fetch_sub(1, Ordering::Relaxed);
            p.obj.put(ctx);
        }
        if let Some(fg) = fg {
            loop {
                let Some((_, r)) = fg.handles.lock().pop_first() else { break };
                self.handle_released(clone, r, ctx);
            }
            drop(fg);
        }
        self.publish();
    }

    fn file(&self, clone: CloneId) -> Result<KArc<FileGem<O>>, Errno> {
        self.files.lock().get(clone.index()).and_then(Option::as_ref).cloned().ok_or(Errno::ENXIO)
    }

    // --- objects and handles ---

    /// Charges `bytes` of object memory to the device's budget
    /// ([`DriverInfo::max_memory`](crate::DriverInfo::max_memory)) before they are
    /// allocated, so that concurrent requests cannot allocate past it even
    /// for a moment. `ENOMEM` if the budget does not have them. The charge
    /// goes back when the reservation is dropped, unless
    /// [`GemCore::create_reserved`] turns it into an object's.
    pub fn reserve(&self, bytes: usize, _ctx: &Sleepable) -> Result<Reservation<'_, O>, Errno> {
        let bytes = bytes as u64;
        let before = self.stats.memory.fetch_add(bytes, Ordering::Relaxed);
        if before.checked_add(bytes).is_none_or(|total| total > self.max_memory) {
            self.stats.memory.fetch_sub(bytes, Ordering::Relaxed);
            return Err(Errno::ENOMEM);
        }
        Ok(Reservation { core: self, bytes })
    }

    /// Turns `reservation` into the charge of memory a driver added to an
    /// existing object (another backing, R5a's `drm_illumos_gem_add_backing`),
    /// with `segments` more scatter/gather entries; the object's
    /// [`GemObject::memory_bytes`] and [`GemObject::dma_segments`] include
    /// them from now on, and its destructor returns them.
    pub fn charge_reserved(&self, reservation: Reservation<'_, O>, segments: usize) {
        core::mem::forget(reservation);
        self.stats.segments.fetch_add(segments as u64, Ordering::Relaxed);
        self.publish();
    }

    /// Makes `obj` a GEM object of this device and returns the first
    /// reference, charging its memory to the budget now: `ENOMEM` if it
    /// does not fit (the object is dropped then, which frees its memory).
    /// Drivers that allocate reserve first and use
    /// [`GemCore::create_reserved`].
    pub fn create(this: &KArc<GemCore<O>>, obj: O, ctx: &Sleepable) -> Result<GemRef<O>, Errno> {
        let r = this.reserve(obj.memory_bytes(), ctx)?;
        Self::create_reserved(this, obj, r, ctx)
    }

    /// Makes `obj` a GEM object of this device and returns the first
    /// reference, taking over `reservation` as the object's charge. `EINVAL`
    /// if the object's size is zero or not a multiple of the page size, or
    /// if its [`GemObject::memory_bytes`] differs from the reservation.
    pub fn create_reserved(
        this: &KArc<GemCore<O>>,
        obj: O,
        reservation: Reservation<'_, O>,
        ctx: &Sleepable,
    ) -> Result<GemRef<O>, Errno> {
        this.housekeeping(ctx);
        let size = obj.size();
        let bytes = obj.memory_bytes() as u64;
        if size == 0 || !(size as u64).is_multiple_of(PAGE_SIZE) || bytes != reservation.bytes {
            return Err(Errno::EINVAL);
        }
        // The charge now belongs to the object; its destructor, which also
        // runs if the allocation below fails, returns it and balances the
        // other counters.
        core::mem::forget(reservation);
        this.stats.objects.fetch_add(1, Ordering::Relaxed);
        this.stats.segments.fetch_add(obj.dma_segments() as u64, Ordering::Relaxed);
        let inner = GemInner {
            common: ObjCommon::new(size),
            core: KArc::clone(this),
            maps: Mutex::new(MapList(KVec::new())),
            resv: Resv::new(),
            dmabuf: AtomicU32::new(0),
            obj,
        };
        let r = KArc::new(inner, ctx).map(GemRef::from_arc);
        this.publish();
        r
    }

    /// Creates a handle to `obj` in file `clone` and returns it. The
    /// reference moves into the handle table; on failure it is dropped.
    pub fn handle_create(&self, clone: CloneId, obj: GemRef<O>, ctx: &Sleepable) -> Result<u32, Errno> {
        let fg = match self.file(clone) {
            Ok(f) => f,
            Err(e) => {
                obj.put(ctx);
                return Err(e);
            }
        };
        if let Err(e) = obj.object().handle_opened(clone, ctx) {
            obj.put(ctx);
            return Err(e);
        }
        let r = {
            let mut names = self.names.lock();
            let mut handles = fg.handles.lock();
            objects::handle_create(&mut names, &mut handles, obj, ctx)
        };
        match r {
            Ok(h) => {
                self.stats.handles.fetch_add(1, Ordering::Relaxed);
                self.publish();
                Ok(h)
            }
            Err((obj, e)) => {
                obj.object().handle_closed(clone, ctx);
                obj.put(ctx);
                Err(e)
            }
        }
    }

    /// Finds a handle of file `clone` whose object satisfies `pred`, without
    /// taking a reference. The file's handle table is locked during the
    /// search, so `pred` must not call into the GEM state.
    pub fn handle_find(&self, clone: CloneId, pred: impl Fn(&GemRef<O>) -> bool) -> Option<u32> {
        let fg = self.file(clone).ok()?;
        let handles = fg.handles.lock();
        handles.iter().find(|(_, r)| pred(r)).map(|(n, _)| n)
    }

    /// Looks up a handle of file `clone` (`ENOENT` if there is none).
    pub fn lookup(&self, clone: CloneId, handle: u32) -> Result<GemRef<O>, Errno> {
        let fg = self.file(clone)?;
        let h = fg.handles.lock();
        objects::lookup(&h, handle)
    }

    /// `GEM_CLOSE` and `MODE_DESTROY_DUMB`: removes a handle (`EINVAL` if
    /// there is none).
    pub fn handle_close(&self, clone: CloneId, handle: u32, ctx: &Sleepable) -> Result<(), Errno> {
        self.housekeeping(ctx);
        let fg = self.file(clone)?;
        let r = objects::handle_delete(&mut fg.handles.lock(), handle)?;
        self.handle_released(clone, r, ctx);
        self.publish();
        Ok(())
    }

    fn handle_released(&self, clone: CloneId, r: GemRef<O>, ctx: &Sleepable) {
        let name_ref = objects::handle_released(&mut self.names.lock(), &r);
        r.object().handle_closed(clone, ctx);
        self.stats.handles.fetch_sub(1, Ordering::Relaxed);
        if name_ref.is_some() {
            self.stats.names.fetch_sub(1, Ordering::Relaxed);
        }
        r.put(ctx);
        put_opt(name_ref, ctx);
    }

    /// `GEM_FLINK`: returns the name of the object behind a handle,
    /// creating one if needed.
    pub fn flink(&self, clone: CloneId, handle: u32, ctx: &Sleepable) -> Result<u32, Errno> {
        self.housekeeping(ctx);
        let r = self.lookup(clone, handle)?;
        let res = {
            let mut names = self.names.lock();
            let had = r.common().name() != 0;
            let n = objects::flink(&mut names, &r, ctx);
            if !had && n.is_ok() {
                self.stats.names.fetch_add(1, Ordering::Relaxed);
            }
            n
        };
        r.put(ctx);
        self.publish();
        res
    }

    /// `GEM_OPEN`: creates a handle in file `clone` for the object named
    /// `name` and returns the handle and the object's size.
    pub fn open_name(&self, clone: CloneId, name: u32, ctx: &Sleepable) -> Result<(u32, u64), Errno> {
        self.housekeeping(ctx);
        let fg = self.file(clone)?;
        // The driver's hook may block, so it runs between two critical
        // sections: the reference taken in the first keeps the object alive
        // if its name goes away meanwhile, as a concurrent GEM_CLOSE of
        // the last other handle could make it.
        let o = objects::open(&self.names.lock(), name)?;
        let size = o.size() as u64;
        if let Err(e) = o.object().handle_opened(clone, ctx) {
            o.put(ctx);
            return Err(e);
        }
        let res = {
            let mut names = self.names.lock();
            let mut handles = fg.handles.lock();
            objects::handle_create(&mut names, &mut handles, o, ctx)
        };
        match res {
            Ok(h) => {
                self.stats.handles.fetch_add(1, Ordering::Relaxed);
                self.publish();
                Ok((h, size))
            }
            Err((o, e)) => {
                o.object().handle_closed(clone, ctx);
                o.put(ctx);
                Err(e)
            }
        }
    }

    /// Returns the object's fake mmap offset, assigning one on first use.
    pub fn mmap_offset(&self, obj: &GemRef<O>, ctx: &Sleepable) -> Result<u64, Errno> {
        let off = obj.common().offset();
        if off != 0 {
            return Ok(off);
        }
        let mut offs = self.offsets.lock();
        let off = obj.common().offset();
        if off != 0 {
            return Ok(off);
        }
        let p = ObjPtr(KArc::as_ptr(&obj.arc));
        let start = offs.insert(obj.size() as u64, p, ctx).map_err(|(_, e)| e)?;
        obj.common().set_offset(start);
        drop(offs);
        self.stats.offsets.fetch_add(1, Ordering::Relaxed);
        self.publish();
        Ok(start)
    }

    // --- mappings ---

    /// The `devmap(9E)` entry point for file `clone`: see the module
    /// documentation. Returns the validated length.
    pub(crate) fn devmap(
        &self,
        clone: CloneId,
        dhp: &DevmapHandle<'_>,
        off: u64,
        len: usize,
        ctx: &Sleepable,
    ) -> Result<usize, Errno> {
        self.housekeeping(ctx);
        let (obj, maplen) = {
            let offs = self.offsets.lock();
            let (p, _) = offs.lookup_exact(off, len as u64).ok_or(Errno::EINVAL)?;
            // SAFETY: an object's destructor removes its entry under this
            // lock before its memory goes away, so the pointer is live.
            let a = unsafe { KArc::upgrade(p.0) }.ok_or(Errno::EINVAL)?;
            let maplen = offsets::page_round(len as u64).and_then(|l| usize::try_from(l).ok());
            (GemRef::from_arc(a), maplen)
        };
        let Some(maplen) = maplen else {
            obj.put(ctx);
            return Err(Errno::EINVAL);
        };
        self.setup(MapOwner::File(clone), obj, dhp, 0, maplen, ctx)?;
        Ok(maplen)
    }

    /// The `devmap(9E)` entry point for the dma-buf file `id` of `obj`: the
    /// object is mapped from offset `off` (page-aligned) for `len` bytes,
    /// which must lie within it (Linux's `dma_buf_mmap_internal`). The
    /// reference moves into the mapping machinery.
    pub(crate) fn devmap_dmabuf(
        &self,
        id: u32,
        obj: GemRef<O>,
        dhp: &DevmapHandle<'_>,
        off: u64,
        len: usize,
        ctx: &Sleepable,
    ) -> Result<usize, Errno> {
        self.housekeeping(ctx);
        let size = obj.size() as u64;
        let maplen = offsets::page_round(len as u64);
        let fits =
            maplen.is_some_and(|l| off.is_multiple_of(PAGE_SIZE) && off.checked_add(l).is_some_and(|e| e <= size));
        let (true, Some(maplen)) = (fits, maplen.and_then(|l| usize::try_from(l).ok())) else {
            obj.put(ctx);
            return Err(Errno::EINVAL);
        };
        self.setup(MapOwner::DmaBuf(id), obj, dhp, off, maplen, ctx)?;
        Ok(maplen)
    }

    /// Describes `maplen` bytes of the object from `obj_off` with
    /// `devmap_umem_setup(9F)` and parks the reference for `devmap_map(9E)`.
    fn setup(
        &self,
        owner: MapOwner,
        obj: GemRef<O>,
        dhp: &DevmapHandle<'_>,
        obj_off: u64,
        maplen: usize,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        // A movable object changes its memory with the mapping list locked.
        let cookie = {
            let _maps = obj.inner().maps.lock();
            obj.object().umem()
        };
        let mut cb = sys::devmap_callback_ctl {
            devmap_rev: sys::DEVMAP_OPS_REV,
            devmap_map: Some(map_cb::<O>),
            devmap_access: Some(access_cb::<O>),
            devmap_dup: Some(dup_cb::<O>),
            devmap_unmap: Some(unmap_cb::<O>),
        };
        let flags = if obj.object().movable() { sys::DEVMAP_ALLOW_REMAP } else { sys::DEVMAP_DEFAULTS };
        let acc = sys::ddi_device_acc_attr_t {
            devacc_attr_version: sys::DDI_DEVICE_ATTR_V0,
            devacc_attr_endian_flags: sys::DDI_NEVERSWAP_ACC,
            devacc_attr_dataorder: sys::DDI_STRICTORDER_ACC,
            devacc_attr_access: 0,
        };
        // SAFETY: called from devmap(9E) with its handle; the callback
        // structure is copied; the cookie belongs to the object, which the
        // reference parked below keeps alive until the mapping holds its
        // own; the range lies within the object (checked by the callers).
        let r = unsafe {
            sys::devmap_umem_setup(
                dhp.as_raw(),
                self.dev.as_raw(),
                &mut cb,
                cookie.as_raw(),
                obj_off as sys::offset_t,
                maplen,
                PROT_ALL,
                flags,
                &acc,
            )
        };
        if r != 0 {
            obj.put(ctx);
            return Err(Errno::ENXIO);
        }
        self.park(dhp.as_raw() as usize, owner, obj, ctx)
    }

    /// `segmap(9E)`: checks an `mmap(2)` of file `clone` before the devmap
    /// framework creates the segment, with the rules `devmap_map(9E)`
    /// applies again later: shared only (`EINVAL`), an object's exact
    /// offset (`EINVAL`), and a file that holds a handle to it (`EACCES`).
    /// Refusing only in `devmap_map(9E)` would leave the segment in the
    /// process until it exits (see `ddi::CharDevice::segmap_check`).
    pub(crate) fn map_check(&self, clone: CloneId, off: u64, len: usize, shared: bool) -> Result<(), Errno> {
        if !shared {
            return Err(Errno::EINVAL);
        }
        let obj = {
            let offs = self.offsets.lock();
            let (p, _) = offs.lookup_exact(off, len as u64).ok_or(Errno::EINVAL)?;
            // SAFETY: as in devmap: the destructor removes the entry under
            // this lock before the memory goes away.
            unsafe { KArc::upgrade(p.0) }.map(GemRef::from_arc).ok_or(Errno::EINVAL)?
        };
        let held = self.file_holds(clone, &obj);
        // Dropping here may destroy the object; no lock is held.
        drop(obj);
        if held { Ok(()) } else { Err(Errno::EACCES) }
    }

    /// Drops what `mmap(2)` calls through the dma-buf file `id` left
    /// parked; its `close(9E)` calls this.
    pub(crate) fn dmabuf_closed(&self, id: u32, ctx: &Sleepable) {
        loop {
            let p = {
                let mut l = self.pending.lock();
                let i = l.iter().position(|p| p.owner == MapOwner::DmaBuf(id));
                i.and_then(|i| l.swap_remove(i))
            };
            let Some(p) = p else { break };
            self.stats.pending.fetch_sub(1, Ordering::Relaxed);
            p.obj.put(ctx);
        }
        self.publish();
    }

    /// Returns the handle file `clone` has for `obj`, creating one if it
    /// has none (the same-device path of `PRIME_FD_TO_HANDLE`, Linux's
    /// prime lookup). Scans the file's handles with the name table and the
    /// file's table locked, as `handle_create` locks them, so that two
    /// concurrent imports of one object give one handle. The reference
    /// moves into the table or is dropped.
    pub fn handle_find_or_create(&self, clone: CloneId, obj: GemRef<O>, ctx: &Sleepable) -> Result<u32, Errno> {
        let fg = match self.file(clone) {
            Ok(f) => f,
            Err(e) => {
                obj.put(ctx);
                return Err(e);
            }
        };
        // The driver's hook may block, so it runs before the lookup; if the
        // file already has a handle, the hook's work is undone.
        if let Err(e) = obj.object().handle_opened(clone, ctx) {
            obj.put(ctx);
            return Err(e);
        }
        let r = {
            let mut names = self.names.lock();
            let mut handles = fg.handles.lock();
            let found = handles.iter().find(|(_, r)| r.same(&obj)).map(|(n, _)| n);
            match found {
                Some(h) => Ok(Err((h, obj))),
                None => objects::handle_create(&mut names, &mut handles, obj, ctx).map(Ok),
            }
        };
        match r {
            Ok(Ok(h)) => {
                self.stats.handles.fetch_add(1, Ordering::Relaxed);
                self.publish();
                Ok(h)
            }
            Ok(Err((h, obj))) => {
                obj.object().handle_closed(clone, ctx);
                obj.put(ctx);
                Ok(h)
            }
            Err((obj, e)) => {
                obj.object().handle_closed(clone, ctx);
                obj.put(ctx);
                Err(e)
            }
        }
    }

    fn park(&self, dhp: usize, owner: MapOwner, obj: GemRef<O>, ctx: &Sleepable) -> Result<(), Errno> {
        let stale = {
            let mut l = self.pending.lock();
            // A handle address can recur once the framework has freed an
            // earlier handle whose mapping failed; the new call replaces it.
            let stale = l.iter().position(|p| p.dhp == dhp).and_then(|i| l.swap_remove(i));
            match l.push(Pending { dhp, owner, thread: current_thread(), obj }, ctx) {
                Ok(()) => {}
                Err(_) => {
                    drop(l);
                    // push only fails if the table would outgrow memory.
                    put_opt(stale.map(|p| p.obj), ctx);
                    return Err(Errno::ENOMEM);
                }
            }
            stale
        };
        match stale {
            Some(p) => p.obj.put(ctx),
            None => {
                self.stats.pending.fetch_add(1, Ordering::Relaxed);
            }
        }
        self.publish();
        Ok(())
    }

    fn unpark(&self, dhp: usize) -> Option<(MapOwner, GemRef<O>)> {
        let p = {
            let mut l = self.pending.lock();
            let i = l.iter().position(|p| p.dhp == dhp)?;
            l.swap_remove(i)?
        };
        self.stats.pending.fetch_sub(1, Ordering::Relaxed);
        self.publish();
        Some((p.owner, p.obj))
    }

    fn file_holds(&self, clone: CloneId, obj: &GemRef<O>) -> bool {
        let Ok(fg) = self.file(clone) else { return false };
        let h = fg.handles.lock();
        h.iter().any(|(_, r)| r.same(obj))
    }

    /// `devmap_map(9E)`.
    fn map(
        &self,
        via: MapOwner,
        dhp: sys::devmap_cookie_t,
        flags: sys::uint_t,
        off: sys::offset_t,
        len: usize,
        ctx: &Sleepable,
    ) -> Result<NonNull<MapPriv<O>>, Errno> {
        let (owner, obj) = self.unpark(dhp as usize).ok_or(Errno::ENXIO)?;
        let refuse = if owner != via {
            Some(Errno::ENXIO)
        } else if flags & MAP_TYPE != MAP_SHARED {
            // A private mapping would have to copy on write; the devmap
            // framework maps the object's pages themselves.
            Some(Errno::EINVAL)
        } else if let MapOwner::File(clone) = via
            && !self.file_holds(clone, &obj)
        {
            Some(Errno::EACCES)
        } else {
            None
        };
        let uoff = u64::try_from(off).ok();
        // The user offset of the object's start: its fake offset through a
        // DRM file, 0 through its dma-buf.
        let base = match via {
            MapOwner::File(_) => obj.common().offset(),
            MapOwner::DmaBuf(_) => 0,
        };
        match (refuse, uoff) {
            (None, Some(uoff)) => record(obj, dhp, uoff, base, len, ctx),
            (e, _) => {
                obj.put(ctx);
                Err(e.unwrap_or(Errno::EINVAL))
            }
        }
    }
}

/// Records a mapping piece set up by `mmap(2)` or `fork(2)` and returns
/// its private data. [`GemObject::mapped`] runs for it; if that fails, the
/// piece is dropped again and the error fails the operation.
fn record<O: GemObject>(
    obj: GemRef<O>,
    dhp: sys::devmap_cookie_t,
    uoff: u64,
    base: u64,
    len: usize,
    ctx: &Sleepable,
) -> Result<NonNull<MapPriv<O>>, Errno> {
    let p = alloc_piece(obj, dhp, uoff, base, len, true, ctx);
    // SAFETY: we own the box until the pointer is handed to the framework.
    let m = unsafe { p.as_ref() };
    let res = match m.obj.as_ref() {
        Some(obj) => {
            let mut maps = obj.inner().maps.lock();
            match maps.0.push(p, ctx) {
                Ok(()) => {
                    let r = obj.object().mapped(&m.mapping(obj), ctx);
                    if r.is_err() {
                        let i = maps.0.iter().position(|q| *q == p);
                        i.and_then(|i| maps.0.swap_remove(i));
                    }
                    r
                }
                Err(e) => Err(e),
            }
        }
        None => Err(Errno::EINVAL),
    };
    if let Err(e) = res {
        // SAFETY: not recorded; we still own the box.
        free_piece(unsafe { KBox::from_raw(p.as_ptr()) }, false, ctx);
        return Err(e);
    }
    count_piece(m, 1);
    Ok(p)
}

/// Records a piece that a partial `munmap(2)` split off. This cannot fail:
/// the framework keeps the piece whatever the callback does, so the piece
/// must hold a reference and have private data. It starts unsettled (see
/// [`Mapping::is_settled`]); if it cannot be listed, it keeps its reference
/// but no movable-object move can redirect it, which is logged.
fn record_split<O: GemObject>(
    obj: GemRef<O>,
    dhp: sys::devmap_cookie_t,
    uoff: u64,
    base: u64,
    len: usize,
    ctx: &Sleepable,
) -> NonNull<MapPriv<O>> {
    let p = alloc_piece(obj, dhp, uoff, base, len, false, ctx);
    // SAFETY: we own the box until the pointer is handed to the framework.
    let m = unsafe { p.as_ref() };
    if let Some(obj) = m.obj.as_ref() {
        let listed = obj.inner().maps.lock().0.push(p, ctx);
        if let Err(e) = listed {
            let dev = obj.inner().core.dev;
            dev.warn(format_args!("mapping piece at {uoff:#x} not listed: {e:?}"));
        }
    }
    count_piece(m, 1);
    p
}

fn alloc_piece<O: GemObject>(
    obj: GemRef<O>,
    dhp: sys::devmap_cookie_t,
    uoff: u64,
    base: u64,
    len: usize,
    settled: bool,
    ctx: &Sleepable,
) -> NonNull<MapPriv<O>> {
    let b = KBox::new(MapPriv { obj: Some(obj), dhp, uoff, base, len, settled: AtomicBool::new(settled) }, ctx);
    // SAFETY: into_raw never returns null.
    unsafe { NonNull::new_unchecked(KBox::into_raw(b)) }
}

fn count_piece<O: GemObject>(m: &MapPriv<O>, n: i64) {
    if let Some(obj) = m.obj.as_ref() {
        let core = &obj.inner().core;
        if n > 0 {
            core.stats.mappings.fetch_add(n as u64, Ordering::Relaxed);
        } else {
            core.stats.mappings.fetch_sub(n.unsigned_abs(), Ordering::Relaxed);
        }
        core.publish();
    }
}

/// Frees a piece and drops its reference; `counted` pieces also leave the
/// statistics.
fn free_piece<O: GemObject>(mut b: KBox<MapPriv<O>>, counted: bool, ctx: &Sleepable) {
    if counted {
        count_piece(&b, -1);
    }
    put_opt(b.obj.take(), ctx);
    drop(b);
}

/// Removes a piece from its object's list and frees it, dropping its
/// reference.
fn unrecord<O: GemObject>(p: NonNull<MapPriv<O>>, ctx: &Sleepable) {
    // SAFETY: a piece from `record` or `record_split`, owned by the
    // framework until now; devmap_unmap(9E) hands it back exactly once.
    let b = unsafe { KBox::from_raw(p.as_ptr()) };
    if let Some(obj) = b.obj.as_ref() {
        let mut maps = obj.inner().maps.lock();
        let i = maps.0.iter().position(|q| *q == p);
        i.and_then(|i| maps.0.swap_remove(i));
    }
    free_piece(b, true, ctx);
}

// --- devmap callbacks -------------------------------------------------------------

/// The device table the callbacks use; see the module documentation.
pub(crate) mod registry {
    use core::ffi::c_void;
    use core::ptr;
    use core::sync::atomic::{AtomicPtr, AtomicUsize, Ordering};

    use super::{GemCore, GemObject, map_cb};
    use crate::minor::MAX_DEVICES;

    static CORES: [AtomicPtr<c_void>; MAX_DEVICES as usize] =
        [const { AtomicPtr::new(ptr::null_mut()) }; MAX_DEVICES as usize];
    static TAGS: [AtomicUsize; MAX_DEVICES as usize] = [const { AtomicUsize::new(0) }; MAX_DEVICES as usize];

    /// A value that differs between object types: the address of the
    /// type's own map callback.
    fn tag<O: GemObject>() -> usize {
        map_cb::<O> as *const () as usize
    }

    /// Records the GEM state of device `index`. The device clears it with
    /// [`remove`] before the state goes away.
    pub(crate) fn insert<O: GemObject>(index: u32, core: &GemCore<O>) -> bool {
        let (Some(c), Some(t)) = (CORES.get(index as usize), TAGS.get(index as usize)) else { return false };
        let claimed = c
            .compare_exchange(
                ptr::null_mut(),
                core as *const GemCore<O> as *mut c_void,
                Ordering::AcqRel,
                Ordering::Relaxed,
            )
            .is_ok();
        // Only the device that claimed the slot sets its tag, so a second
        // registration cannot change the type a live entry is checked
        // against. A lookup before the tag is set finds no device, which is
        // harmless: no file of the device can be open yet.
        if claimed {
            t.store(tag::<O>(), Ordering::Release);
        }
        claimed
    }

    /// Forgets device `index`.
    pub(crate) fn remove(index: u32) {
        if let Some(c) = CORES.get(index as usize) {
            c.store(ptr::null_mut(), Ordering::Release);
        }
    }

    /// Finds the GEM state of device `index`.
    ///
    /// # Safety
    /// The caller must be in a callback of a mapping or file of that device,
    /// which keeps the device attached for the duration of the call.
    pub(crate) unsafe fn get<'a, O: GemObject>(index: u32) -> Option<&'a GemCore<O>> {
        let p = CORES.get(index as usize)?.load(Ordering::Acquire);
        if p.is_null() || TAGS.get(index as usize)?.load(Ordering::Acquire) != tag::<O>() {
            return None;
        }
        // SAFETY: stored by insert from a live GemCore<O> (checked tag); the
        // caller's contract keeps it alive.
        Some(unsafe { &*(p as *const GemCore<O>) })
    }
}

/// `devmap_map(9E)`: runs in the thread that called `mmap(2)`.
unsafe extern "C" fn map_cb<O: GemObject>(
    dhp: sys::devmap_cookie_t,
    dev: sys::dev_t,
    flags: sys::uint_t,
    off: sys::offset_t,
    len: sys::size_t,
    pvtp: *mut *mut c_void,
) -> c_int {
    // SAFETY: devmap_map(9E) runs in user context, in mmap(2).
    let ctx = unsafe { Sleepable::assume() };
    // SAFETY: a device number from the framework.
    let minor = unsafe { sys::getminor(dev) };
    let file = minor::decode(minor).and_then(|d| Some((d.index, MapOwner::File(d.clone?))));
    let dmabuf = || crate::anon_fd::decode(minor).map(|(i, id)| (i, MapOwner::DmaBuf(id)));
    let Some((index, via)) = file.or_else(dmabuf) else {
        return sys::errno::ENXIO;
    };
    // SAFETY: an mmap(2) of an open file of this device is in progress.
    let Some(core) = (unsafe { registry::get::<O>(index) }) else { return sys::errno::ENXIO };
    match core.map(via, dhp, flags, off, len, &ctx) {
        Ok(p) => {
            // SAFETY: the framework passes a writable pointer.
            unsafe { *pvtp = p.as_ptr().cast() };
            0
        }
        Err(e) => e.get(),
    }
}

/// `devmap_dup(9E)`: runs in the thread that called `fork(2)`.
unsafe extern "C" fn dup_cb<O: GemObject>(
    _dhp: sys::devmap_cookie_t,
    pvtp: *mut c_void,
    new_dhp: sys::devmap_cookie_t,
    new_pvtp: *mut *mut c_void,
) -> c_int {
    // SAFETY: devmap_dup(9E) runs in user context, in fork(2).
    let ctx = unsafe { Sleepable::assume() };
    let Some(p) = NonNull::new(pvtp.cast::<MapPriv<O>>()) else { return sys::errno::ENXIO };
    // SAFETY: our private data, recorded and alive while its mapping is.
    let m = unsafe { p.as_ref() };
    let Some(obj) = m.obj.as_ref() else { return sys::errno::ENXIO };
    match record(obj.clone(), new_dhp, m.uoff, m.base, m.len, &ctx) {
        Ok(q) => {
            // SAFETY: the framework passes a writable pointer.
            unsafe { *new_pvtp = q.as_ptr().cast() };
            0
        }
        Err(e) => e.get(),
    }
}

/// `devmap_unmap(9E)`: runs in the thread that unmaps or exits.
#[allow(clippy::too_many_arguments, reason = "the shape of devmap_unmap(9E)")]
unsafe extern "C" fn unmap_cb<O: GemObject>(
    _dhp: sys::devmap_cookie_t,
    pvtp: *mut c_void,
    off: sys::offset_t,
    len: sys::size_t,
    new_dhp1: sys::devmap_cookie_t,
    new_pvtp1: *mut *mut c_void,
    new_dhp2: sys::devmap_cookie_t,
    new_pvtp2: *mut *mut c_void,
) {
    // SAFETY: devmap_unmap(9E) runs in user context, in munmap(2) or exit.
    let ctx = unsafe { Sleepable::assume() };
    let Some(p) = NonNull::new(pvtp.cast::<MapPriv<O>>()) else { return };
    // SAFETY: our private data; the framework hands it back once.
    let m = unsafe { p.as_ref() };
    let (uoff, base, mlen) = (m.uoff, m.base, m.len as u64);
    let off = off as u64;
    let end = off.saturating_add(len as u64);
    if let Some(obj) = m.obj.as_ref() {
        // The pieces that remain when the middle or one end is unmapped.
        // The framework keeps them whatever happens here (new_dhp1 is the
        // old handle itself, new_dhp2 a copy that still points at `p`), so
        // each gets private data with its own reference before `p` goes.
        let pieces = [
            (new_dhp1, new_pvtp1, uoff, off.saturating_sub(uoff)),
            (new_dhp2, new_pvtp2, end, uoff.saturating_add(mlen).saturating_sub(end)),
        ];
        for (ndhp, npvtp, start, plen) in pieces {
            if ndhp.is_null() || npvtp.is_null() {
                continue;
            }
            let q = record_split(obj.clone(), ndhp, start, base, plen as usize, &ctx);
            // SAFETY: the framework passes a writable pointer.
            unsafe { *npvtp = q.as_ptr().cast() };
        }
    }
    unrecord(p, &ctx);
}

/// `devmap_access(9E)`: runs in the faulting thread.
unsafe extern "C" fn access_cb<O: GemObject>(
    dhp: sys::devmap_cookie_t,
    pvtp: *mut c_void,
    off: sys::offset_t,
    len: sys::size_t,
    type_: sys::uint_t,
    rw: sys::uint_t,
) -> c_int {
    let f = Fault { dhp, pvtp, off, len, type_, rw };
    let Some(p) = NonNull::new(pvtp.cast::<MapPriv<O>>()) else { return f.default_access() };
    // SAFETY: our private data, alive while its mapping is.
    let m = unsafe { p.as_ref() };
    let Some(obj) = m.obj.as_ref() else { return f.default_access() };
    let core = &obj.inner().core;
    core.stats.faults.fetch_add(1, Ordering::Relaxed);
    if !m.settled.load(Ordering::Acquire) {
        // The first fault of a piece split off by munmap(2): the framework
        // has finished it (the fault holds the address space lock that the
        // munmap held), so its geometry is final. See Mapping::is_settled.
        // SAFETY: devmap_access(9E) runs in the faulting user thread, which
        // may block.
        let ctx = unsafe { Sleepable::assume() };
        let maps = obj.inner().maps.lock();
        if !m.settled.load(Ordering::Acquire) {
            let r = obj.object().mapped(&Mapping { settled: true, ..m.mapping(obj) }, &ctx);
            if let Err(e) = r {
                drop(maps);
                core.dev.warn(format_args!("mapping piece at {:#x} not settled: {e:?}", m.uoff));
                core.publish();
                return sys::errno::EIO;
            }
            m.settled.store(true, Ordering::Release);
        }
        drop(maps);
    }
    let r = obj.object().access(&m.mapping(obj), &f);
    // devmap_access(9E) runs in the faulting thread, where the kstat's
    // adaptive mutex may be taken.
    core.publish();
    r
}
