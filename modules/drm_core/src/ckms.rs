// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The core's mode setting (R3) for a driver with `illumos_rkms`: the
//! core's `KmsDriver` hooks forwarded to the driver's `drm_rkms_funcs`,
//! with the states converted to the facade's plain structures.

use alloc::sync::Arc;
use alloc::vec::Vec;
use core::ptr;

use ddi::{Errno, Sleepable};
use drm_core::gem::GemRef;
use drm_core::kms::modes::{DisplayMode, ModeLimits};
use drm_core::kms::objects::{FbRef, Framebuffer};
use drm_core::kms::{
    ConnectorDesc, ConnectorState, ConnectorStatus, CrtcDesc, CrtcState, DirtyRect, EncoderDesc, KmsDriver, Layout,
    PlaneDesc, PlaneState, PlaneType, Probe, StateView,
};
use drm_facade_sys as f;
use drm_uapi as uapi;

use crate::devcore::{DevCore, from_neg};
use crate::fobj::FObj;

/// The framebuffer type of the facade's objects.
pub(crate) type Fb = Framebuffer<GemRef<FObj>>;

/// The mode setting of one device.
pub(crate) struct CKms {
    funcs: &'static f::drm_rkms_funcs,
    /// The device structure, set when the device state is complete.
    ddev: core::sync::atomic::AtomicPtr<f::drm_device>,
}

impl CKms {
    pub(crate) fn new(funcs: &'static f::drm_rkms_funcs) -> CKms {
        CKms { funcs, ddev: core::sync::atomic::AtomicPtr::new(ptr::null_mut()) }
    }

    /// Records the device structure the callbacks receive.
    pub(crate) fn bind(&self, core: &DevCore) {
        self.ddev.store(core.ddev(), core::sync::atomic::Ordering::Release);
    }

    fn dev(&self) -> *mut f::drm_device {
        self.ddev.load(core::sync::atomic::Ordering::Acquire)
    }
}

/// A facade mode from a core mode.
pub(crate) fn mode_to_c(m: &DisplayMode) -> f::drm_rkms_mode {
    let u = m.to_umode(true);
    let mut name = [0 as core::ffi::c_char; 32];
    for (d, s) in name.iter_mut().zip(u.name.iter()) {
        *d = *s;
    }
    f::drm_rkms_mode {
        clock: u.clock,
        hdisplay: u.hdisplay,
        hsync_start: u.hsync_start,
        hsync_end: u.hsync_end,
        htotal: u.htotal,
        hskew: u.hskew,
        vdisplay: u.vdisplay,
        vsync_start: u.vsync_start,
        vsync_end: u.vsync_end,
        vtotal: u.vtotal,
        vscan: u.vscan,
        vrefresh: u.vrefresh,
        flags: u.flags,
        type_: u.type_,
        name,
    }
}

/// A core mode from a facade mode; `EINVAL` for a mode the core rejects.
pub(crate) fn mode_from_c(m: &f::drm_rkms_mode) -> Result<DisplayMode, Errno> {
    let mut name = [0 as core::ffi::c_char; 32];
    for (d, s) in name.iter_mut().zip(m.name.iter()) {
        *d = *s;
    }
    let u = uapi::drm_mode_modeinfo {
        clock: m.clock,
        hdisplay: m.hdisplay,
        hsync_start: m.hsync_start,
        hsync_end: m.hsync_end,
        htotal: m.htotal,
        hskew: m.hskew,
        vdisplay: m.vdisplay,
        vsync_start: m.vsync_start,
        vsync_end: m.vsync_end,
        vtotal: m.vtotal,
        vscan: m.vscan,
        vrefresh: m.vrefresh,
        flags: m.flags,
        type_: m.type_,
        name,
    };
    DisplayMode::from_umode(&u)
}

fn idx(v: Option<usize>) -> i32 {
    v.and_then(|i| i32::try_from(i).ok()).unwrap_or(-1)
}

pub(crate) fn crtc_to_c(s: &CrtcState) -> f::drm_rkms_crtc_state {
    f::drm_rkms_crtc_state {
        active: s.active,
        enable: s.enable(),
        has_mode: s.mode.is_some(),
        mode: s.mode.as_ref().map(mode_to_c).unwrap_or_default(),
    }
}

/// A facade plane state; `fb` borrows the framebuffer for the call.
pub(crate) fn plane_to_c(s: &PlaneState<GemRef<FObj>>) -> f::drm_rkms_plane_state {
    f::drm_rkms_plane_state {
        crtc: idx(s.crtc),
        fb: s.fb.as_ref().map(|fb| Arc::as_ptr(fb) as *mut f::drm_rkms_fb).unwrap_or(ptr::null_mut()),
        crtc_x: s.crtc_x,
        crtc_y: s.crtc_y,
        crtc_w: s.crtc_w,
        crtc_h: s.crtc_h,
        src_x: s.src_x,
        src_y: s.src_y,
        src_w: s.src_w,
        src_h: s.src_h,
    }
}

fn connector_to_c(s: &ConnectorState) -> f::drm_rkms_connector_state {
    f::drm_rkms_connector_state { crtc: idx(s.crtc), encoder: idx(s.encoder), link_status: s.link_status }
}

/// Copies a driver array (empty for NULL or 0).
///
/// # Safety
/// `p` points to `n` elements or is NULL.
unsafe fn slice<'a, T>(p: *const T, n: u32) -> &'a [T] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        // SAFETY: the caller's contract.
        unsafe { core::slice::from_raw_parts(p, n as usize) }
    }
}

fn plane_type(t: u32) -> PlaneType {
    match t {
        f::DRM_RKMS_PLANE_PRIMARY => PlaneType::Primary,
        f::DRM_RKMS_PLANE_CURSOR => PlaneType::Cursor,
        _ => PlaneType::Overlay,
    }
}

/// A core layout from a driver's.
///
/// # Safety
/// The arrays of `l` hold the counts it gives.
pub(crate) unsafe fn layout_from_c(l: &f::drm_rkms_layout) -> Layout {
    let mut out = Layout {
        min_width: l.min_width,
        max_width: l.max_width,
        min_height: l.min_height,
        max_height: l.max_height,
        planes: Vec::new(),
        crtcs: Vec::new(),
        encoders: Vec::new(),
        connectors: Vec::new(),
    };
    // SAFETY: the caller's contract.
    unsafe {
        for p in slice(l.planes, l.num_planes) {
            out.planes.push(PlaneDesc {
                typ: plane_type(p.type_),
                possible_crtcs: p.possible_crtcs,
                formats: slice(p.formats, p.num_formats).to_vec(),
                modifiers: slice(p.modifiers, p.num_modifiers).to_vec(),
            });
        }
        for c in slice(l.crtcs, l.num_crtcs) {
            out.crtcs.push(CrtcDesc {
                primary: c.primary as usize,
                cursor: usize::try_from(c.cursor).ok(),
                gamma_size: c.gamma_size,
            });
        }
        for e in slice(l.encoders, l.num_encoders) {
            out.encoders.push(EncoderDesc {
                typ: e.type_,
                possible_crtcs: e.possible_crtcs,
                possible_clones: e.possible_clones,
            });
        }
        for c in slice(l.connectors, l.num_connectors) {
            out.connectors.push(ConnectorDesc {
                typ: c.type_,
                encoders: c.encoders,
                limits: ModeLimits {
                    max_width: u16::try_from(c.limits.max_width).unwrap_or(u16::MAX),
                    max_height: u16::try_from(c.limits.max_height).unwrap_or(u16::MAX),
                    max_clock: c.limits.max_clock,
                    interlace: c.limits.interlace,
                    dblscan: c.limits.dblscan,
                },
            });
        }
    }
    out
}

impl KmsDriver<GemRef<FObj>> for CKms {
    fn layout(&self) -> Layout {
        let mut sink: Option<Layout> = None;
        if let Some(cb) = self.funcs.layout {
            // SAFETY: the driver's callback with its device and a sink valid
            // during the call (drm_rkms_layout_set copies into it).
            unsafe { cb(self.dev(), ptr::addr_of_mut!(sink).cast()) };
        }
        sink.unwrap_or(Layout {
            min_width: 0,
            max_width: 0,
            min_height: 0,
            max_height: 0,
            planes: Vec::new(),
            crtcs: Vec::new(),
            encoders: Vec::new(),
            connectors: Vec::new(),
        })
    }

    fn probe(&self, connector: usize, _ctx: &Sleepable) -> Probe {
        let mut p = Probe::default();
        let Some(cb) = self.funcs.probe else { return p };
        let Ok(c) = u32::try_from(connector) else { return p };
        // SAFETY: the driver's callback; the probe sink is valid during it.
        unsafe { cb(self.dev(), c, ptr::addr_of_mut!(p).cast()) };
        p
    }

    fn format_mod_supported(&self, plane: usize, format: u32, modifier: u64) -> bool {
        match (self.funcs.format_mod_supported, u32::try_from(plane)) {
            // SAFETY: the driver's callback.
            (Some(cb), Ok(p)) => unsafe { cb(self.dev(), p, format, modifier) },
            _ => true,
        }
    }

    fn check_plane(&self, plane: usize, new: &PlaneState<GemRef<FObj>>, crtc: Option<&CrtcState>) -> Result<(), Errno> {
        let Some(cb) = self.funcs.check_plane else { return Ok(()) };
        let n = plane_to_c(new);
        let c = crtc.map(crtc_to_c);
        let cp = c.as_ref().map_or(ptr::null(), |c| c as *const _);
        // SAFETY: the driver's callback with states valid during the call.
        from_neg(unsafe { cb(self.dev(), plane as u32, &n, cp) })
    }

    fn check_crtc(&self, crtc: usize, new: &CrtcState, _view: &StateView<'_, GemRef<FObj>>) -> Result<(), Errno> {
        let Some(cb) = self.funcs.check_crtc else { return Ok(()) };
        let n = crtc_to_c(new);
        // SAFETY: as above; the view is not readable in facade version 0.
        from_neg(unsafe { cb(self.dev(), crtc as u32, &n, ptr::null()) })
    }

    fn check_connector(&self, connector: usize, new: &ConnectorState) -> Result<(), Errno> {
        let Some(cb) = self.funcs.check_connector else { return Ok(()) };
        let n = connector_to_c(new);
        // SAFETY: as above.
        from_neg(unsafe { cb(self.dev(), connector as u32, &n) })
    }

    fn commit_crtc(&self, crtc: usize, old: &CrtcState, new: &CrtcState, _ctx: &Sleepable) {
        let Some(cb) = self.funcs.commit_crtc else { return };
        let (o, n) = (crtc_to_c(old), crtc_to_c(new));
        // SAFETY: as above.
        unsafe { cb(self.dev(), crtc as u32, &o, &n) };
    }

    fn commit_plane(
        &self,
        plane: usize,
        old: &PlaneState<GemRef<FObj>>,
        new: &PlaneState<GemRef<FObj>>,
        _ctx: &Sleepable,
    ) {
        let Some(cb) = self.funcs.commit_plane else { return };
        let (o, n) = (plane_to_c(old), plane_to_c(new));
        // SAFETY: as above.
        unsafe { cb(self.dev(), plane as u32, &o, &n) };
    }

    fn commit_connector(&self, connector: usize, old: &ConnectorState, new: &ConnectorState, _ctx: &Sleepable) {
        let Some(cb) = self.funcs.commit_connector else { return };
        let (o, n) = (connector_to_c(old), connector_to_c(new));
        // SAFETY: as above.
        unsafe { cb(self.dev(), connector as u32, &o, &n) };
    }

    fn set_gamma(&self, crtc: usize, r: &[u16], g: &[u16], b: &[u16]) {
        let Some(cb) = self.funcs.set_gamma else { return };
        let n = r.len().min(g.len()).min(b.len());
        // SAFETY: three arrays of at least n entries.
        unsafe { cb(self.dev(), crtc as u32, r.as_ptr(), g.as_ptr(), b.as_ptr(), n as u32) };
    }

    fn dirty_fb(
        &self,
        fb: &FbRef<GemRef<FObj>>,
        planes: &[usize],
        clips: &[DirtyRect],
        _ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let Some(cb) = self.funcs.dirty_fb else { return Err(drm_core::driver::ENOSYS) };
        let ps: Vec<u32> = planes.iter().map(|p| *p as u32).collect();
        let cs: Vec<f::drm_rkms_rect> =
            clips.iter().map(|c| f::drm_rkms_rect { x1: c.x1, y1: c.y1, x2: c.x2, y2: c.y2 }).collect();
        let fbp = Arc::as_ptr(fb) as *mut f::drm_rkms_fb;
        // SAFETY: the driver's callback with arrays valid during the call.
        from_neg(unsafe { cb(self.dev(), fbp, ps.as_ptr(), ps.len() as u32, cs.as_ptr(), cs.len() as u32) })
    }
}

/// The connector status of a facade value.
pub(crate) fn status_from_c(s: u32) -> ConnectorStatus {
    match s {
        f::DRM_RKMS_CONNECTED => ConnectorStatus::Connected,
        f::DRM_RKMS_DISCONNECTED => ConnectorStatus::Disconnected,
        _ => ConnectorStatus::Unknown,
    }
}
