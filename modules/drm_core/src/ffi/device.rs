// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `drm_device.h` and the per-device settings of `drm_drv.h`.

use alloc::boxed::Box;
use core::ffi::{CStr, c_char, c_void};
use core::ptr::{self, NonNull};

use ddi::sys;
use ddi::{DmaAttr, Errno, IntrPriority, OwnNode};
use drm_core::driver::BUS_ID_MAX;
use drm_core::{Device, ModeConfig};
use drm_facade_sys as f;

use crate::cdriver::CDriver;
use crate::devcore::{DevCore, neg};

fn err_ptr(e: Errno) -> *mut c_void {
    neg(e) as isize as *mut c_void
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn __drm_dev_alloc(
    parent: *mut f::dev_info_t,
    driver: *const f::drm_driver,
    size: usize,
    offset: usize,
) -> *mut c_void {
    // SAFETY: documented for attach(9E), thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller passes its own node and a static table.
    match unsafe { DevCore::alloc(parent as *mut sys::dev_info, driver, size, offset, &ctx) } {
        Ok(p) => p.as_ptr().cast(),
        Err(e) => err_ptr(e),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_dev_alloc(driver: *const f::drm_driver, parent: *mut f::dev_info_t) -> *mut f::drm_device {
    // SAFETY: as __drm_dev_alloc.
    unsafe { __drm_dev_alloc(parent, driver, size_of::<f::drm_device>(), 0) }.cast()
}

/// The state of a device structure, for the settings before registration:
/// `-EINVAL` for a structure the facade did not set up, `-EBUSY` once it is
/// registered.
fn unregistered<'a>(dev: *mut f::drm_device) -> Result<&'a DevCore, Errno> {
    // SAFETY: the caller holds a reference to its device.
    let core = unsafe { DevCore::of(dev) }.ok_or(Errno::EINVAL)?;
    if core.setup.lock().registered {
        return Err(Errno::EBUSY);
    }
    Ok(core)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_dev_set_unique(dev: *mut f::drm_device, name: *const c_char) -> i32 {
    let core = match unregistered(dev) {
        Ok(c) => c,
        Err(e) => return neg(e),
    };
    if name.is_null() {
        return neg(Errno::EINVAL);
    }
    // SAFETY: the caller's NUL-terminated string.
    let b = unsafe { CStr::from_ptr(name) }.to_bytes();
    if b.len() > BUS_ID_MAX || core::str::from_utf8(b).is_err() {
        return neg(Errno::EINVAL);
    }
    let mut s = core.setup.lock();
    s.unique = [0; BUS_ID_MAX + 1];
    for (d, c) in s.unique.iter_mut().zip(b.iter()) {
        *d = *c;
    }
    s.unique_len = b.len();
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_dev_illumos_set_dma_attr(dev: *mut f::drm_device, attr: *const f::ddi_dma_attr_t) -> i32 {
    let core = match unregistered(dev) {
        Ok(c) => c,
        Err(e) => return neg(e),
    };
    if attr.is_null() {
        return neg(Errno::EINVAL);
    }
    // SAFETY: the caller's attributes, of the DDI's layout (opaque in the
    // facade's bindings).
    let raw = unsafe { *(attr as *const sys::ddi_dma_attr_t) };
    core.setup.lock().dma = Some(DmaAttr::from_raw(raw));
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_dev_illumos_set_fence_priority(dev: *mut f::drm_device, pri: u32) -> i32 {
    let core = match unregistered(dev) {
        Ok(c) => c,
        Err(e) => return neg(e),
    };
    // A high-level priority would need spin locks; FenceDomain refuses it
    // at registration, which then fails with EINVAL.
    let Some(p) = IntrPriority::new(pri) else { return neg(Errno::EINVAL) };
    core.setup.lock().fence_pri = Some(p);
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_dev_illumos_set_mode_config(
    dev: *mut f::drm_device,
    cfg: *const f::drm_illumos_mode_config,
) -> i32 {
    let core = match unregistered(dev) {
        Ok(c) => c,
        Err(e) => return neg(e),
    };
    if cfg.is_null() {
        return neg(Errno::EINVAL);
    }
    // SAFETY: the caller's structure.
    let c = unsafe { &*cfg };
    let m = ModeConfig {
        cursor_width: c.cursor_width,
        cursor_height: c.cursor_height,
        preferred_depth: c.preferred_depth,
        prefer_shadow: c.prefer_shadow,
        fb_modifiers: c.fb_modifiers,
    };
    // SAFETY: not registered; the driver's attach is the only user.
    unsafe { core.set_mode_config(m) };
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_dev_register(dev: *mut f::drm_device, flags: u64) -> i32 {
    // SAFETY: documented for attach(9E), thread context.
    let ctx = unsafe { super::sleepable() };
    let core = match unregistered(dev) {
        Ok(c) => c,
        Err(e) => return neg(e),
    };
    if flags != 0 {
        return neg(Errno::EINVAL);
    }
    if let Some(k) = &core.kms {
        k.bind(core);
    }
    // SAFETY: the driver passed its own node to drm_dev_alloc in its
    // attach(9E), which is still running (registration belongs there); the
    // node outlives the registration, which its detach(9E) ends.
    let node = unsafe { OwnNode::from_raw(core.dip.as_raw()) };
    let d = match Device::attach(node, CDriver::new(NonNull::from(core)), &ctx) {
        Ok(d) => Box::new(d),
        Err(_) => return neg(Errno::EINVAL),
    };
    let index = d.index();
    // SAFETY: the minor descriptors live in the state; nothing reads them
    // before the device is published.
    unsafe {
        let m = &mut *core.minors.get();
        m[0] = f::drm_minor {
            index: index as i32,
            type_: f::DRM_MINOR_PRIMARY as i32,
            kdev: core.dip.as_raw() as *mut f::dev_info_t,
            dev,
        };
        m[1] = f::drm_minor {
            index: (drm_core::minor::RENDER_BASE + index) as i32,
            type_: f::DRM_MINOR_RENDER as i32,
            kdev: core.dip.as_raw() as *mut f::dev_info_t,
            dev,
        };
        (*dev).primary = ptr::addr_of_mut!(m[0]);
        (*dev).render = if d.has_render() { ptr::addr_of_mut!(m[1]) } else { ptr::null_mut() };
        (*dev).unique = unique_ptr(core);
        (*dev).vma_offset_manager =
            d.gem().map_or(ptr::null_mut(), |g| drm_core::karc::KArc::as_ptr(g).as_ptr().cast());
        (*dev).registered = true;
    }
    let raw = Box::into_raw(d);
    core.publish(raw);
    // SAFETY: just published, alive until unregistered.
    crate::table::publish(unsafe { &*raw });
    core.setup.lock().registered = true;
    0
}

/// The bus ID as the device reports it, for `drm_device.unique`.
fn unique_ptr(core: &DevCore) -> *mut c_char {
    let mut s = core.setup.lock();
    if s.unique_len == 0 {
        let mut b = drm_core::BusId::new();
        let dev = core.dip;
        if let Ok(name) = dev.driver_name().to_str()
            && core::fmt::Write::write_fmt(&mut b, format_args!("pseudo:{}@{}", name, dev.instance().0)).is_ok()
        {
            let bytes = b.as_bytes();
            for (d, c) in s.unique.iter_mut().zip(bytes.iter()) {
                *d = *c;
            }
            s.unique_len = bytes.len().min(BUS_ID_MAX);
        }
    }
    s.unique.as_mut_ptr().cast()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_dev_unregister(dev: *mut f::drm_device) -> i32 {
    // SAFETY: the caller holds a reference to its device.
    let Some(core) = (unsafe { DevCore::of(dev) }) else { return neg(Errno::EINVAL) };
    // SAFETY: documented for detach(9E), thread context.
    unsafe { unregister(dev, core) }
}

/// The unregistration of `drm_dev_unregister`, also run by the last put of
/// an unplugged device ([`DevCore::put`]): 0, `-EBUSY` (the device stays
/// registered and working) or `-EINVAL` (not registered).
///
/// # Safety
/// `dev` is `core`'s device, alive; thread context.
pub(crate) unsafe fn unregister(dev: *mut f::drm_device, core: &DevCore) -> i32 {
    // SAFETY: the caller's contract.
    let ctx = unsafe { super::sleepable() };
    // The checks run on the published device, which stays where it is and
    // keeps working if they refuse: nothing moves before the detach is
    // certain.
    let index = {
        let Some(d) = core.device() else { return neg(Errno::EINVAL) };
        if d.detach_check(&ctx).is_err() {
            return neg(Errno::EBUSY);
        }
        d.index()
    };
    // Out of the device table (no entry point finds it any more), then out
    // of the facade (waiting for every call that uses it), then torn down
    // and freed.
    crate::table::withdraw(index);
    let raw = core.withdraw();
    if raw.is_null() {
        return neg(Errno::EINVAL);
    }
    // SAFETY: published by drm_dev_register from a box; withdrawn from both
    // places and no guard holds it.
    let d: Box<Device<CDriver>> = unsafe { Box::from_raw(raw) };
    d.detach_teardown(&ctx);
    drop(d);
    // SAFETY: the structure stays until drm_dev_put.
    unsafe {
        (*dev).registered = false;
        (*dev).primary = ptr::null_mut();
        (*dev).render = ptr::null_mut();
        (*dev).vma_offset_manager = ptr::null_mut();
    }
    core.setup.lock().registered = false;
    0
}

/// Facade 1.1: `drm_dev_put` that reports what became of the device: 0
/// (released, or still referenced elsewhere) or `-EBUSY` (the last
/// reference of an unplugged device whose state the core could not tear
/// down after the driver's release: the device stays, leaked).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_dev_illumos_put(dev: *mut f::drm_device) -> i32 {
    // SAFETY: the caller holds a reference, which it gives up.
    match unsafe { DevCore::of(dev) } {
        // SAFETY: as above.
        Some(core) => match unsafe { DevCore::put(core) } {
            Ok(()) => 0,
            Err(e) => neg(e),
        },
        None => neg(Errno::EINVAL),
    }
}

/// Linux's `drm_dev_unplug` before a driver tears itself down: refuses
/// with `-EBUSY` while user space holds the device (open files, user
/// mappings), else takes the device out of the device table, so that no
/// new open reaches it, and returns 0. The device stays registered for the
/// driver's own calls until `drm_dev_unregister`. 0 for a device that is
/// not registered or unplugged already. Thread context (detach(9E)).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_dev_illumos_unplug(dev: *mut f::drm_device) -> i32 {
    // SAFETY: the caller holds a reference to its device.
    let Some(core) = (unsafe { DevCore::of(dev) }) else { return neg(Errno::EINVAL) };
    let Some(d) = core.device() else { return 0 };
    // Out of the table first, so that no open slips in between the check
    // and the withdrawal; back in if the check refuses.
    crate::table::withdraw(d.index());
    if d.user_check().is_err() {
        crate::table::publish(&d);
        return neg(Errno::EBUSY);
    }
    core.setup.lock().unplugged = true;
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_dev_get(dev: *mut f::drm_device) {
    // SAFETY: the caller holds a reference.
    if let Some(core) = unsafe { DevCore::of(dev) } {
        core.get();
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_dev_put(dev: *mut f::drm_device) {
    // SAFETY: the caller holds a reference, which it gives up.
    if let Some(core) = unsafe { DevCore::of(dev) } {
        // SAFETY: as above.
        let _ = unsafe { DevCore::put(core) };
    }
}
