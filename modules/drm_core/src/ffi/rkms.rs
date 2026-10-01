// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `drm_rkms.h`: the probe sink, framebuffers, the plane check and vblanks
//! of the core's R3 mode setting.

use alloc::sync::Arc;
use core::ffi::c_void;

use ddi::Errno;
use drm_core::kms::{PlaneGeometry, Probe, check_plane_geometry};
use drm_facade_sys as f;

use crate::ckms::{Fb, mode_from_c, status_from_c};
use crate::devcore::{DevCore, neg};

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_rkms_layout_set(
    sink: *mut f::drm_rkms_layout_sink,
    layout: *const f::drm_rkms_layout,
) -> i32 {
    if sink.is_null() || layout.is_null() {
        return neg(Errno::EINVAL);
    }
    // SAFETY: the sink ckms::layout passed to the running callback.
    let s = unsafe { &mut *(sink as *mut Option<drm_core::kms::Layout>) };
    if s.is_some() {
        return neg(Errno::EINVAL);
    }
    // SAFETY: the driver's layout with arrays of its counts.
    *s = Some(unsafe { crate::ckms::layout_from_c(&*layout) });
    0
}

/// # Safety
/// `p` is the probe sink the facade passed to the running probe callback.
unsafe fn probe<'a>(p: *mut f::drm_rkms_probe) -> Option<&'a mut Probe> {
    // SAFETY: the caller's contract (ckms::probe passes a &mut Probe).
    if p.is_null() { None } else { Some(unsafe { &mut *(p as *mut Probe) }) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_rkms_probe_set_status(p: *mut f::drm_rkms_probe, status: u32) {
    // SAFETY: as probe().
    if let Some(p) = unsafe { probe(p) } {
        p.status = status_from_c(status);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_rkms_probe_set_edid(p: *mut f::drm_rkms_probe, edid: *const c_void, len: usize) -> i32 {
    // SAFETY: as probe().
    let Some(p) = (unsafe { probe(p) }) else { return neg(Errno::EINVAL) };
    if edid.is_null() || len == 0 {
        p.edid = None;
        return 0;
    }
    // SAFETY: the caller's buffer of len bytes.
    let b = unsafe { core::slice::from_raw_parts(edid as *const u8, len) };
    p.edid = Some(b.to_vec());
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_rkms_probe_add_mode(p: *mut f::drm_rkms_probe, mode: *const f::drm_rkms_mode) -> i32 {
    // SAFETY: as probe().
    let Some(p) = (unsafe { probe(p) }) else { return neg(Errno::EINVAL) };
    if mode.is_null() {
        return neg(Errno::EINVAL);
    }
    // SAFETY: the caller's mode.
    match mode_from_c(unsafe { &*mode }) {
        Ok(m) => {
            p.modes.push(m);
            0
        }
        Err(e) => neg(e),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_rkms_probe_set_size_mm(p: *mut f::drm_rkms_probe, width_mm: u32, height_mm: u32) {
    // SAFETY: as probe().
    if let Some(p) = unsafe { probe(p) } {
        p.width_mm = width_mm;
        p.height_mm = height_mm;
    }
}

/// # Safety
/// `fb` is NULL or a framebuffer the facade handed out, alive (borrowed
/// during a callback or held with `drm_rkms_fb_get`).
unsafe fn fb<'a>(fb: *const f::drm_rkms_fb) -> Option<&'a Fb> {
    // SAFETY: the caller's contract: the pointer is Arc::as_ptr of a live
    // framebuffer.
    if fb.is_null() { None } else { Some(unsafe { &*(fb as *const Fb) }) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_rkms_fb_get(p: *mut f::drm_rkms_fb) -> *mut f::drm_rkms_fb {
    if !p.is_null() {
        // SAFETY: a live framebuffer (see fb()).
        unsafe { Arc::increment_strong_count(p as *const Fb) };
    }
    p
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_rkms_fb_put(p: *mut f::drm_rkms_fb) {
    if !p.is_null() {
        // SAFETY: a reference taken with drm_rkms_fb_get; thread context.
        unsafe { Arc::decrement_strong_count(p as *const Fb) };
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_rkms_fb_info(p: *const f::drm_rkms_fb, info: *mut f::drm_rkms_fb_info) {
    // SAFETY: see fb().
    let (Some(b), false) = (unsafe { fb(p) }, info.is_null()) else { return };
    let i = f::drm_rkms_fb_info {
        id: b.id,
        width: b.width,
        height: b.height,
        format: b.format,
        modifier: b.modifier,
        flags: b.flags,
        pitches: b.pitches,
        offsets: b.offsets,
        hot_x: b.hot_x,
        hot_y: b.hot_y,
        cpp0: b.info().map_or(0, |fi| u32::from(fi.cpp[0])),
        num_objects: b.objs.iter().filter(|o| o.is_some()).count() as u32,
    };
    // SAFETY: the caller's structure.
    unsafe { *info = i };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_rkms_fb_object(p: *const f::drm_rkms_fb, i: u32) -> *mut f::drm_gem_object {
    // SAFETY: see fb().
    let Some(b) = (unsafe { fb(p) }) else { return core::ptr::null_mut() };
    match b.objs.get(i as usize).and_then(Option::as_ref) {
        Some(r) => r.object().base(),
        None => core::ptr::null_mut(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_rkms_check_plane_helper(
    new_state: *const f::drm_rkms_plane_state,
    crtc_state: *const f::drm_rkms_crtc_state,
    can_position: bool,
    can_scale: bool,
) -> i32 {
    if new_state.is_null() {
        return neg(Errno::EINVAL);
    }
    // SAFETY: the caller's states.
    let n = unsafe { &*new_state };
    let mode = if crtc_state.is_null() {
        None
    } else {
        // SAFETY: as above.
        let c = unsafe { &*crtc_state };
        if c.enable && c.has_mode {
            match mode_from_c(&c.mode) {
                Ok(m) => Some(m),
                Err(e) => return neg(e),
            }
        } else {
            None
        }
    };
    let g = PlaneGeometry {
        crtc_x: n.crtc_x,
        crtc_y: n.crtc_y,
        crtc_w: n.crtc_w,
        crtc_h: n.crtc_h,
        src_w: n.src_w,
        src_h: n.src_h,
    };
    match check_plane_geometry(!n.fb.is_null(), mode.as_ref(), &g, can_position, can_scale) {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_rkms_vblank(dev: *mut f::drm_device, crtc: u32, time_ns: i64) {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller's device.
    let Some(d) = (unsafe { DevCore::of(dev) }).and_then(|c| c.device()) else { return };
    if let Some(k) = d.kms() {
        k.vblank_handle().vblank(crtc as usize, time_ns, &ctx);
    }
}
