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

fn usage(u: f::dma_resv_usage) -> Usage {
    match u {
        f::DMA_RESV_USAGE_KERNEL => Usage::Kernel,
        f::DMA_RESV_USAGE_WRITE => Usage::Write,
        f::DMA_RESV_USAGE_READ => Usage::Read,
        _ => Usage::Bookkeep,
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
    if r.lock().is_locked() {
        // SAFETY: the caller locked it with dma_resv_lock (the facade's
        // only way to hold it from C).
        unsafe { r.lock().unlock_raw() };
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
    if let Err(e) = r.add_fence_raw(fc.clone(), usage(u), &ctx) {
        // Not locked or no room: keep the order by waiting, as the header
        // promises.
        // SAFETY: a constant format with one int argument.
        unsafe {
            ddi::sys::cmn_err(
                ddi::sys::CE_WARN,
                c"drm: dma_resv_add_fence failed (%d); waiting for the fence instead".as_ptr(),
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
    let d = super::fence::deadline(t);
    super::fence::wait_result(r.wait(usage(u), intr, d, &ctx), t, d)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_resv_test_signaled(obj: *mut f::dma_resv, u: f::dma_resv_usage) -> bool {
    // SAFETY: the caller holds the object.
    match unsafe { resv(obj) } {
        Some(r) => r.test_signaled(usage(u)),
        None => true,
    }
}
