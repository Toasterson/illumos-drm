// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `drm_illumos.h`: GEM memory, user mappings and device fences.

use core::ffi::{c_int, c_void};
use core::ptr;

use ddi::Errno;
use ddi::sys;
use drm_core::fence::Fence;
use drm_core::gem::{Fault, Mapping};
use drm_facade_sys as f;

use crate::devcore::{DevCore, neg};
use crate::fobj::{MAX_BACKINGS, gem_ref};

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_gem_add_backing(obj: *mut f::drm_gem_object) -> i32 {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller holds the object.
    let Some(r) = (unsafe { gem_ref(obj) }) else { return neg(Errno::EINVAL) };
    let full = r.object().with_backing(MAX_BACKINGS - 1, |_| ()).is_some();
    let none = r.object().with_backing(0, |_| ()).is_none();
    let res = if full || none {
        Err(Errno::EINVAL)
    } else {
        super::gem::new_backing(&r).map_err(|(e, ())| e).and_then(|m| r.object().add_backing(m, &ctx))
    };
    r.put(&ctx);
    match res {
        Ok(i) => i as i32,
        Err(e) => neg(e),
    }
}

/// Runs `f` on backing `i` of a C object.
///
/// # Safety
/// `obj` is NULL or an object the caller holds.
unsafe fn with_backing<R>(
    obj: *mut f::drm_gem_object,
    i: u32,
    f: impl FnOnce(&drm_core::gem::SysMem) -> R,
) -> Option<R> {
    // SAFETY: the caller's contract.
    let r = unsafe { gem_ref(obj) }?;
    let out = r.object().with_backing(i as usize, f);
    super::with_ctx(|c| r.put_in(c));
    out
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_gem_umem(obj: *mut f::drm_gem_object, backing: u32) -> f::ddi_umem_cookie_t {
    // SAFETY: the caller holds the object.
    unsafe { with_backing(obj, backing, |m| m.umem_cookie().as_raw()) }.unwrap_or(ptr::null_mut())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_gem_vaddr(obj: *mut f::drm_gem_object, backing: u32) -> *mut c_void {
    // SAFETY: the caller holds the object.
    unsafe { with_backing(obj, backing, |m| m.kernel_ptr().as_ptr().cast()) }.unwrap_or(ptr::null_mut())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_gem_sg_count(obj: *mut f::drm_gem_object, backing: u32) -> usize {
    // SAFETY: the caller holds the object.
    unsafe { with_backing(obj, backing, |m| m.sg_len()) }.unwrap_or(0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_gem_sg_cookie(
    obj: *mut f::drm_gem_object,
    backing: u32,
    i: usize,
    cookie: *mut f::ddi_dma_cookie_t,
) -> i32 {
    if cookie.is_null() {
        return neg(Errno::EINVAL);
    }
    // SAFETY: the caller holds the object.
    let c = unsafe { with_backing(obj, backing, |m| m.sg_iter().nth(i)) }.flatten();
    let Some(c) = c else { return neg(Errno::EINVAL) };
    // SAFETY: the caller's cookie, of the DDI's layout.
    unsafe {
        let out = cookie as *mut sys::ddi_dma_cookie_t;
        ptr::write_bytes(out, 0, 1);
        (*out)._dmu._dmac_ll = c.address;
        (*out).dmac_size = c.size;
    }
    0
}

/// The core mapping behind a `struct drm_illumos_mapping *`, which is only
/// handed out for the duration of a callback.
///
/// # Safety
/// `m` is a mapping pointer the facade passed to the caller's running
/// callback.
unsafe fn mapping<'a>(m: *const f::drm_illumos_mapping) -> Option<&'a Mapping> {
    // SAFETY: the caller's contract.
    if m.is_null() { None } else { Some(unsafe { &*(m as *const Mapping) }) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_gem_foreach_mapping(
    obj: *mut f::drm_gem_object,
    func: f::drm_illumos_mapping_fn,
    arg: *mut c_void,
) {
    let Some(func) = func else { return };
    // SAFETY: the caller holds the object.
    let Some(r) = (unsafe { gem_ref(obj) }) else { return };
    r.with_mappings(|_, it| {
        for m in it {
            // SAFETY: the driver's callback with a mapping valid during it.
            unsafe { func(&m as *const Mapping as *mut f::drm_illumos_mapping, arg) };
        }
    });
    super::with_ctx(|c| r.put_in(c));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_mapping_cookie(map: *const f::drm_illumos_mapping) -> f::devmap_cookie_t {
    // SAFETY: as mapping().
    unsafe { mapping(map) }.map_or(ptr::null_mut(), |m| m.raw())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_mapping_user_offset(map: *const f::drm_illumos_mapping) -> u64 {
    // SAFETY: as mapping().
    unsafe { mapping(map) }.map_or(0, |m| m.user_offset())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_mapping_object_offset(map: *const f::drm_illumos_mapping) -> u64 {
    // SAFETY: as mapping().
    unsafe { mapping(map) }.map_or(0, |m| m.object_offset())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_mapping_len(map: *const f::drm_illumos_mapping) -> usize {
    // SAFETY: as mapping().
    unsafe { mapping(map) }.map_or(0, |m| m.len())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_mapping_is_settled(map: *const f::drm_illumos_mapping) -> bool {
    // SAFETY: as mapping().
    unsafe { mapping(map) }.is_some_and(|m| m.is_settled())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_mapping_unload(map: *mut f::drm_illumos_mapping) -> c_int {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: as mapping().
    match unsafe { mapping(map) } {
        Some(m) => match m.unload(&ctx) {
            Ok(()) => 0,
            Err(e) => e,
        },
        None => sys::errno::EINVAL,
    }
}

/// # Safety
/// `f` is a fault pointer the facade passed to the running callback.
unsafe fn fault<'a>(f: *const f::drm_illumos_fault) -> Option<&'a Fault> {
    // SAFETY: the caller's contract.
    if f.is_null() { None } else { Some(unsafe { &*(f as *const Fault) }) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_fault_load(flt: *mut f::drm_illumos_fault) -> c_int {
    // SAFETY: as fault().
    unsafe { fault(flt) }.map_or(sys::errno::EINVAL, |x| x.load())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_fault_default_access(flt: *mut f::drm_illumos_fault) -> c_int {
    // SAFETY: as fault().
    unsafe { fault(flt) }.map_or(sys::errno::EINVAL, |x| x.default_access())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_fence_context_alloc(dev: *mut f::drm_device, num: u32) -> u64 {
    // SAFETY: the caller's device.
    match unsafe { DevCore::of(dev) }.and_then(|c| c.device()) {
        Some(d) => d.sync().domain().alloc_contexts(u64::from(num.max(1))),
        None => super::fence::dma_fence_context_alloc(num),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_fence_create(
    dev: *mut f::drm_device,
    context: u64,
    seqno: u64,
) -> *mut f::dma_fence {
    let err = neg(Errno::ENOMEM) as isize as *mut f::dma_fence;
    // SAFETY: the caller's device.
    let Some(d) = (unsafe { DevCore::of(dev) }).and_then(|c| c.device()) else {
        return neg(Errno::ENODEV) as isize as *mut f::dma_fence;
    };
    let domain = d.sync().domain();
    let fence = super::with_ctx(|c| match c {
        drm_core::defer::Ctx::Sleep(s) => Fence::new(domain, context, seqno, s).ok(),
        drm_core::defer::Ctx::Atomic(_) => Fence::try_new(domain, context, seqno).ok(),
    });
    match fence.map(super::fence::proxy) {
        Some(p) if !p.is_null() => p,
        _ => err,
    }
}
