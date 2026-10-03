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
use drm_facade_sys as f;

use crate::devcore::neg;

/// A spin lock for data that interrupt handlers touch. The holder runs
/// with interrupts blocked on its processor (`ddi_enter_critical(9F)`), so
/// an interrupt handler on the same processor never spins on a lock its
/// own processor holds, and the critical sections only move pointers (no
/// call leaves this module, as the DDI requires inside such a region).
/// Unlike a kernel mutex it needs no initialisation or destruction, so its
/// owner may be made and freed in any context.
struct Spin<T> {
    locked: AtomicBool,
    v: core::cell::UnsafeCell<T>,
}

// SAFETY: the value is reached only with the lock held.
unsafe impl<T: Send> Sync for Spin<T> {}
// SAFETY: as above.
unsafe impl<T: Send> Send for Spin<T> {}

impl<T> Spin<T> {
    const fn new(v: T) -> Spin<T> {
        Spin { locked: AtomicBool::new(false), v: core::cell::UnsafeCell::new(v) }
    }

    /// Runs `f` with the lock held; `f` must only touch memory.
    fn with<R>(&self, f: impl FnOnce(&mut T) -> R) -> R {
        // SAFETY: may be called in any context; paired below.
        let spl = unsafe { sys::ddi_enter_critical() };
        while self.locked.compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() {
            core::hint::spin_loop();
        }
        // SAFETY: the lock is held.
        let r = f(unsafe { &mut *self.v.get() });
        self.locked.store(false, Ordering::Release);
        // SAFETY: the value ddi_enter_critical returned.
        unsafe { sys::ddi_exit_critical(spl) };
        r
    }
}

/// The callbacks added through one `struct dma_fence`, shared by its
/// [`FenceCore`] and every callback record ([`CCb`]), so that a record can
/// tell that the structure is gone without touching it.
///
/// The list is intrusive, through the driver's `struct dma_fence_cb`,
/// which the core owns while the callback is added (as on Linux):
/// `node.next` links the next callback, `node.prev` holds the record's
/// handle ([`FenceCb`]). Nothing is allocated with the lock held.
pub(crate) struct CbList {
    st: Spin<CbState>,
}

struct CbState {
    head: *mut f::dma_fence_cb,
    /// The structure was released: no callback may run any more.
    gone: bool,
    /// The callback whose function runs now, and its thread (the thread
    /// that signals the fence runs its callbacks one after the other).
    running: *mut f::dma_fence_cb,
    runner: u64,
}

// SAFETY: the pointers are reached only with the lock held and point at
// callbacks the driver keeps while they are added.
unsafe impl Send for CbState {}

const _: () = assert!(core::mem::size_of::<FenceCb<CCb>>() == core::mem::size_of::<*mut f::list_head>());
const _: () = assert!(core::mem::align_of::<FenceCb<CCb>>() <= core::mem::align_of::<*mut f::list_head>());

/// Leaves a callback's `node` empty (pointing at itself), as Linux does
/// once a callback is off the list, so that `list_empty(&cb->node)` holds.
///
/// # Safety
/// `cb` is a callback structure the caller may write (off the list).
unsafe fn reinit_node(cb: *mut f::dma_fence_cb) {
    // SAFETY: the caller's contract.
    unsafe {
        let n = ptr::addr_of_mut!((*cb).node);
        (*n).next = n;
        (*n).prev = n;
    }
}

/// The handle slot of a callback structure.
fn slot(cb: *mut f::dma_fence_cb) -> *mut *mut f::list_head {
    // SAFETY: only computes the field's address.
    unsafe { ptr::addr_of_mut!((*cb).node.prev) }
}

/// Stores a record's handle in its callback structure. Lock held.
///
/// # Safety
/// `cb` is a listed callback with an empty slot.
unsafe fn put_handle(cb: *mut f::dma_fence_cb, h: FenceCb<CCb>) {
    // SAFETY: the caller's contract; the slot is pointer-sized (above).
    unsafe { ptr::write(slot(cb) as *mut FenceCb<CCb>, h) }
}

/// Takes the handle out of a callback structure, if it has one. Lock held.
///
/// # Safety
/// `cb` is a listed callback.
unsafe fn take_handle(cb: *mut f::dma_fence_cb) -> Option<FenceCb<CCb>> {
    // SAFETY: the caller's contract.
    unsafe {
        if (*slot(cb)).is_null() {
            return None;
        }
        let h = ptr::read(slot(cb) as *const FenceCb<CCb>);
        *slot(cb) = ptr::null_mut();
        Some(h)
    }
}

impl CbState {
    /// Whether `cb` is on the list.
    fn contains(&self, cb: *mut f::dma_fence_cb) -> bool {
        let mut cur = self.head;
        while !cur.is_null() {
            if cur == cb {
                return true;
            }
            // SAFETY: listed callbacks are live.
            cur = unsafe { (*cur).node.next } as *mut f::dma_fence_cb;
        }
        false
    }

    /// Unlinks `cb`; `false` if it is not on the list.
    fn unlink(&mut self, cb: *mut f::dma_fence_cb) -> bool {
        let mut prev: *mut f::dma_fence_cb = ptr::null_mut();
        let mut cur = self.head;
        while !cur.is_null() {
            // SAFETY: listed callbacks are live (the driver keeps them
            // while they are added and the structure is not released).
            let next = unsafe { (*cur).node.next } as *mut f::dma_fence_cb;
            if cur == cb {
                if prev.is_null() {
                    self.head = next;
                } else {
                    // SAFETY: as above.
                    unsafe { (*prev).node.next = next as *mut f::list_head };
                }
                return true;
            }
            prev = cur;
            cur = next;
        }
        false
    }
}

/// A callback added through `dma_fence_add_callback`.
pub(crate) struct CCb {
    fence: *mut f::dma_fence,
    cb: *mut f::dma_fence_cb,
    func: unsafe extern "C" fn(*mut f::dma_fence, *mut f::dma_fence_cb),
    list: KArc<CbList>,
}

// SAFETY: the pointers are the driver's, used only while the shared list
// says the structure is live and the callback added.
unsafe impl Send for CCb {}
// SAFETY: as above.
unsafe impl Sync for CCb {}

impl FenceCallback for CCb {
    fn signaled(&self, _fence: &Fence, _ctx: Ctx<'_>) -> Option<Fence> {
        let me = ksync::thread_id();
        // Under the lock: is the structure still there? Then the callback
        // is ours to run: take it off the list (the driver may free it in
        // its function) together with its handle.
        let r = self.list.st.with(|st| {
            if st.gone {
                return None;
            }
            // SAFETY: the structure is live, so is a listed callback.
            let h = if st.unlink(self.cb) {
                // SAFETY: as above; off the list now.
                unsafe {
                    let h = take_handle(self.cb);
                    reinit_node(self.cb);
                    h
                }
            } else {
                None
            };
            st.running = self.cb;
            st.runner = me;
            Some(h)
        });
        // The structure was released before the callback could run: it
        // counts as removed, and nothing of it may be touched.
        let h = r?;
        // SAFETY: the structure and the callback are live: a release waits
        // until `running` is clear (unless it happens in this very
        // function, from the driver's own put), and the driver keeps the
        // callback until it has run or is removed, as on Linux.
        unsafe {
            mark_signaled(self.fence);
            (self.func)(self.fence, self.cb);
        }
        // The function may have dropped the structure's last reference;
        // only the shared list is touched from here on.
        self.list.st.with(|st| st.running = ptr::null_mut());
        drop(h);
        None
    }
}

/// The core state of one `struct dma_fence`.
pub(crate) struct FenceCore {
    pub(crate) fence: Fence,
    list: KArc<CbList>,
}

impl Drop for FenceCore {
    /// The structure's last reference went (in any context, possibly in
    /// one of its own callbacks): no callback may run on it from now on.
    fn drop(&mut self) {
        let me = ksync::thread_id();
        let head = self.list.st.with(|st| {
            st.gone = true;
            core::mem::replace(&mut st.head, ptr::null_mut())
        });
        // Callbacks still added: removed from the core fence without
        // waiting (one queued behind a running callback finds `gone` and
        // does nothing).
        let mut cur = head;
        while !cur.is_null() {
            // SAFETY: the driver keeps added callbacks; the list was ours
            // alone once taken.
            let next = unsafe { (*cur).node.next } as *mut f::dma_fence_cb;
            // SAFETY: as above.
            if let Some(h) = unsafe { take_handle(cur) } {
                self.fence.remove_callback_nowait(&h);
            }
            // SAFETY: as above; off the list.
            unsafe { reinit_node(cur) };
            cur = next;
        }
        // A callback function running on another thread uses the
        // structure: wait for it. One running on this thread is the caller
        // (its function dropped the last reference): waiting would never
        // end, and it does not touch the structure after its function.
        loop {
            let busy = self.list.st.with(|st| !st.running.is_null() && st.runner != me);
            if !busy {
                break;
            }
            core::hint::spin_loop();
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
    let list = KArc::try_new(CbList {
        st: Spin::new(CbState { head: ptr::null_mut(), gone: false, running: ptr::null_mut(), runner: 0 }),
    })
    .ok()?;
    KArc::try_new(FenceCore { fence, list }).ok()
}

/// Publishes the counters of the module's fence domain (kstat
/// `drm:0:sync`), in thread context only (the kstat's lock is a thread
/// lock); the domain's drain task publishes what interrupt-context calls
/// changed.
fn publish_module() {
    // The kstat's lock is a thread-level adaptive mutex: not on an
    // interrupt thread (the facade's callers never hold spin locks or
    // raise the priority level).
    if !ddi::private::servicing_interrupt() {
        crate::module_fences().publish();
    }
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
    // Without sleeping for memory, in any context (a timeout(9F) handler
    // included, which the core cannot tell from a thread).
    let made = Fence::try_new(domain, context, seqno).ok();
    match made.and_then(new_core) {
        Some(fc) => {
            init_tail(fence, fc);
            publish_module();
        }
        None => {
            // Nothing may wait for it forever: signalled, with an error.
            // SAFETY: as above.
            unsafe { (*fence).error = neg(Errno::ENOMEM) };
            flags_or(fence, SIGNALED);
            // SAFETY: a constant message.
            unsafe { sys::cmn_err(sys::CE_WARN, c"drm_core: dma_fence_init: no memory; fence signalled".as_ptr()) };
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
    // Atomic in every context: releases that signalling triggers are
    // deferred, so it never blocks (timeout(9F) handlers included).
    // SAFETY: nothing on this path blocks.
    let a = unsafe { ddi::Atomic::assume() };
    c.fence.signal(Ctx::Atomic(&a));
    // SAFETY: as above.
    unsafe { mark_signaled(fence) };
    publish_module();
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
        publish_module();
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
    let Ok(h) = FenceCb::try_new(CCb { fence, cb, func, list: c.list.clone() }) else { return neg(Errno::ENOMEM) };
    // Listed first, with an empty handle slot, so that the callback finds
    // itself on the list however soon the fence signals.
    c.list.st.with(|st| {
        // SAFETY: the caller's callback structure, the core's from now on.
        unsafe {
            (*cb).func = Some(func);
            *slot(cb) = ptr::null_mut();
            (*cb).node.next = st.head as *mut f::list_head;
        }
        st.head = cb;
    });
    match c.fence.add_callback(&h) {
        Ok(()) => {
            // Keep the handle with the callback unless it has run already.
            let left = c.list.st.with(|st| {
                if st.contains(cb) {
                    // SAFETY: listed, with the empty slot set above.
                    unsafe { put_handle(cb, h) };
                    None
                } else {
                    Some(h)
                }
            });
            drop(left);
            0
        }
        Err(e) => {
            c.list.st.with(|st| {
                if st.unlink(cb) {
                    // SAFETY: the caller's callback, off the list.
                    unsafe { reinit_node(cb) };
                }
            });
            drop(h);
            match e {
                AddError::Signaled => neg(Errno::ENOENT),
                AddError::Busy => neg(Errno::EINVAL),
            }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_remove_callback(fence: *mut f::dma_fence, cb: *mut f::dma_fence_cb) -> bool {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller holds a reference.
    let Some(c) = (unsafe { core_of(fence) }) else { return false };
    let found = c.list.st.with(|st| {
        // SAFETY: a listed callback is live.
        if st.unlink(cb) {
            // SAFETY: as above; off the list now.
            unsafe {
                let h = take_handle(cb);
                reinit_node(cb);
                Some(h)
            }
        } else {
            None
        }
    });
    match found {
        Some(Some(h)) => c.fence.remove_callback(&h, &ctx),
        // Added by a call that has not returned yet: the driver's race.
        Some(None) => false,
        None => {
            // It has run, or runs now: wait until its function returns
            // (unless it is this thread's, which cannot wait for itself).
            let me = ksync::thread_id();
            while c.list.st.with(|st| st.running == cb && st.runner != me) {
                ksync::yield_briefly(&ctx);
            }
            false
        }
    }
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
