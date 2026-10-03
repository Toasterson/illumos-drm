// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `drm_managed.h` over `drm_core::managed`. Every function checks that
//! the device is one the facade set up.

use core::ffi::{c_char, c_void};
use core::ptr;

use ddi::Errno;
use drm_core::managed;
use drm_facade_sys as f;

use crate::devcore::{DevCore, neg};

/// Whether `dev` is a device structure of the facade.
///
/// # Safety
/// `dev` is NULL or a device the caller holds a reference to.
unsafe fn ok(dev: *mut f::drm_device) -> bool {
    // SAFETY: the caller's contract.
    unsafe { DevCore::of(dev) }.is_some()
}

/// Whether an allocation with `gfp` may sleep.
fn sleeps(gfp: u32) -> bool {
    gfp & f::__GFP_DIRECT_RECLAIM != 0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn __drmm_add_action(
    dev: *mut f::drm_device,
    action: f::drmres_release_t,
    data: *mut c_void,
    name: *const c_char,
) -> i32 {
    // SAFETY: the caller's device; documented for thread context.
    unsafe {
        if !ok(dev) {
            return neg(Errno::EINVAL);
        }
        managed::add_action(dev, action, data, name)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn __drmm_add_action_or_reset(
    dev: *mut f::drm_device,
    action: f::drmres_release_t,
    data: *mut c_void,
    name: *const c_char,
) -> i32 {
    // SAFETY: as __drmm_add_action.
    unsafe {
        if !ok(dev) {
            if let Some(a) = action {
                a(dev, data);
            }
            return neg(Errno::EINVAL);
        }
        managed::add_action_or_reset(dev, action, data, name)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drmm_release_action(dev: *mut f::drm_device, action: f::drmres_release_t, data: *mut c_void) {
    // SAFETY: as __drmm_add_action.
    unsafe {
        if ok(dev) {
            managed::release_action(dev, action, data);
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drmm_kmalloc(dev: *mut f::drm_device, size: usize, gfp: f::gfp_t) -> *mut c_void {
    // SAFETY: the caller's device; with __GFP_DIRECT_RECLAIM documented for
    // thread context, any context otherwise.
    unsafe {
        if !ok(dev) {
            return ptr::null_mut();
        }
        managed::kmalloc(dev, size, gfp & f::__GFP_ZERO != 0, sleeps(gfp), c"drmm_kmalloc".as_ptr())
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drmm_kstrdup(dev: *mut f::drm_device, s: *const c_char, gfp: f::gfp_t) -> *mut c_char {
    // SAFETY: as drmm_kmalloc.
    unsafe {
        if !ok(dev) {
            return ptr::null_mut();
        }
        managed::kstrdup(dev, s, sleeps(gfp))
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drmm_kfree(dev: *mut f::drm_device, data: *mut c_void) {
    // SAFETY: the caller's device.
    unsafe {
        if ok(dev) {
            managed::kfree(dev, data);
        }
    }
}
