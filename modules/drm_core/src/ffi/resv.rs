// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `dma_resv.h`: the reservation object of a GEM object. The structure's
//! tail points at the core's `Resv` inside the object, which lives as long
//! as the object; callers hold a reference to the object.

use alloc::boxed::Box;

use ddi::Errno;
use drm_core::defer::Ctx;
use drm_core::resv::{Resv, Usage};
use drm_core::wwmutex::RESERVATION_CLASS;
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

/// The `dma_resv_usage` of a core usage.
fn usage_c(u: Usage) -> f::dma_resv_usage {
    match u {
        Usage::Kernel => f::DMA_RESV_USAGE_KERNEL,
        Usage::Write => f::DMA_RESV_USAGE_WRITE,
        Usage::Read => f::DMA_RESV_USAGE_READ,
        Usage::Bookkeep => f::DMA_RESV_USAGE_BOOKKEEP,
    }
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

// --- facade 1.1: reservations outside GEM objects, transactions, snapshots ---

/// Gives a driver's own `struct dma_resv` (one that is not a GEM object's
/// `_resv`) a core reservation, which `dma_resv_fini` releases. Thread
/// context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_resv_init(obj: *mut f::dma_resv) {
    if obj.is_null() {
        return;
    }
    // A reservation that has one already (a GEM object's `_resv`, or a
    // second init) keeps it: overwriting would orphan the core's.
    // SAFETY: the caller's structure; a fresh one holds no matching pair.
    let (c0, c1) = unsafe { ((*obj).__core[0], (*obj).__core[1]) };
    if c0 != 0 && (c1 == c0 ^ crate::MAGIC_RESV || c1 == c0 ^ crate::MAGIC_RESV_GEM) {
        // SAFETY: a constant format without arguments.
        unsafe {
            ddi::sys::cmn_err(
                ddi::sys::CE_WARN,
                c"drm_core: dma_resv_init of a reservation that has one already; ignored".as_ptr(),
            )
        };
        return;
    }
    // SAFETY: the caller's structure, not in use yet.
    unsafe {
        (*obj).__core[0] = 0;
        (*obj).__core[1] = 0;
    }
    // The kernel's allocator waits for memory (KM_SLEEP), as for every
    // other Box of the core.
    let p = Box::into_raw(Box::new(Resv::new())) as u64;
    // SAFETY: as above.
    unsafe {
        (*obj).__core[0] = p;
        (*obj).__core[1] = p ^ crate::MAGIC_RESV;
    }
}

/// Releases what `dma_resv_init` set up: the fences and the reservation.
/// A GEM object's reservation is the core's and stays (nothing happens);
/// a reservation still locked is not released (a warning).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_resv_fini(obj: *mut f::dma_resv) {
    if obj.is_null() {
        return;
    }
    // SAFETY: the caller's structure.
    let (p, check) = unsafe { ((*obj).__core[0], (*obj).__core[1]) };
    if p == 0 || p ^ crate::MAGIC_RESV != check {
        return;
    }
    // SAFETY: allocated by dma_resv_init.
    let r = unsafe { &*(p as *const Resv) };
    if r.lock().is_locked() {
        // SAFETY: a constant format without arguments.
        unsafe {
            ddi::sys::cmn_err(ddi::sys::CE_WARN, c"drm_core: dma_resv_fini of a locked reservation; kept".as_ptr())
        };
        return;
    }
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    r.clear(&ctx);
    // SAFETY: as above; the mark goes first.
    unsafe {
        (*obj).__core[0] = 0;
        (*obj).__core[1] = 0;
        drop(Box::from_raw(p as *mut Resv));
    }
}

/// Linux's `dma_resv_trylock`: takes the lock without waiting, as
/// `dma_resv_lock(obj, NULL)` would; false if anyone holds it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_resv_trylock(obj: *mut f::dma_resv) -> bool {
    // SAFETY: the caller holds the object.
    match unsafe { resv(obj) } {
        Some(r) => r.lock().try_lock_raw(),
        None => false,
    }
}

/// A new transaction stamp of the reservation class, for a compatibility
/// layer's `ww_acquire_init` (smaller is older; shared with the core's
/// own transactions).
#[unsafe(no_mangle)]
pub extern "C" fn drm_illumos_ww_stamp() -> u64 {
    RESERVATION_CLASS.next_stamp()
}

/// Locks `obj` for the transaction `stamp` that holds `acquired`
/// reservations (Linux's `dma_resv_lock(obj, ctx)` and
/// `dma_resv_lock_slow(obj, ctx)` with `slow`): the core's wait-die rule
/// against every holder, its own transactions included. 0, `-EDEADLK`
/// (release everything, then lock the contended one with `slow`),
/// `-EALREADY` (the transaction holds it), `-EINTR` (`intr`), `-EINVAL`.
/// The caller counts `acquired` itself and releases with
/// `drm_illumos_resv_unlock_ctx`. Thread context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_resv_lock_ctx(
    obj: *mut f::dma_resv,
    stamp: u64,
    acquired: u32,
    slow: bool,
    intr: bool,
) -> i32 {
    // SAFETY: documented for thread context.
    let sctx = unsafe { super::sleepable() };
    // SAFETY: the caller holds the object.
    let Some(r) = (unsafe { resv(obj) }) else { return neg(Errno::EINVAL) };
    match r.lock().lock_raw_ctx(stamp, acquired, slow, intr, &sctx) {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

/// Takes the lock for the transaction `stamp` without waiting (Linux's
/// `ww_mutex_trylock` with a context); false if anyone holds it. Released
/// with `drm_illumos_resv_unlock_ctx`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_resv_trylock_ctx(obj: *mut f::dma_resv, stamp: u64) -> bool {
    // SAFETY: the caller holds the object.
    match unsafe { resv(obj) } {
        Some(r) => r.lock().try_lock_raw_ctx(stamp),
        None => false,
    }
}

/// Whether anyone holds the reservation's lock (Linux's `dma_resv_held`,
/// for assertions; false for an unknown reservation).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_resv_is_locked(obj: *mut f::dma_resv) -> bool {
    // SAFETY: the caller holds the object.
    unsafe { resv(obj) }.is_some_and(|r| r.lock().is_locked())
}

/// Releases a lock taken with `drm_illumos_resv_lock_ctx` for `stamp`:
/// 0, or `-EPERM` (with a warning) if the calling thread does not hold it
/// that way, which leaves the lock as it was; `-EINVAL` for no reservation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_resv_unlock_ctx(obj: *mut f::dma_resv, stamp: u64) -> i32 {
    // SAFETY: the caller holds the object.
    let Some(r) = (unsafe { resv(obj) }) else { return neg(Errno::EINVAL) };
    if r.lock().unlock_raw_ctx(stamp).is_err() {
        // SAFETY: a constant format without arguments.
        unsafe {
            ddi::sys::cmn_err(
                ddi::sys::CE_WARN,
                c"drm_core: drm_illumos_resv_unlock_ctx by a thread or transaction that does not hold the lock; refused"
                    .as_ptr(),
            )
        };
        return neg(Errno::EPERM);
    }
    0
}

/// A snapshot of the fences of `obj` with a usage at least as important
/// as `u` (Linux's reservation iterators and `dma_resv_get_fences`, into a
/// caller's array): stores up to `max` fences, each with a new reference,
/// in `fences`, the usage each one has in `usages` (if not NULL), and the
/// number the reservation had in `*count`, which may exceed `max` (then
/// retry with a larger array; the stored ones are still the caller's to
/// put). Signalled fences are included with `DRM_ILLUMOS_RESV_SIGNALED`
/// in `flags`. A fence a driver made (`dma_fence_init`) comes back as
/// that driver's structure while it lives; other fences as the core's
/// structures. 0, `-EINVAL` or `-ENOMEM`. Thread context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_resv_fences(
    obj: *mut f::dma_resv,
    u: f::dma_resv_usage,
    flags: u32,
    fences: *mut *mut f::dma_fence,
    usages: *mut u8,
    max: u32,
    count: *mut u32,
) -> i32 {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller holds the object.
    let (Some(r), Some(u)) = (unsafe { resv(obj) }, usage(u)) else { return neg(Errno::EINVAL) };
    if count.is_null() || (max > 0 && fences.is_null()) || flags & !f::DRM_ILLUMOS_RESV_SIGNALED != 0 {
        return neg(Errno::EINVAL);
    }
    let list = match r.entries(u, flags & f::DRM_ILLUMOS_RESV_SIGNALED != 0, &ctx) {
        Ok(l) => l,
        Err(e) => return neg(e),
    };
    let mut stored = 0u32;
    let mut err = 0;
    for (fence, fu) in list.iter() {
        if stored >= max {
            break;
        }
        let p = super::fence::proxy(fence.clone());
        if p.is_null() {
            err = neg(Errno::ENOMEM);
            break;
        }
        // SAFETY: the caller's arrays of max entries.
        unsafe {
            *fences.add(stored as usize) = p;
            if !usages.is_null() {
                *usages.add(stored as usize) = usage_c(*fu) as u8;
            }
        }
        stored += 1;
    }
    let n = list.len() as u32;
    let mut list = list;
    while let Some((fence, _)) = list.len().checked_sub(1).and_then(|i| list.swap_remove(i)) {
        fence.put(Ctx::Sleep(&ctx));
    }
    if err != 0 {
        for i in 0..stored {
            // SAFETY: stored above with a reference.
            unsafe { super::fence::dma_fence_put(*fences.add(i as usize)) };
        }
        return err;
    }
    // SAFETY: checked above.
    unsafe { *count = n };
    0
}
