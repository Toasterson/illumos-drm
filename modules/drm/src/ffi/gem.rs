// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `drm_gem.h`: GEM objects (see [`crate::fobj`] for the model).

use core::ptr::NonNull;

use ddi::Errno;
use drm_core::dumb::{self, DumbRequest};
use drm_core::gem::{GemCore, GemObject, GemRef, SysMem};
use drm_core::offsets::PAGE_SIZE;
use drm_facade_sys as f;
use drm_uapi as uapi;

use crate::cdriver::file_of;
use crate::devcore::{DevCore, neg};
use crate::fobj::{FObj, gem_ref, init_public};

/// The common part of the two init functions.
///
/// # Safety
/// `dev` and `obj` are the caller's live structures; `obj` is not in use.
unsafe fn init(dev: *mut f::drm_device, obj: *mut f::drm_gem_object, size: usize, with_memory: bool) -> i32 {
    // SAFETY: documented for thread context (ioctl handlers).
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller's device.
    let Some(core) = (unsafe { DevCore::of(dev) }) else { return neg(Errno::EINVAL) };
    let Some(obj_nn) = NonNull::new(obj) else { return neg(Errno::EINVAL) };
    // SAFETY: the caller's object.
    let funcs = unsafe { (*obj).funcs };
    if funcs.is_null() || size == 0 || !(size as u64).is_multiple_of(PAGE_SIZE) {
        return neg(Errno::EINVAL);
    }
    // SAFETY: the driver's static callbacks.
    let funcs: &'static f::drm_gem_object_funcs = unsafe { &*funcs };
    // An object without its own memory must say where its mappings go;
    // otherwise a mapping would have nothing to show.
    if funcs.free.is_none() || (!with_memory && funcs.illumos_umem.is_none()) {
        return neg(Errno::EINVAL);
    }
    let Some(gem) = core.device().and_then(|d| d.gem().cloned()) else { return neg(Errno::ENODEV) };
    let r = (|| -> Result<GemRef<FObj>, Errno> {
        if size > core.info().max_object_size {
            return Err(Errno::ENOMEM);
        }
        let (reservation, mem) = if with_memory {
            let res = gem.reserve(size, &ctx)?;
            (Some(res), Some(gem.sys_mem(size, &ctx)?))
        } else {
            (None, None)
        };
        let fobj = FObj::new(obj_nn, NonNull::from(core), funcs, size, mem, None, &ctx)?;
        match reservation {
            Some(res) => GemCore::create_reserved(&gem, fobj, res, &ctx),
            None => GemCore::create(&gem, fobj, &ctx),
        }
    })();
    match r {
        Ok(r) => {
            // SAFETY: the object is not in use yet.
            unsafe { init_public(obj, core, &r) };
            r.object().arm();
            // The caller's reference: the count stays, the GemRef goes.
            let _ = r.into_raw();
            0
        }
        Err(e) => neg(e),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_gem_object_init(dev: *mut f::drm_device, obj: *mut f::drm_gem_object, size: usize) -> i32 {
    // SAFETY: the caller's structures.
    unsafe { init(dev, obj, size, true) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_gem_private_object_init(
    dev: *mut f::drm_device,
    obj: *mut f::drm_gem_object,
    size: usize,
) -> i32 {
    // SAFETY: the caller's structures.
    unsafe { init(dev, obj, size, false) }
}

#[unsafe(no_mangle)]
pub extern "C" fn drm_gem_object_release(_obj: *mut f::drm_gem_object) {}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_gem_object_get(obj: *mut f::drm_gem_object) {
    // SAFETY: the caller holds a reference.
    if let Some(r) = unsafe { gem_ref(obj) } {
        let _ = r.into_raw();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_gem_object_put(obj: *mut f::drm_gem_object) {
    // SAFETY: the caller holds a reference, which it gives up.
    let Some(p) = (unsafe { crate::fobj::raw_of(obj) }) else { return };
    // SAFETY: one of the caller's counted references.
    let r = unsafe { GemRef::<FObj>::from_raw(p) };
    super::with_ctx(|c| r.put_in(c));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_gem_object_put_deferred(obj: *mut f::drm_gem_object) {
    // SAFETY: the caller holds a reference, which it gives up.
    let Some(p) = (unsafe { crate::fobj::raw_of(obj) }) else { return };
    // SAFETY: one of the caller's counted references.
    let r = unsafe { GemRef::<FObj>::from_raw(p) };
    // SAFETY: the caller promises nothing about its context; the atomic
    // path never blocks and parks a last reference for the task queue.
    let a = unsafe { ddi::Atomic::assume() };
    r.put_in(drm_core::defer::Ctx::Atomic(&a));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_gem_handle_create(
    file: *mut f::drm_file,
    obj: *mut f::drm_gem_object,
    handlep: *mut u32,
) -> i32 {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller's open file.
    let Some((c, core)) = (unsafe { file_of(file) }) else { return neg(Errno::EINVAL) };
    if handlep.is_null() {
        return neg(Errno::EINVAL);
    }
    // SAFETY: the caller holds a reference.
    let Some(r) = (unsafe { gem_ref(obj) }) else { return neg(Errno::EINVAL) };
    let Some(gem) = core.device().and_then(|d| d.gem().cloned()) else {
        r.put(&ctx);
        return neg(Errno::ENODEV);
    };
    // Only this device's objects: another device's would enter tables it
    // does not belong to (an import makes an object of this device).
    if !drm_core::karc::KArc::ptr_eq(r.core(), &gem) {
        r.put(&ctx);
        return neg(Errno::EINVAL);
    }
    match gem.handle_create(c, r, &ctx) {
        Ok(h) => {
            // SAFETY: checked above.
            unsafe { *handlep = h };
            0
        }
        Err(e) => neg(e),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_gem_handle_delete(file: *mut f::drm_file, handle: u32) -> i32 {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller's open file.
    let Some((c, core)) = (unsafe { file_of(file) }) else { return neg(Errno::EINVAL) };
    let Some(gem) = core.device().and_then(|d| d.gem().cloned()) else { return neg(Errno::ENODEV) };
    match gem.handle_close(c, handle, &ctx) {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_gem_object_lookup(file: *mut f::drm_file, handle: u32) -> *mut f::drm_gem_object {
    // SAFETY: the caller's open file.
    let Some((c, core)) = (unsafe { file_of(file) }) else { return core::ptr::null_mut() };
    let Some(gem) = core.device().and_then(|d| d.gem().cloned()) else { return core::ptr::null_mut() };
    match gem.lookup(c, handle) {
        Ok(r) => {
            let base = r.object().base();
            let _ = r.into_raw();
            base
        }
        Err(_) => core::ptr::null_mut(),
    }
}

/// The fake offset of an object, made if it has none.
fn offset_of_obj(core: &DevCore, r: &GemRef<FObj>) -> Result<u64, Errno> {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    let gem = core.device().and_then(|d| d.gem().cloned()).ok_or(Errno::ENODEV)?;
    let off = gem.mmap_offset(r, &ctx)?;
    // SAFETY: the object's C structure is live (r holds it).
    unsafe {
        let b = r.object().base();
        (*b).vma_node.vm_node.start = off / PAGE_SIZE;
        (*b).vma_node.vm_node.size = (r.object().size() as u64) / PAGE_SIZE;
    }
    Ok(off)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_gem_create_mmap_offset(obj: *mut f::drm_gem_object) -> i32 {
    // SAFETY: the caller holds a reference.
    let Some(r) = (unsafe { gem_ref(obj) }) else { return neg(Errno::EINVAL) };
    let core = r.object().core();
    let res = offset_of_obj(core, &r);
    // SAFETY: documented for thread context.
    r.put(&unsafe { super::sleepable() });
    match res {
        Ok(_) => 0,
        Err(e) => neg(e),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_gem_dumb_map_offset(
    file: *mut f::drm_file,
    _dev: *mut f::drm_device,
    handle: u32,
    offset: *mut u64,
) -> i32 {
    // SAFETY: the caller's open file.
    let Some((c, core)) = (unsafe { file_of(file) }) else { return neg(Errno::EINVAL) };
    if offset.is_null() {
        return neg(Errno::EINVAL);
    }
    let Some(gem) = core.device().and_then(|d| d.gem().cloned()) else { return neg(Errno::ENODEV) };
    let r = match gem.lookup(c, handle) {
        Ok(r) => r,
        Err(e) => return neg(e),
    };
    let res = offset_of_obj(core, &r);
    // SAFETY: documented for thread context.
    r.put(&unsafe { super::sleepable() });
    match res {
        Ok(o) => {
            // SAFETY: checked above.
            unsafe { *offset = o };
            0
        }
        Err(e) => neg(e),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_mode_size_dumb(
    _dev: *mut f::drm_device,
    args: *mut f::drm_mode_create_dumb,
    hw_pitch_align: u64,
    hw_size_align: u64,
) -> i32 {
    if args.is_null() {
        return neg(Errno::EINVAL);
    }
    // SAFETY: the caller's structure, of the uAPI layout.
    let a = unsafe { &mut *(args as *mut uapi::drm_mode_create_dumb) };
    let req = DumbRequest { width: a.width, height: a.height, bpp: a.bpp, flags: a.flags };
    let pitch_align = if hw_pitch_align == 0 { dumb::PITCH_ALIGN } else { hw_pitch_align as u32 };
    let r = dumb::check_request(&req).and_then(|()| dumb::layout(&req, req.bpp.div_ceil(8), pitch_align));
    let mut l = match r {
        Ok(l) => l,
        Err(e) => return neg(e),
    };
    if hw_size_align > PAGE_SIZE {
        if !hw_size_align.is_power_of_two() {
            return neg(Errno::EINVAL);
        }
        match l.size.checked_add(hw_size_align - 1) {
            Some(s) => l.size = s & !(hw_size_align - 1),
            None => return neg(Errno::EINVAL),
        }
    }
    a.pitch = l.pitch;
    a.size = l.size;
    0
}

/// Allocates a backing for an object, as `drm_gem_object_init` did for
/// backing 0, and charges it to the device.
pub(crate) fn new_backing(r: &GemRef<FObj>) -> Result<SysMem, (Errno, ())> {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    let core = r.object().core();
    let gem = core.device().and_then(|d| d.gem().cloned()).ok_or((Errno::ENODEV, ()))?;
    let size = r.object().size();
    let res = gem.reserve(size, &ctx).map_err(|e| (e, ()))?;
    let mem = gem.sys_mem(size, &ctx).map_err(|e| (e, ()))?;
    let segs = mem.sg_len();
    gem.charge_reserved(res, segs);
    Ok(mem)
}
