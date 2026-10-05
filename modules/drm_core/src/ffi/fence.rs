// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `dma_fence.h`: the C functions over `drm_core::cfence`, which implements
//! Linux's fence semantics on the public structure and links it to the
//! core's fence objects (the shadow of a driver's fence, the proxy of a
//! core fence). Releases are parked on the module's fence domain, whose
//! task queue runs them in thread context.

use core::ffi::{c_char, c_void};
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicU32, Ordering};

use ddi::Errno;
use ddi::sys;
use drm_core::cfence::{self, TickClock};
use drm_core::fence::Fence;
use drm_facade_sys as f;

/// illumos clock ticks (hz).
pub(crate) struct Hz;

impl TickClock for Hz {
    fn ticks_to_ns(&self, t: i64) -> i64 {
        // SAFETY: drv_hztousec(9F) may be called from any context.
        let us = unsafe { sys::drv_hztousec(t as sys::clock_t) } as i64;
        us.saturating_mul(1000)
    }
    fn ns_to_ticks(&self, ns: i64) -> i64 {
        // SAFETY: drv_usectohz(9F) may be called from any context.
        unsafe { sys::drv_usectohz((ns / 1000) as sys::clock_t) as i64 }
    }
}

/// A deadline on the monotonic clock for a timeout in clock ticks.
pub(crate) fn deadline(timeout: i64) -> Option<i64> {
    cfence::deadline(timeout, &Hz)
}

/// The result of a wait as Linux returns it.
pub(crate) fn wait_result(r: Result<(), Errno>, timeout: i64, deadline: Option<i64>) -> i64 {
    cfence::wait_result(r, timeout, deadline, &Hz)
}

/// The core fence of a structure (enabling signalling and adding the
/// bridge of a driver's fence); for an initialised structure without one
/// (a `dma_fence_init` that found no memory) a new signalled core fence
/// with its error, or the module's stub if there is no memory for that
/// either; `None` for NULL or a structure that was never initialised.
///
/// # Safety
/// `fence` is NULL or a structure with a reference the caller holds; the
/// caller does not hold its lock.
pub(crate) unsafe fn core_fence(fence: *mut f::dma_fence) -> Option<Fence> {
    // SAFETY: the caller's contract.
    unsafe {
        match cfence::core_fence(fence) {
            Some(c) => Some(c),
            None if cfence::is_initialised(fence) => {
                // dma_fence_init signalled it for lack of memory: a
                // signalled core fence with its error, or else the stub.
                let e = (*fence).error;
                match Fence::try_new(crate::module_fences(), (*fence).context, (*fence).seqno) {
                    Ok(f) => {
                        if let Some(e) = Errno::new(-e).filter(|_| e < 0) {
                            f.set_error(e);
                        }
                        // SAFETY: signalling never blocks.
                        let a = ddi::Atomic::assume();
                        f.signal(drm_core::defer::Ctx::Atomic(&a));
                        Some(f)
                    }
                    Err(_) => crate::kernel_stub(),
                }
            }
            None => None,
        }
    }
}

/// A new structure for a core fence, with one reference; NULL without
/// memory. Any context.
pub(crate) fn proxy(fence: Fence) -> *mut f::dma_fence {
    // A driver's own structure when the core fence shadows one.
    let p = cfence::structure_of(fence, crate::module_fences());
    publish_module();
    p
}

/// Publishes the counters of the module's fence domain (kstat
/// `drm:0:sync`), in thread context only (the kstat's lock is a thread
/// lock); the domain's drain task publishes what interrupt-context calls
/// changed.
fn publish_module() {
    if !ddi::private::servicing_interrupt() {
        crate::module_fences().publish();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_init(
    fence: *mut f::dma_fence,
    ops: *const f::dma_fence_ops,
    lock: *mut f::spinlock_t,
    context: u64,
    seqno: u64,
) {
    // SAFETY: the caller's structure, not shared yet.
    unsafe { cfence::init(fence, ops, lock, context, seqno, 0, crate::module_fences()) };
    publish_module();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_init64(
    fence: *mut f::dma_fence,
    ops: *const f::dma_fence_ops,
    lock: *mut f::spinlock_t,
    context: u64,
    seqno: u64,
) {
    // SAFETY: as dma_fence_init.
    unsafe { cfence::init(fence, ops, lock, context, seqno, cfence::SEQNO64, crate::module_fences()) };
    publish_module();
}

#[unsafe(no_mangle)]
pub extern "C" fn dma_fence_context_alloc(num: u32) -> u64 {
    crate::module_fences().alloc_contexts(u64::from(num.max(1)))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_signal(fence: *mut f::dma_fence) {
    // SAFETY: the caller holds a reference and not the lock.
    unsafe { cfence::signal(fence, None) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_signal_locked(fence: *mut f::dma_fence) {
    // SAFETY: the caller holds a reference and the lock.
    unsafe { cfence::signal_locked(fence, None) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_signal_timestamp(fence: *mut f::dma_fence, timestamp: f::ktime_t) {
    // SAFETY: as dma_fence_signal.
    unsafe { cfence::signal(fence, Some(timestamp)) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_signal_timestamp_locked(fence: *mut f::dma_fence, timestamp: f::ktime_t) {
    // SAFETY: as dma_fence_signal_locked.
    unsafe { cfence::signal_locked(fence, Some(timestamp)) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_check_and_signal(fence: *mut f::dma_fence) -> bool {
    // SAFETY: as dma_fence_signal.
    unsafe {
        let was = cfence::signaled_flag(fence);
        cfence::signal(fence, None);
        was
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_check_and_signal_locked(fence: *mut f::dma_fence) -> bool {
    // SAFETY: as dma_fence_signal_locked.
    unsafe {
        let was = cfence::signaled_flag(fence);
        cfence::signal_locked(fence, None);
        was
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_set_error(fence: *mut f::dma_fence, error: i32) {
    // SAFETY: the caller holds a reference.
    unsafe { cfence::set_error(fence, error) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_get(fence: *mut f::dma_fence) -> *mut f::dma_fence {
    // SAFETY: the caller holds a reference.
    unsafe { cfence::get(fence) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_put(fence: *mut f::dma_fence) {
    // SAFETY: the caller gives up a reference; documented for interrupt
    // handlers and threads that may block.
    unsafe { super::with_ctx(|ctx| cfence::put(fence, ctx, crate::module_fences())) };
    publish_module();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_fence_put_deferred(fence: *mut f::dma_fence) {
    // SAFETY: the caller gives up a reference; any context.
    unsafe { cfence::put_deferred(fence, Some(crate::module_fences())) };
}

#[unsafe(no_mangle)]
pub extern "C" fn drm_illumos_fence_barrier() {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    crate::module_fences().barrier(&ctx);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_is_signaled(fence: *mut f::dma_fence) -> bool {
    // SAFETY: the caller holds a reference and not the lock.
    unsafe { cfence::is_signaled(fence) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_get_status(fence: *mut f::dma_fence) -> i32 {
    // SAFETY: as dma_fence_is_signaled.
    unsafe { cfence::get_status(fence) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_enable_sw_signaling(fence: *mut f::dma_fence) {
    // SAFETY: as dma_fence_is_signaled.
    unsafe { cfence::enable_sw_signaling(fence) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_wait_timeout(fence: *mut f::dma_fence, intr: bool, timeout: i64) -> i64 {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller holds a reference and not the lock.
    unsafe { cfence::wait_timeout(fence, intr, timeout, &Hz, &ctx) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_default_wait(fence: *mut f::dma_fence, intr: bool, timeout: i64) -> i64 {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: as dma_fence_wait_timeout.
    unsafe { cfence::default_wait(fence, intr, timeout, &Hz, &ctx) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_add_callback(
    fence: *mut f::dma_fence,
    cb: *mut f::dma_fence_cb,
    func: f::dma_fence_func_t,
) -> i32 {
    // SAFETY: the caller holds a reference and keeps the callback.
    unsafe { cfence::add_callback(fence, cb, func) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_remove_callback(fence: *mut f::dma_fence, cb: *mut f::dma_fence_cb) -> bool {
    // SAFETY: as dma_fence_add_callback.
    unsafe { cfence::remove_callback(fence, cb) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_set_deadline(fence: *mut f::dma_fence, deadline: f::ktime_t) {
    // SAFETY: the caller holds a reference.
    unsafe { cfence::set_deadline(fence, deadline) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_driver_name(fence: *mut f::dma_fence) -> *const c_char {
    // SAFETY: the caller holds a reference.
    unsafe { cfence::name(fence, false) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_fence_timeline_name(fence: *mut f::dma_fence) -> *const c_char {
    // SAFETY: the caller holds a reference.
    unsafe { cfence::name(fence, true) }
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
