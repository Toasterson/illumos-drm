// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `dma_fence.h`.
//!
//! A `struct dma_fence` is the C handle of a core fence ([`Fence`]): its
//! tail holds a [`FenceCore`] (the core fence and the callbacks added
//! through this structure), and its `refcount` counts the C references.
//! A driver's fence (`dma_fence_init` on its own structure) gets a new core
//! fence in the module's fence domain; a fence the core hands out
//! (`dma_fence_get_stub`, a sync object's fence, `drm_illumos_fence_create`)
//! gets a structure allocated here, a proxy, which `release` frees.
//! Signalling, waiting and callbacks all happen on the core fence, which
//! may outlive the structure (a sync object or a reservation keeps it).

use core::ffi::c_void;
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use ddi::Errno;
use ddi::sys;
use drm_core::defer::Ctx;
use drm_core::fence::{AddError, Fence, FenceCallback, FenceCb};
use drm_core::karc::KArc;
use drm_core::ksync;
use drm_core::kvec::KVec;
use drm_facade_sys as f;

use crate::devcore::neg;

/// A spin lock for data that interrupt handlers touch and whose holder
/// never blocks; unlike a kernel mutex it needs no destruction, so its
/// owner may be freed in any context.
struct Spin<T> {
    locked: AtomicBool,
    v: core::cell::UnsafeCell<T>,
}

// SAFETY: the value is reached only with the lock held.
unsafe impl<T: Send> Sync for Spin<T> {}

impl<T> Spin<T> {
    const fn new(v: T) -> Spin<T> {
        Spin { locked: AtomicBool::new(false), v: core::cell::UnsafeCell::new(v) }
    }

    fn with<R>(&self, f: impl FnOnce(&mut T) -> R) -> R {
        while self.locked.compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() {
            core::hint::spin_loop();
        }
        // SAFETY: the lock is held.
        let r = f(unsafe { &mut *self.v.get() });
        self.locked.store(false, Ordering::Release);
        r
    }
}

/// A callback added through `dma_fence_add_callback`.
pub(crate) struct CCb {
    fence: *mut f::dma_fence,
    cb: *mut f::dma_fence_cb,
    func: unsafe extern "C" fn(*mut f::dma_fence, *mut f::dma_fence_cb),
}

// SAFETY: the pointers are the driver's, valid while the callback is
// added (the facade removes every callback before the structure goes).
unsafe impl Send for CCb {}
// SAFETY: as above.
unsafe impl Sync for CCb {}

impl FenceCallback for CCb {
    fn signaled(&self, _fence: &Fence, _ctx: Ctx<'_>) -> Option<Fence> {
        // SAFETY: the structure and the callback are live: the facade
        // removes the callback (waiting for a running one) before the
        // structure is released, and the driver keeps its callback until it
        // has run or is removed, as on Linux.
        unsafe {
            mark_signaled(self.fence);
            (self.func)(self.fence, self.cb);
        }
        None
    }
}

/// The core state of one `struct dma_fence`.
pub(crate) struct FenceCore {
    pub(crate) fence: Fence,
    cbs: Spin<KVec<FenceCb<CCb>>>,
}

impl Drop for FenceCore {
    fn drop(&mut self) {
        // No callback may run on the structure once it is gone.
        let cbs = self.cbs.with(core::mem::take);
        for h in cbs.iter() {
            self.fence.remove_callback_spin(h);
        }
    }
}

/// A structure for a fence the core hands out.
#[repr(C)]
struct ProxyFence {
    c: core::cell::UnsafeCell<f::dma_fence>,
}

// SAFETY: the structure is only changed through the facade's functions.
unsafe impl Send for ProxyFence {}
// SAFETY: as above.
unsafe impl Sync for ProxyFence {}

unsafe extern "C" fn proxy_name(_f: *mut f::dma_fence) -> *const core::ffi::c_char {
    c"drm".as_ptr()
}

unsafe extern "C" fn proxy_release(fence: *mut f::dma_fence) {
    // SAFETY: the structure is the first field of a ProxyFence made by
    // proxy(), whose reference this was.
    drop(unsafe { KArc::from_raw(NonNull::new_unchecked(fence as *mut ProxyFence)) });
}

static PROXY_OPS: f::dma_fence_ops = f::dma_fence_ops {
    get_driver_name: Some(proxy_name),
    get_timeline_name: Some(proxy_name),
    enable_signaling: None,
    signaled: None,
    wait: None,
    release: Some(proxy_release),
    set_deadline: None,
};

const SIGNALED: u64 = 1 << f::DMA_FENCE_FLAG_SIGNALED_BIT;
const TIMESTAMP: u64 = 1 << f::DMA_FENCE_FLAG_TIMESTAMP_BIT;
const INITIALIZED: u64 = 1 << f::DMA_FENCE_FLAG_INITIALIZED_BIT;

fn flags_or(fence: *mut f::dma_fence, bits: u64) {
    // SAFETY: a live structure; flags change only atomically.
    unsafe {
        let p = ptr::addr_of_mut!((*fence).flags);
        sys::atomic_or_64(p, bits);
    }
}

/// Copies the core fence's signalled state into the structure.
///
/// # Safety
/// `fence` is a live structure with a [`FenceCore`].
unsafe fn mark_signaled(fence: *mut f::dma_fence) {
    // SAFETY: the caller's contract.
    let Some(core) = (unsafe { core_of(fence) }) else { return };
    if !core.fence.is_signaled() {
        return;
    }
    // SAFETY: as above; error and timestamp are written before the flags
    // that announce them.
    unsafe {
        (*fence).error = core.fence.error().map_or(0, |e| -e.get());
        if let Some(t) = core.fence.timestamp() {
            (*fence).__bindgen_anon_2.timestamp = t;
            flags_or(fence, TIMESTAMP);
        }
    }
    flags_or(fence, SIGNALED);
}

/// The [`FenceCore`] of a structure the facade set up.
///
/// # Safety
/// `fence` is NULL or a structure with a reference the caller holds.
pub(crate) unsafe fn core_of<'a>(fence: *const f::dma_fence) -> Option<&'a FenceCore> {
    if fence.is_null() {
        return None;
    }
    // SAFETY: the caller's contract.
    let (p, check) = unsafe { ((*fence).__core[0], (*fence).__core[1]) };
    if p == 0 || p ^ crate::MAGIC_FENCE ^ (fence as u64) != check {
        return None;
    }
    // SAFETY: a FenceCore owned by the structure (see init_tail).
    Some(unsafe { &*(p as *const FenceCore) })
}

/// The core fence of a structure, or the module's signalled stub for a
/// structure without one (a `dma_fence_init` that found no memory).
///
/// # Safety
/// As [`core_of`].
pub(crate) unsafe fn core_fence(fence: *const f::dma_fence) -> Option<Fence> {
    // SAFETY: the caller's contract.
    match unsafe { core_of(fence) } {
        Some(c) => Some(c.fence.clone()),
        None if !fence.is_null() => crate::kernel_stub(),
        None => None,
    }
}

/// Gives the structure its core state, owned through `__core[0]`.
fn init_tail(fence: *mut f::dma_fence, fc: KArc<FenceCore>) {
    let p = KArc::into_raw(fc).as_ptr() as u64;
    // SAFETY: the caller's structure, not shared yet.
    unsafe {
        (*fence).__core[0] = p;
        (*fence).__core[1] = p ^ crate::MAGIC_FENCE ^ (fence as u64);
    }
}

fn new_core(fence: Fence) -> Option<KArc<FenceCore>> {
    KArc::try_new(FenceCore { fence, cbs: Spin::new(KVec::new()) }).ok()
}

/// A new structure for a core fence, with one reference; NULL without
/// memory. Any context.
pub(crate) fn proxy(fence: Fence) -> *mut f::dma_fence {
    // SAFETY: plain data; zero is valid.
    let pf = ProxyFence { c: core::cell::UnsafeCell::new(unsafe { core::mem::zeroed() }) };
    let Ok(arc) = KArc::try_new(pf) else { return ptr::null_mut() };
    let raw = KArc::into_raw(arc);
    let c = raw.as_ptr() as *mut f::dma_fence;
    let (context, seqno) = (fence.context(), fence.seqno());
    let Some(fc) = new_core(fence) else {
        // SAFETY: never shared.
        drop(unsafe { KArc::from_raw(raw) });
        return ptr::null_mut();
    };
    // SAFETY: just allocated, not shared.
    unsafe {
        (*c).ops = &PROXY_OPS;
        (*c).context = context;
        (*c).seqno = seqno;
        (*c).flags = INITIALIZED;
        (*c).refcount.refcount.refs.counter = 1;
        let l = ptr::addr_of_mut!((*c).__bindgen_anon_2.cb_list);
        (*l).next = l;
        (*l).prev = l;
    }
    init_tail(c, fc);
    // SAFETY: just set up.
    unsafe { mark_signaled(c) };
    c
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_init(
    fence: *mut f::dma_fence,
    ops: *const f::dma_fence_ops,
    lock: *mut f::spinlock_t,
    context: u64,
    seqno: u64,
) {
    if fence.is_null() {
        return;
    }
    // SAFETY: the caller's structure, not shared yet.
    unsafe {
        *(*fence).__bindgen_anon_1.extern_lock.as_mut() = lock;
        (*fence).ops = ops;
        (*fence).context = context;
        (*fence).seqno = seqno;
        (*fence).flags = INITIALIZED;
        (*fence).error = 0;
        (*fence).refcount.refcount.refs.counter = 1;
        let l = ptr::addr_of_mut!((*fence).__bindgen_anon_2.cb_list);
        (*l).next = l;
        (*l).prev = l;
        (*fence).__core = [0; 4];
    }
    let domain = crate::module_fences();
    let made = super::with_ctx(|c| match c {
        Ctx::Sleep(s) => Fence::new(domain, context, seqno, s).ok(),
        Ctx::Atomic(_) => Fence::try_new(domain, context, seqno).ok(),
    });
    match made.and_then(new_core) {
        Some(fc) => init_tail(fence, fc),
        None => {
            // Nothing may wait for it forever: signalled, with an error.
            // SAFETY: as above.
            unsafe { (*fence).error = neg(Errno::ENOMEM) };
            flags_or(fence, SIGNALED);
            // SAFETY: a constant message.
            unsafe { sys::cmn_err(sys::CE_WARN, c"drm: dma_fence_init: no memory; fence signalled".as_ptr()) };
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn dma_fence_context_alloc(num: u32) -> u64 {
    crate::module_fences().alloc_contexts(u64::from(num.max(1)))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_signal(fence: *mut f::dma_fence) {
    // SAFETY: the caller holds a reference.
    let Some(c) = (unsafe { core_of(fence) }) else { return };
    super::with_ctx(|ctx| c.fence.signal(ctx));
    // SAFETY: as above.
    unsafe { mark_signaled(fence) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_set_error(fence: *mut f::dma_fence, error: i32) {
    // SAFETY: the caller holds a reference.
    let Some(c) = (unsafe { core_of(fence) }) else { return };
    if c.fence.is_signaled() {
        return;
    }
    let Some(e) = Errno::new(error.saturating_abs()) else { return };
    c.fence.set_error(e);
    // SAFETY: as above.
    unsafe { (*fence).error = -e.get() };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_get(fence: *mut f::dma_fence) -> *mut f::dma_fence {
    if !fence.is_null() {
        // SAFETY: the caller holds a reference; the count changes only
        // atomically.
        unsafe { sys::atomic_inc_32(ptr::addr_of_mut!((*fence).refcount.refcount.refs.counter) as *mut u32) };
    }
    fence
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_put(fence: *mut f::dma_fence) {
    if fence.is_null() {
        return;
    }
    // SAFETY: the caller gives up a reference.
    let left = unsafe { sys::atomic_dec_32_nv(ptr::addr_of_mut!((*fence).refcount.refcount.refs.counter) as *mut u32) };
    if left != 0 {
        return;
    }
    // The last reference: the core state goes, then the structure.
    // SAFETY: nothing else refers to the structure.
    unsafe {
        let p = (*fence).__core[0];
        let check = (*fence).__core[1];
        (*fence).__core[0] = 0;
        (*fence).__core[1] = 0;
        if p != 0 && p ^ crate::MAGIC_FENCE ^ (fence as u64) == check {
            // Dropping the core state in any context is allowed: the core
            // fence defers its own destruction (drm_core::defer), the
            // callback records are freed with kmem_free, which interrupt
            // handlers may call, and FenceCore has no kernel lock.
            drop(KArc::from_raw(NonNull::new_unchecked(p as *mut FenceCore)));
        }
        let ops = (*fence).ops;
        if !ops.is_null()
            && let Some(rel) = (*ops).release
        {
            rel(fence);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_is_signaled(fence: *mut f::dma_fence) -> bool {
    // SAFETY: the caller holds a reference.
    match unsafe { core_of(fence) } {
        Some(c) => {
            let s = c.fence.is_signaled();
            if s {
                // SAFETY: as above.
                unsafe { mark_signaled(fence) };
            }
            s
        }
        // A structure without core state was signalled at init.
        None => !fence.is_null(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_get_status(fence: *mut f::dma_fence) -> i32 {
    // SAFETY: the caller holds a reference.
    if !unsafe { dma_fence_is_signaled(fence) } {
        return 0;
    }
    // SAFETY: as above.
    let e = unsafe { (*fence).error };
    if e < 0 { e } else { 1 }
}

/// Clock ticks to nanoseconds and back.
fn ticks_to_ns(t: i64) -> i64 {
    // SAFETY: drv_hztousec(9F) may be called from any context.
    let us = unsafe { sys::drv_hztousec(t as sys::clock_t) } as i64;
    us.saturating_mul(1000)
}

fn ns_to_ticks(ns: i64) -> i64 {
    // SAFETY: drv_usectohz(9F) may be called from any context.
    unsafe { sys::drv_usectohz((ns / 1000) as sys::clock_t) as i64 }
}

/// A deadline on the monotonic clock for a timeout in clock ticks.
pub(crate) fn deadline(timeout: i64) -> Option<i64> {
    // MAX_SCHEDULE_TIMEOUT, LONG_MAX, is no limit.
    if timeout == i64::MAX || timeout < 0 { None } else { Some(ksync::now().saturating_add(ticks_to_ns(timeout))) }
}

/// The remaining ticks of a wait that succeeded, at least 1.
pub(crate) fn remaining(timeout: i64, deadline: Option<i64>) -> i64 {
    match deadline {
        None => timeout.max(1),
        Some(d) => ns_to_ticks(d.saturating_sub(ksync::now())).max(1),
    }
}

/// The result of a wait as Linux returns it.
pub(crate) fn wait_result(r: Result<(), Errno>, timeout: i64, deadline: Option<i64>) -> i64 {
    match r {
        Ok(()) => remaining(timeout, deadline),
        Err(e) if e.get() == sys::errno::ETIME => 0,
        Err(e) => i64::from(neg(e)),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_wait_timeout(fence: *mut f::dma_fence, intr: bool, timeout: i64) -> i64 {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller holds a reference.
    let Some(fc) = (unsafe { core_fence(fence) }) else { return i64::from(neg(Errno::EINVAL)) };
    let d = deadline(timeout);
    let r = wait_result(fc.wait(intr, d, &ctx), timeout, d);
    fc.put(Ctx::Sleep(&ctx));
    // SAFETY: as above.
    unsafe { mark_signaled(fence) };
    r
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_add_callback(
    fence: *mut f::dma_fence,
    cb: *mut f::dma_fence_cb,
    func: f::dma_fence_func_t,
) -> i32 {
    let (Some(func), false) = (func, cb.is_null()) else { return neg(Errno::EINVAL) };
    // SAFETY: the caller holds a reference.
    let Some(c) = (unsafe { core_of(fence) }) else { return neg(Errno::ENOENT) };
    // SAFETY: the caller's callback structure.
    unsafe {
        (*cb).func = Some(func);
        (*cb).node.next = ptr::addr_of_mut!((*cb).node);
        (*cb).node.prev = ptr::addr_of_mut!((*cb).node);
    }
    let Ok(h) = FenceCb::try_new(CCb { fence, cb, func }) else { return neg(Errno::ENOMEM) };
    match c.fence.add_callback(&h) {
        Ok(()) => {}
        Err(AddError::Signaled) => return neg(Errno::ENOENT),
        Err(AddError::Busy) => return neg(Errno::EINVAL),
    }
    match c.cbs.with(|l| l.try_push(h)) {
        Ok(()) => 0,
        Err(h) => {
            c.fence.remove_callback_spin(&h);
            neg(Errno::ENOMEM)
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_remove_callback(fence: *mut f::dma_fence, cb: *mut f::dma_fence_cb) -> bool {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller holds a reference.
    let Some(c) = (unsafe { core_of(fence) }) else { return false };
    let h = c.cbs.with(|l| {
        let i = l.iter().rposition(|h| ptr::eq(h.get().cb, cb))?;
        l.swap_remove(i)
    });
    let Some(h) = h else { return false };
    c.fence.remove_callback(&h, &ctx)
}

#[unsafe(no_mangle)]
pub extern "C" fn dma_fence_get_stub() -> *mut f::dma_fence {
    match crate::kernel_stub() {
        Some(s) => proxy(s),
        None => ptr::null_mut(),
    }
}

/// The token of `dma_fence_chain_alloc`; the core allocates the chain
/// link itself in `drm_syncobj_add_point`.
static CHAIN_TOKENS: AtomicU32 = AtomicU32::new(0);

#[unsafe(no_mangle)]
pub extern "C" fn dma_fence_chain_alloc() -> *mut f::dma_fence_chain {
    CHAIN_TOKENS.fetch_add(1, Ordering::Relaxed);
    // A non-NULL token that is never dereferenced.
    NonNull::<c_void>::dangling().as_ptr().cast()
}

#[unsafe(no_mangle)]
pub extern "C" fn dma_fence_chain_free(_chain: *mut f::dma_fence_chain) {}
