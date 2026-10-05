// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `drm_lkms.h` (facade 2.0): the start of a Linux-KMS driver's mode
//! setting, and the event functions of its KMS code over the device's
//! event queues (`drm_core::lkms`).
//!
//! The KMS code calls them with its `dev->event_lock` held, in thread
//! context and in the driver's low-level interrupt handler. Nothing on
//! these paths blocks, allocates or calls back into C: the file is found
//! from the marks in its structure (`file_of`), the device through its
//! publication (atomic counts), and the queue under the device's event
//! lock, an adaptive mutex no path holds while calling C code
//! (`drm_core::lkms::dev`).

use core::ptr::NonNull;

use ddi::Errno;
use drm_core::lkms::LkmsHooks;
use drm_core::lkms::events::{Delivery, HEADER};
use drm_facade_sys as f;

use crate::cdriver::{CDriver, file_of};
use crate::devcore::{DevCore, neg};

/// Starts the mode setting of a Linux-KMS driver's registered device; see
/// `drm_lkms.h`. The device's features (`DRIVER_MODESET`, `DRIVER_ATOMIC`)
/// are read now, after the driver has settled them.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_dev_illumos_lkms_register(dev: *mut f::drm_device) -> i32 {
    // SAFETY: documented for thread context (the driver's attach).
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller holds a reference to its device.
    let Some(core) = (unsafe { DevCore::of(dev) }) else { return neg(Errno::EINVAL) };
    let Some(d) = core.device() else { return neg(Errno::EINVAL) };
    {
        let mut s = core.setup.lock();
        if s.unplugged {
            return neg(Errno::EINVAL);
        }
        if s.lkms_called {
            return neg(Errno::EBUSY);
        }
        // SAFETY: the caller's live device.
        let driver_features = unsafe { (*dev).driver_features };
        if core.lkms.is_none() || driver_features & f::DRIVER_MODESET == 0 {
            s.lkms_called = true;
            return 0;
        }
        // Files opened now would have no KMS state: refused, and not
        // counted as the call (the driver's attach, where the call
        // belongs, has no open files; lkms_start checks again).
        if d.open_files() != 0 {
            return neg(Errno::EBUSY);
        }
        s.lkms_called = true;
    }
    // SAFETY: as above.
    let driver_features = unsafe { (*dev).driver_features };
    let features = crate::devcore::features(core.table, driver_features);
    let drv = CDriver::new(NonNull::from(core));
    let mut reg = ::core::mem::take(&mut core.setup.lock().lkms_reg);
    let r = d.lkms_start(features, &drv as &dyn LkmsHooks, &mut reg, &ctx);
    core.setup.lock().lkms_reg = reg;
    match r {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_event_reserve(file: *mut f::drm_file, size: usize) -> i32 {
    // SAFETY: the caller's open file, or a pointer the marks refuse.
    let Some((c, core)) = (unsafe { file_of(file) }) else { return neg(Errno::EINVAL) };
    let Some(d) = core.device() else { return neg(Errno::ENODEV) };
    let Some(q) = d.lkms_events() else { return neg(Errno::EINVAL) };
    match q.reserve(c, size) {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_event_unreserve(file: *mut f::drm_file, size: usize) {
    // SAFETY: as above.
    let Some((c, core)) = (unsafe { file_of(file) }) else { return };
    let Some(d) = core.device() else { return };
    if let Some(q) = d.lkms_events() {
        q.unreserve(c, size);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_illumos_event_deliver(file: *mut f::drm_file, event: *const f::drm_event) {
    if event.is_null() {
        return;
    }
    // SAFETY: as above.
    let Some((c, core)) = (unsafe { file_of(file) }) else { return };
    let Some(d) = core.device() else { return };
    let Some(q) = d.lkms_events() else { return };
    // The length is the header's (struct drm_event: type, then length);
    // a length outside the header and the budget is refused before
    // anything is read beyond the header.
    let p = event as *const u8;
    let mut h = [0u8; HEADER];
    // SAFETY: the caller's event has at least its header.
    unsafe { core::ptr::copy_nonoverlapping(p, h.as_mut_ptr(), HEADER) };
    let len = u32::from_ne_bytes([h[4], h[5], h[6], h[7]]) as usize;
    if !(HEADER..=drm_core::kms::events::EVENT_SPACE).contains(&len) {
        // Counted as refused by the queue, which sees the header alone.
        let _ = q.deliver(c, &h);
        return;
    }
    // SAFETY: the caller's event is `length` bytes long (drm_lkms.h).
    let ev = unsafe { core::slice::from_raw_parts(p, len) };
    // A refused or dropped event is counted in the queue's statistics.
    let _: Delivery = q.deliver(c, ev);
}
