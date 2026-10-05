// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `drm_syncobj.h`: sync objects and sync files.

use alloc::boxed::Box;
use core::ptr;

use ddi::Errno;
use drm_core::defer::Ctx;
use drm_core::syncobj::Syncobj;
use drm_facade_sys as f;

use crate::cdriver::file_of;
use crate::devcore::neg;

/// The handle a `struct drm_syncobj *` points to.
struct SyncobjBox {
    magic: u64,
    s: Syncobj,
}

const MAGIC_SYNCOBJ: u64 = 0x6472_6d5f_736f_6230; // "drm_sob0"

/// # Safety
/// `p` is NULL or a handle from `drm_syncobj_find` not yet put.
unsafe fn syncobj<'a>(p: *mut f::drm_syncobj) -> Option<&'a Syncobj> {
    if p.is_null() {
        return None;
    }
    // SAFETY: the caller's contract.
    let b = unsafe { &*(p as *const SyncobjBox) };
    (b.magic == MAGIC_SYNCOBJ ^ (p as u64)).then_some(&b.s)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_syncobj_find(file: *mut f::drm_file, handle: u32) -> *mut f::drm_syncobj {
    // SAFETY: the caller's open file.
    let Some((c, core)) = (unsafe { file_of(file) }) else { return ptr::null_mut() };
    let Some(d) = core.device() else { return ptr::null_mut() };
    let Ok(s) = d.sync().syncobj(c, handle) else { return ptr::null_mut() };
    let b = Box::into_raw(Box::new(SyncobjBox { magic: 0, s }));
    // SAFETY: just made.
    unsafe { (*b).magic = MAGIC_SYNCOBJ ^ (b as u64) };
    b.cast()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_syncobj_put(obj: *mut f::drm_syncobj) {
    // SAFETY: the caller gives up its handle.
    if unsafe { syncobj(obj) }.is_some() {
        // SAFETY: a box from drm_syncobj_find; the mark goes first.
        unsafe {
            (*(obj as *mut SyncobjBox)).magic = 0;
            drop(Box::from_raw(obj as *mut SyncobjBox));
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_syncobj_replace_fence(obj: *mut f::drm_syncobj, fence: *mut f::dma_fence) {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller's handle.
    let Some(s) = (unsafe { syncobj(obj) }) else { return };
    // SAFETY: the caller holds the fence (NULL resets).
    let fc = if fence.is_null() { None } else { unsafe { super::fence::core_fence(fence) } };
    s.replace_fence(fc, &ctx);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_syncobj_add_point(
    obj: *mut f::drm_syncobj,
    chain: *mut f::dma_fence_chain,
    fence: *mut f::dma_fence,
    point: u64,
) {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    super::fence::dma_fence_chain_free(chain);
    // SAFETY: the caller's handle and fence.
    let (Some(s), Some(fc)) = (unsafe { syncobj(obj) }, unsafe { super::fence::core_fence(fence) }) else { return };
    if let Err(e) = s.add_point(fc, point, &ctx) {
        // SAFETY: a constant format with one int argument.
        unsafe {
            ddi::sys::cmn_err(ddi::sys::CE_WARN, c"drm_core: drm_syncobj_add_point failed (%d)".as_ptr(), e.get())
        };
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_syncobj_find_fence(
    file: *mut f::drm_file,
    handle: u32,
    point: u64,
    flags: u64,
    fence: *mut *mut f::dma_fence,
) -> i32 {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    if fence.is_null() || flags & !u64::from(drm_uapi::DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) != 0 {
        return neg(Errno::EINVAL);
    }
    // SAFETY: the caller's open file.
    let Some((c, core)) = (unsafe { file_of(file) }) else { return neg(Errno::EINVAL) };
    let Some(d) = core.device() else { return neg(Errno::ENODEV) };
    let s = match d.sync().syncobj(c, handle) {
        Ok(s) => s,
        Err(e) => return neg(e),
    };
    let wfs = flags & u64::from(drm_uapi::DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) != 0;
    let r = s.find_fence(point, wfs, &ctx);
    drop(s);
    match r {
        Ok(fc) => {
            let p = super::fence::proxy(fc);
            if p.is_null() {
                return neg(Errno::ENOMEM);
            }
            // SAFETY: checked above.
            unsafe { *fence = p };
            0
        }
        Err(e) => neg(e),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_sync_file_install(fence: *mut f::dma_fence, cloexec: bool, fdp: *mut i32) -> i32 {
    // SAFETY: documented for ioctl handlers, thread context.
    let ctx = unsafe { super::sleepable() };
    if fdp.is_null() {
        return neg(Errno::EINVAL);
    }
    // SAFETY: the caller holds the fence.
    let Some(fc) = (unsafe { super::fence::core_fence(fence) }) else { return neg(Errno::EINVAL) };
    let r = crate::ctxt::with_current(|frame, arg| {
        // SAFETY: the frame's device is the ioctl's, registered while it
        // runs.
        let core = unsafe { frame.core.as_ref() };
        let d = core.device().ok_or(Errno::ENODEV)?;
        let mut out = -1;
        d.sync().export_sync_file(
            fc.clone(),
            b"drm",
            cloexec,
            arg,
            |fd| {
                out = fd;
                Ok(())
            },
            &ctx,
        )?;
        Ok(out)
    });
    fc.put(Ctx::Sleep(&ctx));
    match r {
        Some(Ok(fd)) => {
            // SAFETY: checked above.
            unsafe { *fdp = fd };
            0
        }
        Some(Err(e)) => neg(e),
        // Outside an ioctl handler there is no process to give it to.
        None => neg(Errno::EINVAL),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_sync_file_get_fence(fd: i32, fence: *mut *mut f::dma_fence) -> i32 {
    // SAFETY: documented for ioctl handlers, thread context.
    let ctx = unsafe { super::sleepable() };
    if fence.is_null() {
        return neg(Errno::EINVAL);
    }
    let r = crate::ctxt::with_current(|frame, arg| {
        // SAFETY: as in drm_sync_file_install.
        let core = unsafe { frame.core.as_ref() };
        let d = core.device().ok_or(Errno::ENODEV)?;
        d.sync().sync_file_fence(fd, arg, &ctx)
    });
    match r {
        Some(Ok(fc)) => {
            let p = super::fence::proxy(fc);
            if p.is_null() {
                return neg(Errno::ENOMEM);
            }
            // SAFETY: checked above.
            unsafe { *fence = p };
            0
        }
        Some(Err(e)) => neg(e),
        None => neg(Errno::EINVAL),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sync_file_get_fence(fd: i32) -> *mut f::dma_fence {
    // SAFETY: documented for ioctl handlers, thread context.
    let ctx = unsafe { super::sleepable() };
    let r = crate::ctxt::with_current(|frame, arg| {
        // SAFETY: as in drm_sync_file_install.
        let core = unsafe { frame.core.as_ref() };
        let d = core.device().ok_or(Errno::ENODEV)?;
        d.sync().sync_file_fence(fd, arg, &ctx)
    });
    match r {
        Some(Ok(fc)) => super::fence::proxy(fc),
        _ => ptr::null_mut(),
    }
}

// --- facade 1.1: sync objects a driver owns ---

/// Wraps a sync object in a new handle structure (as `drm_syncobj_find`).
fn boxed(s: Syncobj) -> *mut f::drm_syncobj {
    let b = Box::into_raw(Box::new(SyncobjBox { magic: 0, s }));
    // SAFETY: just made.
    unsafe { (*b).magic = MAGIC_SYNCOBJ ^ (b as u64) };
    b.cast()
}

/// Linux's `drm_syncobj_create`: a new sync object without a handle,
/// signalled if `flags` has DRM_SYNCOBJ_CREATE_SIGNALED, holding `fence`
/// if not NULL. 0 and the object in `*out` (release with
/// `drm_syncobj_put`), `-EINVAL` or `-ENOMEM`. Thread context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_syncobj_create(
    out: *mut *mut f::drm_syncobj,
    flags: u32,
    fence: *mut f::dma_fence,
) -> i32 {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    if out.is_null() || flags & !drm_uapi::DRM_SYNCOBJ_CREATE_SIGNALED != 0 {
        return neg(Errno::EINVAL);
    }
    let signaled = flags & drm_uapi::DRM_SYNCOBJ_CREATE_SIGNALED != 0;
    let s = match drm_core::syncobj::create(crate::module_fences(), signaled, &ctx) {
        Ok(s) => s,
        Err(e) => return neg(e),
    };
    if !fence.is_null() {
        // SAFETY: the caller holds the fence.
        let fc = unsafe { super::fence::core_fence(fence) };
        s.replace_fence(fc, &ctx);
    }
    let b = boxed(s);
    if b.is_null() {
        return neg(Errno::ENOMEM);
    }
    // SAFETY: checked above.
    unsafe { *out = b };
    0
}

/// A new handle structure for the same sync object (release each with
/// `drm_syncobj_put`); NULL without memory.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_syncobj_dup(obj: *mut f::drm_syncobj) -> *mut f::drm_syncobj {
    // SAFETY: the caller's handle.
    match unsafe { syncobj(obj) } {
        Some(s) => boxed(s.clone()),
        None => ptr::null_mut(),
    }
}

/// Linux's `drm_syncobj_fence_get`: the sync object's current fence with a
/// new reference (the core's view of it), or NULL if it has none.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_syncobj_fence_get(obj: *mut f::drm_syncobj) -> *mut f::dma_fence {
    // SAFETY: the caller's handle.
    let Some(s) = (unsafe { syncobj(obj) }) else { return ptr::null_mut() };
    match s.fence() {
        Some(fc) => super::fence::proxy(fc),
        None => ptr::null_mut(),
    }
}

/// Linux's `drm_is_current_master`: whether `file` is its device's
/// current master; false for an unknown file.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_is_current_master(file: *mut f::drm_file) -> bool {
    // SAFETY: the caller's open file.
    let Some((c, core)) = (unsafe { file_of(file) }) else { return false };
    core.device().is_some_and(|d| d.is_current_master(c))
}
