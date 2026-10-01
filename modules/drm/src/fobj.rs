// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! GEM objects of the facade: a driver's `struct drm_gem_object` (embedded
//! in its own buffer structure) and the core object behind it.
//!
//! The core object ([`GemRef<FObj>`]) owns the reference count, the
//! handles, names, offsets, mappings and the reservation; the C structure
//! carries the public fields and, in `__core[0]`, the address of the core
//! object, without a reference of its own: every reference a C caller holds
//! (the one `drm_gem_object_init` returns, those of `drm_gem_object_get`
//! and `drm_gem_object_lookup`) is a count of the core object. When the
//! last one goes, the core releases its state and calls the driver's
//! `funcs->free`, which frees the C structure.
//!
//! Objects that the core makes itself (the import of another device's
//! dma-buf) have a C structure too, allocated here
//! ([`ImportBox`]), so that a driver can look them up like its own.

use alloc::boxed::Box;
use core::ffi::{c_int, c_void};
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicPtr, Ordering};

use ddi::sys;
use ddi::{Errno, Sleepable};
use drm_core::gem::{Fault, GemObject, GemRef, Mapping, SysMem, UmemCookie};
use drm_core::ksync::KMutex;
use drm_core::kvec::KVec;
use drm_core::minor::CloneId;
use drm_facade_sys as f;

use crate::devcore::DevCore;

/// The most backings an object can have (backing 0 and three more).
pub(crate) const MAX_BACKINGS: usize = 4;

/// The core side of a facade GEM object.
pub(crate) struct FObj {
    /// The C structure, inside the driver's (or an [`ImportBox`]'s)
    /// allocation; valid until `release` calls `funcs->free`.
    base: NonNull<f::drm_gem_object>,
    /// The device's state, for its files and its driver's table.
    core: NonNull<DevCore>,
    /// The object's callbacks.
    funcs: &'static f::drm_gem_object_funcs,
    size: usize,
    /// Backing 0 (from `drm_gem_object_init`) and the backings added later.
    mem: KMutex<KVec<SysMem>>,
    /// The bytes and DMA segments of `mem`, read without the lock.
    bytes: core::sync::atomic::AtomicUsize,
    segments: core::sync::atomic::AtomicUsize,
    /// For an import: the exporting object, owned (a boxed reference).
    imported: AtomicPtr<GemRef<FObj>>,
    /// Set once the object is fully made ([`FObj::arm`]): only then does
    /// its release free the C structure. A creation that fails half-way
    /// leaves the structure to its maker, as Linux's `drm_gem_object_init`
    /// leaves it to the driver.
    armed: core::sync::atomic::AtomicBool,
}

// SAFETY: the C structure is only touched through the facade's functions
// and the driver's callbacks, which the facade's contract makes
// thread-safe; everything else is locked or atomic.
unsafe impl Send for FObj {}
// SAFETY: as above.
unsafe impl Sync for FObj {}

/// A fallback page for a mapping whose object has no memory to show (a
/// driver bug: `illumos_umem` returned NULL). Mapping it shows zeros or
/// fails, instead of following a NULL cookie.
static FALLBACK: AtomicPtr<c_void> = AtomicPtr::new(ptr::null_mut());

fn fallback_cookie() -> UmemCookie {
    let mut p = FALLBACK.load(Ordering::Acquire);
    if p.is_null() {
        let mut cookie: sys::ddi_umem_cookie_t = ptr::null_mut();
        // SAFETY: one zeroed page; the cookie is kept for the module's life
        // (it is never freed: a mapping may still refer to it).
        unsafe {
            sys::ddi_umem_alloc(sys::ptob(1) as usize, sys::DDI_UMEM_SLEEP, &mut cookie);
        }
        p = match FALLBACK.compare_exchange(ptr::null_mut(), cookie, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => cookie,
            Err(other) => {
                // SAFETY: the cookie just allocated, unused.
                unsafe { sys::ddi_umem_free(cookie) };
                other
            }
        };
    }
    // SAFETY: a live cookie from ddi_umem_alloc.
    unsafe { UmemCookie::from_raw(NonNull::new_unchecked(p)) }
}

impl FObj {
    /// A new core object for the C structure `base` of device `core`.
    pub(crate) fn new(
        base: NonNull<f::drm_gem_object>,
        core: NonNull<DevCore>,
        funcs: &'static f::drm_gem_object_funcs,
        size: usize,
        mem: Option<SysMem>,
        imported: Option<GemRef<FObj>>,
        ctx: &Sleepable,
    ) -> Result<FObj, Errno> {
        let (bytes, segments) = mem.as_ref().map(|m| (m.size(), m.sg_len())).unwrap_or((0, 0));
        let mut list = KVec::new();
        if let Some(m) = mem {
            list.push(m, ctx)?;
        }
        let imported = match imported {
            Some(r) => Box::into_raw(Box::new(r)),
            None => ptr::null_mut(),
        };
        Ok(FObj {
            base,
            core,
            funcs,
            size,
            mem: KMutex::new(list),
            bytes: core::sync::atomic::AtomicUsize::new(bytes),
            segments: core::sync::atomic::AtomicUsize::new(segments),
            imported: AtomicPtr::new(imported),
            armed: core::sync::atomic::AtomicBool::new(false),
        })
    }

    /// Hands the C structure to the object: from now on the last reference
    /// frees it through `funcs->free`.
    pub(crate) fn arm(&self) {
        self.armed.store(true, Ordering::Release);
    }

    /// The C structure.
    pub(crate) fn base(&self) -> *mut f::drm_gem_object {
        self.base.as_ptr()
    }

    pub(crate) fn core(&self) -> &DevCore {
        // SAFETY: an object keeps its device's GEM state alive, and the
        // device state lives until after the device unregisters, which it
        // cannot while objects remain.
        unsafe { self.core.as_ref() }
    }

    /// Records the exporting object of an object a driver made in its
    /// `gem_prime_import` callback; the first record wins.
    pub(crate) fn set_imported(&self, exporter: GemRef<FObj>) {
        let b = Box::into_raw(Box::new(exporter));
        if self.imported.compare_exchange(ptr::null_mut(), b, Ordering::AcqRel, Ordering::Acquire).is_err() {
            // SAFETY: the box just made, never shared.
            drop(unsafe { Box::from_raw(b) });
        }
    }

    fn exporter(&self) -> Option<&GemRef<FObj>> {
        let p = self.imported.load(Ordering::Acquire);
        // SAFETY: set once from a box that lives until drop.
        if p.is_null() { None } else { Some(unsafe { &*p }) }
    }

    /// Adds a backing of the object's size (`drm_illumos_gem_add_backing`)
    /// and returns its index.
    pub(crate) fn add_backing(&self, mem: SysMem, ctx: &Sleepable) -> Result<usize, Errno> {
        let (b, s) = (mem.size(), mem.sg_len());
        let mut l = self.mem.lock();
        if l.is_empty() || l.len() >= MAX_BACKINGS {
            return Err(Errno::EINVAL);
        }
        l.push(mem, ctx)?;
        self.bytes.fetch_add(b, Ordering::Relaxed);
        self.segments.fetch_add(s, Ordering::Relaxed);
        Ok(l.len() - 1)
    }

    /// Runs `f` on backing `i`.
    pub(crate) fn with_backing<R>(&self, i: usize, f: impl FnOnce(&SysMem) -> R) -> Option<R> {
        let l = self.mem.lock();
        l.get(i).map(f)
    }

    fn file(&self, c: CloneId) -> Option<*mut f::drm_file> {
        self.core().file(c).map(|p| p.as_ptr())
    }
}

impl Drop for FObj {
    fn drop(&mut self) {
        if !self.armed.load(Ordering::Acquire) && ptr::eq(self.funcs, &IMPORT_FUNCS) {
            // A core-made import whose creation failed: its box is ours.
            // SAFETY: the box made by import_object, never freed (release
            // frees only armed objects).
            drop(unsafe { Box::from_raw(self.base.as_ptr() as *mut ImportBox) });
        }
        let p = self.imported.swap(ptr::null_mut(), Ordering::AcqRel);
        if !p.is_null() {
            // SAFETY: the box set by new or set_imported; dropped in thread
            // context (objects are destroyed there).
            drop(unsafe { Box::from_raw(p) });
        }
    }
}

impl GemObject for FObj {
    fn movable(&self) -> bool {
        self.funcs.illumos_movable
    }

    fn size(&self) -> usize {
        self.size
    }

    fn memory_bytes(&self) -> usize {
        self.bytes.load(Ordering::Relaxed)
    }

    fn dma_segments(&self) -> usize {
        self.segments.load(Ordering::Relaxed)
    }

    fn umem(&self) -> UmemCookie {
        if let Some(cb) = self.funcs.illumos_umem {
            // SAFETY: the driver's callback with its live object.
            let c = unsafe { cb(self.base()) };
            return match NonNull::new(c) {
                // SAFETY: the driver promises a live cookie of the object.
                Some(p) => unsafe { UmemCookie::from_raw(p) },
                None => {
                    self.core().dip.warn(format_args!("illumos_umem returned NULL; mapping a fallback page"));
                    fallback_cookie()
                }
            };
        }
        if let Some(c) = self.with_backing(0, |m| m.umem_cookie()) {
            return c;
        }
        if let Some(e) = self.exporter() {
            return e.object().umem();
        }
        fallback_cookie()
    }

    fn mapped(&self, m: &Mapping, _ctx: &Sleepable) -> Result<(), Errno> {
        let Some(cb) = self.funcs.illumos_mapped else { return Ok(()) };
        let map = m as *const Mapping as *mut f::drm_illumos_mapping;
        // SAFETY: the callback gets the live object and the mapping, valid
        // during the call.
        let r = unsafe { cb(self.base(), map) };
        crate::devcore::from_neg(r)
    }

    fn access(&self, m: &Mapping, fault: &Fault) -> c_int {
        let Some(cb) = self.funcs.illumos_access else { return fault.default_access() };
        let map = m as *const Mapping as *mut f::drm_illumos_mapping;
        let flt = fault as *const Fault as *mut f::drm_illumos_fault;
        // SAFETY: as in mapped.
        unsafe { cb(self.base(), map, flt) }
    }

    fn handle_opened(&self, file: CloneId, _ctx: &Sleepable) -> Result<(), Errno> {
        let Some(cb) = self.funcs.open else { return Ok(()) };
        let Some(fp) = self.file(file) else { return Ok(()) };
        // SAFETY: the driver's callback with live structures.
        crate::devcore::from_neg(unsafe { cb(self.base(), fp) })
    }

    fn handle_closed(&self, file: CloneId, _ctx: &Sleepable) {
        let Some(cb) = self.funcs.close else { return };
        let Some(fp) = self.file(file) else { return };
        // SAFETY: as above.
        unsafe { cb(self.base(), fp) };
    }

    fn release(&mut self, _ctx: &Sleepable) {
        if !self.armed.load(Ordering::Acquire) {
            return;
        }
        // The core has released everything that refers to the object; the
        // C structure goes now. Its tail no longer names a live object.
        // SAFETY: the C structure is live until this call.
        unsafe {
            (*self.base()).__core[0] = 0;
            (*self.base()).__core[1] = 0;
        }
        if let Some(free) = self.funcs.free {
            // SAFETY: the driver's callback; it frees its structure.
            unsafe { free(self.base()) };
        }
    }

    fn imported_from(&self) -> Option<&GemRef<Self>> {
        self.exporter()
    }
}

/// A C structure the core makes for an import of another device's
/// dma-buf when the driver has no `gem_prime_import`: the object, its
/// attachment and a handle for the dma-buf, freed by [`import_free`].
#[repr(C)]
pub(crate) struct ImportBox {
    pub(crate) obj: f::drm_gem_object,
    pub(crate) attach: f::dma_buf_attachment,
}

unsafe extern "C" fn import_free(obj: *mut f::drm_gem_object) {
    // SAFETY: obj is the first field of an ImportBox made by
    // import_object, freed once.
    drop(unsafe { Box::from_raw(obj as *mut ImportBox) });
}

static IMPORT_FUNCS: f::drm_gem_object_funcs = f::drm_gem_object_funcs {
    free: Some(import_free),
    open: None,
    close: None,
    illumos_umem: None,
    illumos_mapped: None,
    illumos_access: None,
    illumos_movable: false,
};

/// Makes an object of device `core` that shows `exporter`'s memory (the
/// default import of another device's dma-buf) and returns its first
/// reference.
pub(crate) fn import_object(core: &DevCore, exporter: &GemRef<FObj>, ctx: &Sleepable) -> Result<GemRef<FObj>, Errno> {
    let dev = core.device().ok_or(Errno::ENODEV)?;
    let gem = dev.gem().ok_or(Errno::ENODEV)?;
    // SAFETY: plain data; zero is valid for every field.
    let b: Box<ImportBox> = Box::new(unsafe { core::mem::zeroed() });
    let b = Box::into_raw(b);
    // SAFETY: just allocated.
    let base = unsafe { NonNull::new_unchecked(ptr::addr_of_mut!((*b).obj)) };
    let size = exporter.object().size();
    let fobj = match FObj::new(base, NonNull::from(core), &IMPORT_FUNCS, size, None, Some(exporter.clone()), ctx) {
        Ok(o) => o,
        Err(e) => {
            // SAFETY: never shared.
            drop(unsafe { Box::from_raw(b) });
            return Err(e);
        }
    };
    // From here the unarmed object frees the box if its creation fails.
    let r = drm_core::gem::GemCore::create(gem, fobj, ctx)?;
    // SAFETY: the box is live; fill the public fields.
    unsafe {
        init_public(base.as_ptr(), core, &r);
        (*b).attach.dev = core.dip.as_raw() as *mut f::dev_info_t;
        (*b).obj.import_attach = ptr::addr_of_mut!((*b).attach);
        (*b).obj.funcs = &IMPORT_FUNCS;
    }
    r.object().arm();
    Ok(r)
}

/// Fills the public fields of a C object the core just made `r` for.
///
/// # Safety
/// `obj` points to the object's C structure, which no other thread uses
/// yet.
pub(crate) unsafe fn init_public(obj: *mut f::drm_gem_object, core: &DevCore, r: &GemRef<FObj>) {
    // SAFETY: the caller's contract.
    unsafe {
        (*obj).dev = core.ddev();
        (*obj).size = r.object().size();
        (*obj).resv = ptr::addr_of_mut!((*obj)._resv);
        (*obj)._resv.__core[0] = r.resv() as *const _ as u64;
        let l = ptr::addr_of_mut!((*obj).gpuva.list);
        (*l).next = l;
        (*l).prev = l;
        let l = ptr::addr_of_mut!((*obj).lru_node);
        (*l).next = l;
        (*l).prev = l;
        let p = r.as_raw().as_ptr() as u64;
        (*obj).__core[0] = p;
        (*obj).__core[1] = p ^ crate::MAGIC_GEM ^ (obj as u64);
    }
}

/// A new reference to the core object of a C structure the facade set up,
/// or `None` for anything else.
///
/// # Safety
/// `obj` is NULL or points to a `struct drm_gem_object` with a live
/// reference the caller holds.
pub(crate) unsafe fn gem_ref(obj: *const f::drm_gem_object) -> Option<GemRef<FObj>> {
    // SAFETY: the caller's contract.
    let p = unsafe { raw_of(obj) }?;
    // SAFETY: a live object (the caller holds a reference), of FObj.
    Some(unsafe { GemRef::<FObj>::clone_from_raw(p) })
}

/// The core object's address of a C structure the facade set up.
///
/// # Safety
/// As [`gem_ref`].
pub(crate) unsafe fn raw_of(obj: *const f::drm_gem_object) -> Option<NonNull<c_void>> {
    if obj.is_null() {
        return None;
    }
    // SAFETY: the caller's contract.
    let (p, check) = unsafe { ((*obj).__core[0], (*obj).__core[1]) };
    if p == 0 || p ^ crate::MAGIC_GEM ^ (obj as u64) != check {
        return None;
    }
    NonNull::new(p as *mut c_void)
}
