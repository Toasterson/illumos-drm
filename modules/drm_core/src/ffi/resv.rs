// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `dma_resv.h`: the reservation object of a GEM object. The structure's
//! tail points at the core's `Resv` inside the object, which lives as long
//! as the object; callers hold a reference to the object.

use ddi::Errno;
use drm_core::defer::Ctx;
use drm_core::resv::{Resv, Usage};
use drm_facade_sys as f;

use crate::devcore::neg;

/// The core reservation of a structure the facade set up.
///
/// # Safety
/// `obj` is NULL or the reservation of a GEM object the caller holds.
unsafe fn resv<'a>(obj: *const f::dma_resv) -> Option<&'a Resv> {
    if obj.is_null() {
        return None;
    }
    // SAFETY: the caller's contract; set by fobj::init_public.
    let p = unsafe { (*obj).__core[0] };
    if p == 0 {
        return None;
    }
    // SAFETY: the object's live reservation.
    Some(unsafe { &*(p as *const Resv) })
}

/// The core's usage of a `dma_resv_usage`; `None` (after a warning) for a
/// value Linux does not define, which the functions refuse.
fn usage(u: f::dma_resv_usage) -> Option<Usage> {
    match u {
        f::DMA_RESV_USAGE_KERNEL => Some(Usage::Kernel),
        f::DMA_RESV_USAGE_WRITE => Some(Usage::Write),
        f::DMA_RESV_USAGE_READ => Some(Usage::Read),
        f::DMA_RESV_USAGE_BOOKKEEP => Some(Usage::Bookkeep),
        _ => {
            // SAFETY: a constant format with one unsigned argument.
            unsafe { ddi::sys::cmn_err(ddi::sys::CE_WARN, c"drm_core: unknown dma_resv_usage %u".as_ptr(), u) };
            None
        }
    }
}

unsafe fn lock(obj: *mut f::dma_resv, ctx: *mut f::ww_acquire_ctx, intr: bool) -> i32 {
    // SAFETY: documented for thread context.
    let sctx = unsafe { super::sleepable() };
    // SAFETY: the caller holds the object.
    let Some(r) = (unsafe { resv(obj) }) else { return neg(Errno::EINVAL) };
    if !ctx.is_null() {
        // Acquire contexts arrive with the public-layout base objects
        // (R5b); one object at a time until then.
        return neg(Errno::EINVAL);
    }
    match r.lock().lock_raw(intr, &sctx) {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_resv_lock(obj: *mut f::dma_resv, ctx: *mut f::ww_acquire_ctx) -> i32 {
    // SAFETY: the caller's arguments.
    unsafe { lock(obj, ctx, false) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_resv_lock_interruptible(obj: *mut f::dma_resv, ctx: *mut f::ww_acquire_ctx) -> i32 {
    // SAFETY: the caller's arguments.
    unsafe { lock(obj, ctx, true) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_resv_unlock(obj: *mut f::dma_resv) {
    // SAFETY: the caller holds the object.
    let Some(r) = (unsafe { resv(obj) }) else { return };
    if r.lock().unlock_raw().is_err() {
        // Only the thread that took it with dma_resv_lock releases it.
        // SAFETY: a constant format without arguments.
        unsafe {
            ddi::sys::cmn_err(
                ddi::sys::CE_WARN,
                c"drm_core: dma_resv_unlock by a thread that does not hold the lock; ignored".as_ptr(),
            )
        };
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_resv_reserve_fences(obj: *mut f::dma_resv, num_fences: u32) -> i32 {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller holds the object.
    let Some(r) = (unsafe { resv(obj) }) else { return neg(Errno::EINVAL) };
    match r.reserve(num_fences as usize, &ctx) {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_resv_add_fence(obj: *mut f::dma_resv, fence: *mut f::dma_fence, u: f::dma_resv_usage) {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller holds the object and the fence.
    let (Some(r), Some(fc)) = (unsafe { resv(obj) }, unsafe { super::fence::core_fence(fence) }) else { return };
    let added = match usage(u) {
        Some(u) => r.add_fence_raw(fc.clone(), u, &ctx),
        None => Err(Errno::EINVAL),
    };
    if let Err(e) = added {
        // Not locked or no room: keep the order by waiting, as the header
        // promises.
        // SAFETY: a constant format with one int argument.
        unsafe {
            ddi::sys::cmn_err(
                ddi::sys::CE_WARN,
                c"drm_core: dma_resv_add_fence failed (%d); waiting for the fence instead".as_ptr(),
                e.get(),
            )
        };
        let _ = fc.wait(false, None, &ctx);
    }
    fc.put(Ctx::Sleep(&ctx));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_resv_wait_timeout(
    obj: *mut f::dma_resv,
    u: f::dma_resv_usage,
    intr: bool,
    timeout: u64,
) -> i64 {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller holds the object.
    let Some(r) = (unsafe { resv(obj) }) else { return i64::from(neg(Errno::EINVAL)) };
    let t = i64::try_from(timeout).unwrap_or(i64::MAX);
    let Some(u) = usage(u) else { return i64::from(neg(Errno::EINVAL)) };
    let d = super::fence::deadline(t);
    super::fence::wait_result(r.wait(u, intr, d, &ctx), t, d)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_resv_test_signaled(obj: *mut f::dma_resv, u: f::dma_resv_usage) -> bool {
    // SAFETY: the caller holds the object.
    match (unsafe { resv(obj) }, usage(u)) {
        (Some(r), Some(u)) => r.test_signaled(u),
        // An unknown usage is refused: not signalled, so that nobody goes
        // ahead on it (its wait fails with -EINVAL).
        (Some(_), None) => false,
        (None, _) => true,
    }
}
