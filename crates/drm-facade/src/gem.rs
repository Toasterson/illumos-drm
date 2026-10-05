// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! GEM objects: a driver's object is a [`Gem<T>`], the facade's
//! `struct drm_gem_object` followed by the driver's data `T`, handled
//! through counted references ([`GemRef<T>`]).

use alloc::boxed::Box;
use core::ffi::{c_int, c_void};
use core::ptr::{self, NonNull};

use ddi::{DmaCookie, Errno};
use drm_facade_sys as sys;

use crate::device::{Dev, Driver, File};
use crate::fence::Fence;
use crate::{check, neg};

/// How a fence uses a buffer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Usage {
    /// Kernel memory management.
    Kernel,
    /// Writing the buffer (implicit synchronisation).
    Write,
    /// Reading the buffer.
    Read,
    /// Only keeping the buffer alive.
    Bookkeep,
}

impl Usage {
    fn raw(self) -> sys::dma_resv_usage {
        match self {
            Usage::Kernel => sys::DMA_RESV_USAGE_KERNEL,
            Usage::Write => sys::DMA_RESV_USAGE_WRITE,
            Usage::Read => sys::DMA_RESV_USAGE_READ,
            Usage::Bookkeep => sys::DMA_RESV_USAGE_BOOKKEEP,
        }
    }
}

/// A piece of a user mapping, valid during a callback.
pub struct Mapping<'a> {
    raw: *mut sys::drm_illumos_mapping,
    _call: core::marker::PhantomData<&'a ()>,
}

impl Mapping<'_> {
    /// The devmap handle, for `devmap_umem_remap`.
    pub fn cookie(&self) -> sys::devmap_cookie_t {
        // SAFETY: a mapping of the running callback.
        unsafe { sys::drm_illumos_mapping_cookie(self.raw) }
    }
    /// The piece's offset in the object.
    pub fn object_offset(&self) -> u64 {
        // SAFETY: as above.
        unsafe { sys::drm_illumos_mapping_object_offset(self.raw) }
    }
    /// The piece's fake offset.
    pub fn user_offset(&self) -> u64 {
        // SAFETY: as above.
        unsafe { sys::drm_illumos_mapping_user_offset(self.raw) }
    }
    /// The piece's length in bytes.
    pub fn len(&self) -> usize {
        // SAFETY: as above.
        unsafe { sys::drm_illumos_mapping_len(self.raw) }
    }
    /// Whether the piece is empty.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    /// Whether the framework has finished setting the piece up.
    pub fn is_settled(&self) -> bool {
        // SAFETY: as above.
        unsafe { sys::drm_illumos_mapping_is_settled(self.raw) }
    }
    /// `devmap_unload(9F)` of the piece; thread context.
    pub fn unload(&self) -> Result<(), c_int> {
        // SAFETY: as above.
        match unsafe { sys::drm_illumos_mapping_unload(self.raw) } {
            0 => Ok(()),
            e => Err(e),
        }
    }
}

/// A fault on an unloaded mapping, valid during the access callback.
pub struct Fault<'a> {
    raw: *mut sys::drm_illumos_fault,
    _call: core::marker::PhantomData<&'a ()>,
}

impl Fault<'_> {
    /// `devmap_load(9F)` of the faulting range.
    pub fn load(&self) -> c_int {
        // SAFETY: a fault of the running callback.
        unsafe { sys::drm_illumos_fault_load(self.raw) }
    }
    /// `devmap_default_access(9F)`.
    pub fn default_access(&self) -> c_int {
        // SAFETY: as above.
        unsafe { sys::drm_illumos_fault_default_access(self.raw) }
    }
}

/// A driver's buffer-object type: the data that follows the facade's
/// `drm_gem_object` in a [`Gem<Self>`], and the callbacks.
pub trait GemObject: Send + Sync + Sized + 'static {
    /// Mappings may be unloaded and refaulted (`illumos_movable`).
    const MOVABLE: bool = false;
    /// [`GemObject::umem`] chooses the memory mappings show.
    const CUSTOM_UMEM: bool = false;

    /// The `ddi_umem` cookie mappings show now (with [`Self::CUSTOM_UMEM`]).
    fn umem(gem: &Gem<Self>) -> sys::ddi_umem_cookie_t {
        gem.umem(0)
    }
    /// A user mapping piece was set up.
    fn mapped(gem: &Gem<Self>, m: &Mapping<'_>) -> Result<(), Errno> {
        let _ = (gem, m);
        Ok(())
    }
    /// A fault on an unloaded piece (`devmap_access(9E)`).
    fn access(gem: &Gem<Self>, m: &Mapping<'_>, f: &Fault<'_>) -> c_int {
        let _ = (gem, m);
        f.default_access()
    }
    /// `file` gets its first handle to the object; an error refuses it.
    /// Once per file and object, not per handle (see `funcs->open` in
    /// `include/drm/drm_gem.h`): a second handle, or the import of an
    /// object the file holds, causes no call.
    fn open(gem: &Gem<Self>, file: &File) -> Result<(), Errno> {
        let _ = (gem, file);
        Ok(())
    }
    /// The last handle on `file` was closed, or the handle [`GemObject::open`]
    /// was just called for could not be made.
    fn close(gem: &Gem<Self>, file: &File) {
        let _ = (gem, file);
    }
    /// The object is being destroyed (thread context), before its memory
    /// goes: tell the device to stop using it. Return `false` if the
    /// device may still use the memory (it did not answer): the core then
    /// never frees it (`illumos_release`).
    fn release(gem: &mut Gem<Self>) -> bool {
        let _ = gem;
        true
    }
}

/// A GEM object of the driver: the facade's structure and the driver's
/// data after it. Allocated by [`GemRef::new`], freed by the core's last
/// reference through `funcs->free`.
#[repr(C)]
pub struct Gem<T: GemObject> {
    /// Changed by the core through raw pointers while references exist.
    base: core::cell::UnsafeCell<sys::drm_gem_object>,
    /// The object's type, which [`is_type`] checks.
    vt: &'static GemVt,
    data: T,
}

/// The start of every [`Gem`], whatever its type.
#[repr(C)]
struct GemHead {
    base: core::cell::UnsafeCell<sys::drm_gem_object>,
    vt: &'static GemVt,
}

/// What the facade knows of an object type at run time.
struct GemVt {
    /// Frees a `Gem<T>`.
    drop: unsafe fn(*mut sys::drm_gem_object),
    /// `TypeId::of::<T>`.
    type_id: fn() -> core::any::TypeId,
}

trait HasVt: GemObject {
    const VT: GemVt;
}

impl<T: GemObject> HasVt for T {
    const VT: GemVt = GemVt { drop: drop_gem::<T>, type_id: core::any::TypeId::of::<T> };
}

/// # Safety
/// `obj` is the structure of a boxed `Gem<T>` whose last reference went.
unsafe fn drop_gem<T: GemObject>(obj: *mut sys::drm_gem_object) {
    // SAFETY: the caller's contract.
    drop(unsafe { Box::from_raw(obj as *mut Gem<T>) });
}

// SAFETY: the C structure is changed only by the facade; T is Send+Sync.
unsafe impl<T: GemObject> Send for Gem<T> {}
// SAFETY: as above.
unsafe impl<T: GemObject> Sync for Gem<T> {}

impl<T: GemObject> core::ops::Deref for Gem<T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.data
    }
}

impl<T: GemObject> Gem<T> {
    /// The C object.
    pub fn raw(&self) -> *mut sys::drm_gem_object {
        self.base.get()
    }
    /// The size in bytes.
    pub fn size(&self) -> usize {
        // SAFETY: set at creation, never changed.
        unsafe { (*self.raw()).size }
    }
    /// Backing `i`'s `ddi_umem` cookie, or NULL.
    pub fn umem(&self, i: u32) -> sys::ddi_umem_cookie_t {
        // SAFETY: a live object.
        unsafe { sys::drm_illumos_gem_umem(self.raw(), i) }
    }
    /// Backing `i`'s kernel address, or NULL.
    pub fn vaddr(&self, i: u32) -> *mut u8 {
        // SAFETY: a live object.
        unsafe { sys::drm_illumos_gem_vaddr(self.raw(), i).cast() }
    }
    /// The number of DMA cookies of backing `i`.
    pub fn sg_count(&self, i: u32) -> usize {
        // SAFETY: a live object.
        unsafe { sys::drm_illumos_gem_sg_count(self.raw(), i) }
    }
    /// DMA cookie `j` of backing `i`.
    pub fn sg_cookie(&self, i: u32, j: usize) -> Option<DmaCookie> {
        let mut c: ddi::sys::ddi_dma_cookie_t = unsafe { core::mem::zeroed() };
        // SAFETY: a live object and a cookie of the DDI's layout.
        let r =
            unsafe { sys::drm_illumos_gem_sg_cookie(self.raw(), i, j, &mut c as *mut _ as *mut sys::ddi_dma_cookie_t) };
        // SAFETY: both members of the union are integers.
        (r == 0).then_some(DmaCookie { address: unsafe { c._dmu._dmac_ll }, size: c.dmac_size })
    }
    /// The DMA cookies of backing `i`.
    pub fn sg_iter(&self, i: u32) -> impl Iterator<Item = DmaCookie> + '_ {
        (0..self.sg_count(i)).filter_map(move |j| self.sg_cookie(i, j))
    }
}

unsafe extern "C" fn release_thunk<T: GemObject>(obj: *mut sys::drm_gem_object) -> bool {
    // SAFETY: the last reference is gone: nothing else uses the object.
    T::release(unsafe { &mut *(obj as *mut Gem<T>) })
}

/// The `free` callback of every object type of the driver: one function,
/// so that its address tells the driver's objects from others.
unsafe extern "C" fn free_any(obj: *mut sys::drm_gem_object) {
    // SAFETY: the structure is the first field of a boxed Gem<T> made by
    // GemRef::new, which starts with a GemHead; the core calls free once,
    // in thread context.
    unsafe {
        let vt = (*(obj as *const GemHead)).vt;
        (vt.drop)(obj)
    }
}

unsafe extern "C" fn open_thunk<T: GemObject>(obj: *mut sys::drm_gem_object, file: *mut sys::drm_file) -> c_int {
    // SAFETY: a live object of this type and a live file.
    let (g, f) = unsafe { (&*(obj as *const Gem<T>), File::from_raw(file)) };
    let Some(f) = f else { return 0 };
    match T::open(g, &f) {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

unsafe extern "C" fn close_thunk<T: GemObject>(obj: *mut sys::drm_gem_object, file: *mut sys::drm_file) {
    // SAFETY: as above.
    let (g, f) = unsafe { (&*(obj as *const Gem<T>), File::from_raw(file)) };
    if let Some(f) = f {
        T::close(g, &f);
    }
}

unsafe extern "C" fn umem_thunk<T: GemObject>(obj: *mut sys::drm_gem_object) -> sys::ddi_umem_cookie_t {
    // SAFETY: as above.
    T::umem(unsafe { &*(obj as *const Gem<T>) })
}

unsafe extern "C" fn mapped_thunk<T: GemObject>(
    obj: *mut sys::drm_gem_object,
    map: *mut sys::drm_illumos_mapping,
) -> c_int {
    // SAFETY: as above; the mapping is valid during the call.
    let g = unsafe { &*(obj as *const Gem<T>) };
    match T::mapped(g, &Mapping { raw: map, _call: core::marker::PhantomData }) {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

unsafe extern "C" fn access_thunk<T: GemObject>(
    obj: *mut sys::drm_gem_object,
    map: *mut sys::drm_illumos_mapping,
    fault: *mut sys::drm_illumos_fault,
) -> c_int {
    // SAFETY: as above.
    let g = unsafe { &*(obj as *const Gem<T>) };
    let m = Mapping { raw: map, _call: core::marker::PhantomData };
    T::access(g, &m, &Fault { raw: fault, _call: core::marker::PhantomData })
}

/// The facade's callbacks of an object type.
trait Funcs: GemObject {
    const FUNCS: sys::drm_gem_object_funcs;
}

impl<T: GemObject> Funcs for T {
    const FUNCS: sys::drm_gem_object_funcs = sys::drm_gem_object_funcs {
        free: Some(free_any),
        open: Some(open_thunk::<T>),
        close: Some(close_thunk::<T>),
        illumos_release: Some(release_thunk::<T>),
        illumos_umem: if T::CUSTOM_UMEM { Some(umem_thunk::<T>) } else { None },
        illumos_mapped: Some(mapped_thunk::<T>),
        illumos_access: Some(access_thunk::<T>),
        illumos_movable: T::MOVABLE,
        illumos_close_every_handle: false,
    };
}

/// Whether `obj` is an object of type `T`: one of this driver's objects
/// (its free callback is this module's [`free_any`]) whose type tag is
/// `T`'s `TypeId`. Function addresses are compared only for the one
/// non-generic function; the type itself is told by its `TypeId`, since
/// distinct generic instances may share an address.
fn is_type<T: GemObject>(obj: *const sys::drm_gem_object) -> bool {
    // SAFETY: a live object the caller holds.
    let funcs = unsafe { (*obj).funcs };
    if funcs.is_null() {
        return false;
    }
    // SAFETY: an object's funcs live as long as it.
    let free = unsafe { (*funcs).free };
    let any: unsafe extern "C" fn(*mut sys::drm_gem_object) = free_any;
    if free.map(|f| f as usize) != Some(any as usize) {
        return false;
    }
    // SAFETY: an object of this driver starts with a GemHead.
    let vt = unsafe { (*(obj as *const GemHead)).vt };
    (vt.type_id)() == core::any::TypeId::of::<T>()
}

/// A counted reference to an object of type `T`.
pub struct GemRef<T: GemObject> {
    p: NonNull<Gem<T>>,
}

// SAFETY: references may move between threads and be dropped anywhere
// (drm_gem_object_put defers the release out of interrupt context).
unsafe impl<T: GemObject> Send for GemRef<T> {}
// SAFETY: as above.
unsafe impl<T: GemObject> Sync for GemRef<T> {}

impl<T: GemObject> Clone for GemRef<T> {
    fn clone(&self) -> GemRef<T> {
        // SAFETY: a live object.
        unsafe { sys::drm_gem_object_get(self.raw()) };
        GemRef { p: self.p }
    }
}

impl<T: GemObject> Drop for GemRef<T> {
    /// Drops the reference without assuming that the context may block
    /// (a `timeout(9F)` handler cannot be told from a thread): a last
    /// reference is released by a task queue. [`GemRef::put`] releases it
    /// at once where blocking is proven.
    fn drop(&mut self) {
        // SAFETY: this reference's count; any context.
        unsafe { sys::drm_illumos_gem_object_put_deferred(self.raw()) };
    }
}

impl<T: GemObject> core::ops::Deref for GemRef<T> {
    type Target = Gem<T>;
    fn deref(&self) -> &Gem<T> {
        // SAFETY: the reference keeps the object alive.
        unsafe { self.p.as_ref() }
    }
}

impl<T: GemObject> GemRef<T> {
    /// Makes an object of `size` bytes (a multiple of the page size) of
    /// device `dev` with `data`: with system memory (`drm_gem_object_init`,
    /// backing 0) if `memory`, else without (`drm_gem_private_object_init`;
    /// the type must have [`GemObject::CUSTOM_UMEM`]). Thread context.
    pub fn new<D: Driver>(dev: &Dev<D>, size: usize, data: T, memory: bool) -> Result<GemRef<T>, Errno> {
        // SAFETY: plain data; zero is valid.
        let mut base: sys::drm_gem_object = unsafe { core::mem::zeroed() };
        base.funcs = &<T as Funcs>::FUNCS;
        let b = Box::into_raw(Box::new(Gem { base: core::cell::UnsafeCell::new(base), vt: &<T as HasVt>::VT, data }));
        let obj = b as *mut sys::drm_gem_object;
        // SAFETY: the new object and the driver's device.
        let r = unsafe {
            if memory {
                sys::drm_gem_object_init(dev.raw(), obj, size)
            } else {
                sys::drm_gem_private_object_init(dev.raw(), obj, size)
            }
        };
        if let Err(e) = check(r) {
            // A failed init leaves the structure to its maker.
            // SAFETY: never shared.
            drop(unsafe { Box::from_raw(b) });
            return Err(e);
        }
        // SAFETY: just made; the init's reference is ours.
        Ok(GemRef { p: unsafe { NonNull::new_unchecked(b) } })
    }

    /// The object of `handle` in `file`, if it is of type `T`: `ENOENT`
    /// without such a handle, `EINVAL` for an object of another type (an
    /// import of another device's buffer, say).
    pub fn lookup(file: &File, handle: u32) -> Result<GemRef<T>, Errno> {
        // SAFETY: a live file.
        let obj = unsafe { sys::drm_gem_object_lookup(file.raw(), handle) };
        if obj.is_null() {
            return Err(Errno::ENOENT);
        }
        if !is_type::<T>(obj) {
            // SAFETY: the lookup's reference.
            unsafe { sys::drm_gem_object_put(obj) };
            return Err(Errno::EINVAL);
        }
        // SAFETY: an object of type T with the lookup's reference.
        Ok(GemRef { p: unsafe { NonNull::new_unchecked(obj as *mut Gem<T>) } })
    }

    /// A new reference to a C object of type `T`, or `None`.
    ///
    /// # Safety
    /// `obj` is NULL or a live object the caller may read.
    pub unsafe fn from_borrowed(obj: *mut sys::drm_gem_object) -> Option<GemRef<T>> {
        if obj.is_null() || !is_type::<T>(obj) {
            return None;
        }
        // SAFETY: a live object of type T.
        unsafe { sys::drm_gem_object_get(obj) };
        Some(GemRef { p: NonNull::new(obj as *mut Gem<T>)? })
    }

    /// Creates a handle for the object in `file`.
    pub fn handle_create(&self, file: &File) -> Result<u32, Errno> {
        let mut h = 0;
        // SAFETY: a live file and object.
        check(unsafe { sys::drm_gem_handle_create(file.raw(), self.raw(), &mut h) })?;
        Ok(h)
    }

    /// The fake mmap offset, made if the object has none.
    pub fn mmap_offset(&self) -> Result<u64, Errno> {
        // SAFETY: a live object.
        check(unsafe { sys::drm_gem_create_mmap_offset(self.raw()) })?;
        // SAFETY: set by the call above, under the core's rules.
        Ok(unsafe { (*self.raw()).vma_node.vm_node.start } * drm_core::offsets::PAGE_SIZE)
    }

    /// Adds a backing of the object's size; returns its index.
    pub fn add_backing(&self) -> Result<u32, Errno> {
        // SAFETY: a live object.
        let r = unsafe { sys::drm_illumos_gem_add_backing(self.raw()) };
        check(r)?;
        Ok(r as u32)
    }

    /// Runs `f` on every mapping piece, with the mapping list locked.
    pub fn with_mappings(&self, mut f: impl FnMut(&Mapping<'_>)) {
        unsafe extern "C" fn tramp(map: *mut sys::drm_illumos_mapping, arg: *mut c_void) {
            // SAFETY: arg is the closure passed below, alive during the call.
            let f = unsafe { &mut *(arg as *mut &mut dyn FnMut(&Mapping<'_>)) };
            f(&Mapping { raw: map, _call: core::marker::PhantomData });
        }
        let mut dynf: &mut dyn FnMut(&Mapping<'_>) = &mut f;
        // SAFETY: a live object; the closure outlives the call.
        unsafe {
            sys::drm_illumos_gem_foreach_mapping(
                self.raw(),
                Some(tramp),
                &mut dynf as *mut &mut dyn FnMut(&Mapping<'_>) as *mut c_void,
            )
        };
    }

    /// Adds `fence` to the object's reservation (locks it, makes room,
    /// adds, unlocks). Thread context.
    pub fn add_fence(&self, fence: &Fence, usage: Usage) -> Result<(), Errno> {
        add_fence_raw(self.raw(), fence, usage)
    }

    /// Waits for the reservation's fences of `usage` and stronger, at most
    /// `timeout_ns` (negative: no limit); `ETIME` on timeout, `EINTR`
    /// with `intr`. Thread context.
    pub fn wait(&self, usage: Usage, intr: bool, timeout_ns: i64) -> Result<(), Errno> {
        wait_raw(self.raw(), usage, intr, timeout_ns)
    }

    /// Whether every fence of `usage` and stronger has signalled.
    pub fn test_signaled(&self, usage: Usage) -> bool {
        // SAFETY: a live object's reservation.
        unsafe { sys::dma_resv_test_signaled((*self.raw()).resv, usage.raw()) }
    }

    /// Whether two references name the same object.
    pub fn ptr_eq(a: &GemRef<T>, b: &GemRef<T>) -> bool {
        a.p == b.p
    }

    /// Drops the reference without releasing the object in this context:
    /// a last reference is parked for the core's task queue
    /// (`drm_illumos_gem_object_put_deferred`). For a thread that must not
    /// run the object's [`GemObject::release`], such as one that completes
    /// the requests the release waits for. Any context.
    pub fn put_deferred(self) {
        let p = self.into_raw();
        // SAFETY: this reference's count.
        unsafe { sys::drm_illumos_gem_object_put_deferred(p.as_ptr().cast()) };
    }

    /// Drops the reference in a thread that may block, as the token
    /// proves: a last reference releases the object here and now.
    pub fn put(self, _ctx: &ddi::Sleepable) {
        let p = self.into_raw();
        // SAFETY: this reference's count, in a thread that may block.
        unsafe { sys::drm_gem_object_put(p.as_ptr().cast()) };
    }

    /// The reference as a raw pointer, for a driver that parks it (a
    /// timer); give it back with [`GemRef::from_raw`].
    pub fn into_raw(self) -> NonNull<Gem<T>> {
        let p = self.p;
        core::mem::forget(self);
        p
    }

    /// # Safety
    /// `p` came from [`GemRef::into_raw`] and is given back once.
    pub unsafe fn from_raw(p: NonNull<Gem<T>>) -> GemRef<T> {
        GemRef { p }
    }
}

/// A reference to an object of any type (for the reservation of an
/// object the driver did not make, such as an import).
pub struct GemAny {
    p: NonNull<sys::drm_gem_object>,
}

// SAFETY: as GemRef.
unsafe impl Send for GemAny {}
// SAFETY: as GemRef.
unsafe impl Sync for GemAny {}

impl Drop for GemAny {
    /// As [`GemRef`]'s: a last reference is released by a task queue;
    /// [`GemAny::put`] releases it at once where blocking is proven.
    fn drop(&mut self) {
        // SAFETY: this reference's count; any context.
        unsafe { sys::drm_illumos_gem_object_put_deferred(self.p.as_ptr()) };
    }
}

impl GemAny {
    /// The object of `handle` in `file`, whatever its type (`ENOENT`).
    pub fn lookup(file: &File, handle: u32) -> Result<GemAny, Errno> {
        // SAFETY: a live file.
        let obj = unsafe { sys::drm_gem_object_lookup(file.raw(), handle) };
        NonNull::new(obj).map(|p| GemAny { p }).ok_or(Errno::ENOENT)
    }
    /// The C object.
    pub fn raw(&self) -> *mut sys::drm_gem_object {
        self.p.as_ptr()
    }
    /// As [`GemRef::add_fence`].
    pub fn add_fence(&self, fence: &Fence, usage: Usage) -> Result<(), Errno> {
        add_fence_raw(self.raw(), fence, usage)
    }
    /// As [`GemRef::wait`].
    pub fn wait(&self, usage: Usage, intr: bool, timeout_ns: i64) -> Result<(), Errno> {
        wait_raw(self.raw(), usage, intr, timeout_ns)
    }
    /// As [`GemRef::test_signaled`].
    pub fn test_signaled(&self, usage: Usage) -> bool {
        // SAFETY: a live object's reservation.
        unsafe { sys::dma_resv_test_signaled((*self.raw()).resv, usage.raw()) }
    }
    /// Drops the reference without releasing the object in this context:
    /// a last reference is parked for the core's task queue. For
    /// `timeout(9F)` handlers.
    pub fn put_deferred(self) {
        let p = self.into_raw();
        // SAFETY: this reference's count.
        unsafe { sys::drm_illumos_gem_object_put_deferred(p.as_ptr()) };
    }

    /// Drops the reference in a thread that may block, as the token
    /// proves: a last reference releases the object here and now.
    pub fn put(self, _ctx: &ddi::Sleepable) {
        let p = self.into_raw();
        // SAFETY: this reference's count, in a thread that may block.
        unsafe { sys::drm_gem_object_put(p.as_ptr()) };
    }

    /// The reference as a raw pointer, for a driver that parks it (a
    /// timer); give it back with [`GemAny::from_raw`].
    pub fn into_raw(self) -> NonNull<sys::drm_gem_object> {
        let p = self.p;
        core::mem::forget(self);
        p
    }

    /// # Safety
    /// `p` came from [`GemAny::into_raw`] and is given back once.
    pub unsafe fn from_raw(p: NonNull<sys::drm_gem_object>) -> GemAny {
        GemAny { p }
    }

    /// The object as type `T`, if it is one.
    pub fn downcast<T: GemObject>(self) -> Result<GemRef<T>, GemAny> {
        if is_type::<T>(self.raw()) {
            let p = self.p.cast::<Gem<T>>();
            core::mem::forget(self);
            Ok(GemRef { p })
        } else {
            Err(self)
        }
    }
}

fn add_fence_raw(obj: *mut sys::drm_gem_object, fence: &Fence, usage: Usage) -> Result<(), Errno> {
    // SAFETY: a live object (the caller's reference) and its reservation.
    unsafe {
        let resv = (*obj).resv;
        check(sys::dma_resv_lock(resv, ptr::null_mut()))?;
        let r = check(sys::dma_resv_reserve_fences(resv, 1));
        if r.is_ok() {
            sys::dma_resv_add_fence(resv, fence.raw(), usage.raw());
        }
        sys::dma_resv_unlock(resv);
        r
    }
}

fn wait_raw(obj: *mut sys::drm_gem_object, usage: Usage, intr: bool, timeout_ns: i64) -> Result<(), Errno> {
    let ticks = crate::fence::ns_to_ticks(timeout_ns);
    // SAFETY: a live object's reservation.
    let r = unsafe { sys::dma_resv_wait_timeout((*obj).resv, usage.raw(), intr, ticks as u64) };
    crate::fence::wait_errno(r)
}
