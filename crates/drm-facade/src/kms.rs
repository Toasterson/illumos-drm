// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Mode setting with the core's R3 implementation (`drm_rkms.h`): a driver
//! describes its display ([`Layout`]), reports what is connected
//! ([`Probe`]), and checks and commits the state of its CRTCs, planes and
//! connectors ([`KmsDriver`]); the core does everything else.

use alloc::vec::Vec;
use core::ffi::c_int;
use core::ptr::{self, NonNull};

use ddi::Errno;
use drm_core::kms::modes::{DisplayMode, ModeLimits};
use drm_facade_sys as sys;
use drm_uapi as uapi;

use crate::device::{Dev, Driver};
use crate::gem::{GemObject, GemRef};
use crate::{check, neg};

pub use drm_core::kms::{edid, formats, modes, soft};

/// What kind of plane.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PlaneType {
    /// An overlay plane.
    Overlay,
    /// The primary plane of a CRTC.
    Primary,
    /// The cursor plane of a CRTC.
    Cursor,
}

/// A plane of the layout.
#[derive(Clone, Debug)]
pub struct PlaneDesc {
    /// Its kind.
    pub typ: PlaneType,
    /// The CRTCs it can show on (bit mask).
    pub possible_crtcs: u32,
    /// Its formats (fourcc).
    pub formats: Vec<u32>,
    /// Its format modifiers.
    pub modifiers: Vec<u64>,
}

/// A CRTC of the layout.
#[derive(Clone, Debug)]
pub struct CrtcDesc {
    /// Its primary plane.
    pub primary: usize,
    /// Its cursor plane.
    pub cursor: Option<usize>,
    /// Entries of its gamma table (0 for none).
    pub gamma_size: u32,
}

/// An encoder of the layout.
#[derive(Clone, Debug)]
pub struct EncoderDesc {
    /// `DRM_MODE_ENCODER_*`.
    pub typ: u32,
    /// The CRTCs it can be driven by.
    pub possible_crtcs: u32,
    /// The encoders it can clone.
    pub possible_clones: u32,
}

/// A connector of the layout.
#[derive(Clone, Debug)]
pub struct ConnectorDesc {
    /// `DRM_MODE_CONNECTOR_*`.
    pub typ: u32,
    /// Its encoders (bit mask).
    pub encoders: u32,
    /// The modes it can take.
    pub limits: ModeLimits,
}

/// The display's objects, fixed for the device's life.
#[derive(Clone, Debug, Default)]
pub struct Layout {
    /// The smallest framebuffer width.
    pub min_width: u32,
    /// The largest framebuffer width.
    pub max_width: u32,
    /// The smallest framebuffer height.
    pub min_height: u32,
    /// The largest framebuffer height.
    pub max_height: u32,
    /// The planes.
    pub planes: Vec<PlaneDesc>,
    /// The CRTCs.
    pub crtcs: Vec<CrtcDesc>,
    /// The encoders.
    pub encoders: Vec<EncoderDesc>,
    /// The connectors.
    pub connectors: Vec<ConnectorDesc>,
}

/// Whether something is connected.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ConnectorStatus {
    /// Connected.
    Connected,
    /// Disconnected.
    Disconnected,
    /// Unknown.
    #[default]
    Unknown,
}

/// What a probe found.
#[derive(Clone, Debug, Default)]
pub struct Probe {
    /// The status.
    pub status: ConnectorStatus,
    /// The EDID, if any (the core takes the modes from it).
    pub edid: Option<Vec<u8>>,
    /// Modes besides the EDID's.
    pub modes: Vec<DisplayMode>,
    /// The physical width in mm.
    pub width_mm: u32,
    /// The physical height in mm.
    pub height_mm: u32,
}

/// A damaged rectangle (x2, y2 exclusive).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DirtyRect {
    /// Left.
    pub x1: u32,
    /// Top.
    pub y1: u32,
    /// Right, exclusive.
    pub x2: u32,
    /// Bottom, exclusive.
    pub y2: u32,
}

/// The fixed properties of a framebuffer.
pub type FbInfo = sys::drm_rkms_fb_info;

/// A counted reference to a framebuffer. Thread context.
pub struct Fb {
    p: NonNull<sys::drm_rkms_fb>,
    info: FbInfo,
}

// SAFETY: framebuffer references are counted atomically by the core.
unsafe impl Send for Fb {}
// SAFETY: as above.
unsafe impl Sync for Fb {}

impl Fb {
    /// Takes a reference to a framebuffer a callback passed.
    ///
    /// # Safety
    /// `p` is NULL or a framebuffer of the running callback.
    unsafe fn from_borrowed(p: *mut sys::drm_rkms_fb) -> Option<Fb> {
        let p = NonNull::new(p)?;
        // SAFETY: a live framebuffer.
        unsafe { sys::drm_rkms_fb_get(p.as_ptr()) };
        // SAFETY: plain data.
        let mut info: FbInfo = unsafe { core::mem::zeroed() };
        // SAFETY: as above.
        unsafe { sys::drm_rkms_fb_info(p.as_ptr(), &mut info) };
        Some(Fb { p, info })
    }

    /// The object of plane `i`, if it is of type `T`.
    pub fn object<T: GemObject>(&self, i: u32) -> Option<GemRef<T>> {
        // SAFETY: a live framebuffer; the object is borrowed from it.
        unsafe { GemRef::from_borrowed(sys::drm_rkms_fb_object(self.p.as_ptr(), i)) }
    }

    /// Whether two references name the same framebuffer.
    pub fn ptr_eq(a: &Fb, b: &Fb) -> bool {
        a.p == b.p
    }

    fn raw(&self) -> *mut sys::drm_rkms_fb {
        self.p.as_ptr()
    }
}

impl Clone for Fb {
    fn clone(&self) -> Fb {
        // SAFETY: a live framebuffer.
        unsafe { sys::drm_rkms_fb_get(self.p.as_ptr()) };
        Fb { p: self.p, info: self.info }
    }
}

impl Drop for Fb {
    fn drop(&mut self) {
        // SAFETY: this reference.
        unsafe { sys::drm_rkms_fb_put(self.p.as_ptr()) };
    }
}

impl core::ops::Deref for Fb {
    type Target = FbInfo;
    fn deref(&self) -> &FbInfo {
        &self.info
    }
}

/// The state of a CRTC.
#[derive(Clone, Debug, Default)]
pub struct CrtcState {
    /// It scans out.
    pub active: bool,
    /// A mode is set.
    pub enable: bool,
    /// The mode.
    pub mode: Option<DisplayMode>,
}

/// The state of a plane.
#[derive(Clone, Default)]
pub struct PlaneState {
    /// The CRTC it shows on.
    pub crtc: Option<usize>,
    /// The framebuffer it shows.
    pub fb: Option<Fb>,
    /// Destination left edge.
    pub crtc_x: i32,
    /// Destination top edge.
    pub crtc_y: i32,
    /// Destination width.
    pub crtc_w: u32,
    /// Destination height.
    pub crtc_h: u32,
    /// Source left edge, 16.16.
    pub src_x: u32,
    /// Source top edge, 16.16.
    pub src_y: u32,
    /// Source width, 16.16.
    pub src_w: u32,
    /// Source height, 16.16.
    pub src_h: u32,
}

/// The state of a connector.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ConnectorState {
    /// Its CRTC.
    pub crtc: Option<usize>,
    /// Its encoder.
    pub encoder: Option<usize>,
    /// The link status property.
    pub link_status: u32,
}

/// A driver's display; the semantics of R3's `KmsDriver`. Every method
/// runs in thread context.
pub trait KmsDriver: Send + Sync {
    /// The display's objects.
    fn layout(&self) -> Layout;
    /// What connector `connector` has.
    fn probe(&self, connector: usize) -> Probe;
    /// Whether plane `plane` takes `format` with `modifier`.
    fn format_mod_supported(&self, plane: usize, format: u32, modifier: u64) -> bool {
        let _ = (plane, format, modifier);
        true
    }
    /// Checks a plane's new state.
    fn check_plane(&self, plane: usize, new: &PlaneState, crtc: Option<&CrtcState>) -> Result<(), Errno> {
        let _ = (plane, new, crtc);
        Ok(())
    }
    /// Checks a CRTC's new state.
    fn check_crtc(&self, crtc: usize, new: &CrtcState) -> Result<(), Errno> {
        let _ = (crtc, new);
        Ok(())
    }
    /// Checks a connector's new state.
    fn check_connector(&self, connector: usize, new: &ConnectorState) -> Result<(), Errno> {
        let _ = (connector, new);
        Ok(())
    }
    /// Applies a CRTC's new state.
    fn commit_crtc(&self, crtc: usize, old: &CrtcState, new: &CrtcState) {
        let _ = (crtc, old, new);
    }
    /// Applies a plane's new state.
    fn commit_plane(&self, plane: usize, old: &PlaneState, new: &PlaneState) {
        let _ = (plane, old, new);
    }
    /// Applies a connector's new state.
    fn commit_connector(&self, connector: usize, old: &ConnectorState, new: &ConnectorState) {
        let _ = (connector, old, new);
    }
    /// Loads a gamma table.
    fn set_gamma(&self, crtc: usize, r: &[u16], g: &[u16], b: &[u16]) {
        let _ = (crtc, r, g, b);
    }
    /// `DIRTYFB`.
    fn dirty_fb(&self, fb: &Fb, planes: &[usize], clips: &[DirtyRect]) -> Result<(), Errno> {
        let _ = (fb, planes, clips);
        Err(crate::ENOSYS)
    }
}

/// R3's plane check: a visible plane on an enabled CRTC must cover the
/// whole mode unless `can_position`, and must not scale unless
/// `can_scale`.
pub fn check_plane_helper(
    new: &PlaneState,
    crtc: Option<&CrtcState>,
    can_position: bool,
    can_scale: bool,
) -> Result<(), Errno> {
    let n = plane_to_c(new);
    let c = crtc.map(crtc_to_c);
    let cp = c.as_ref().map_or(ptr::null(), |c| c as *const _);
    // SAFETY: states valid during the call.
    check(unsafe { sys::drm_rkms_check_plane_helper(&n, cp, can_position, can_scale) })
}

// --- conversions ------------------------------------------------------------

fn mode_to_c(m: &DisplayMode) -> sys::drm_rkms_mode {
    let u = m.to_umode(true);
    let mut name = [0 as core::ffi::c_char; 32];
    for (d, s) in name.iter_mut().zip(u.name.iter()) {
        *d = *s;
    }
    sys::drm_rkms_mode {
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

fn mode_from_c(m: &sys::drm_rkms_mode) -> Option<DisplayMode> {
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
    DisplayMode::from_umode(&u).ok()
}

fn idx(v: Option<usize>) -> i32 {
    v.and_then(|i| i32::try_from(i).ok()).unwrap_or(-1)
}

fn crtc_to_c(s: &CrtcState) -> sys::drm_rkms_crtc_state {
    sys::drm_rkms_crtc_state {
        active: s.active,
        enable: s.enable,
        has_mode: s.mode.is_some(),
        mode: s.mode.as_ref().map(mode_to_c).unwrap_or_default(),
    }
}

/// # Safety
/// `c` is NULL or a state of the running callback.
unsafe fn crtc_from_c(c: *const sys::drm_rkms_crtc_state) -> Option<CrtcState> {
    if c.is_null() {
        return None;
    }
    // SAFETY: the caller's contract.
    let c = unsafe { &*c };
    Some(CrtcState { active: c.active, enable: c.enable, mode: if c.has_mode { mode_from_c(&c.mode) } else { None } })
}

fn plane_to_c(s: &PlaneState) -> sys::drm_rkms_plane_state {
    sys::drm_rkms_plane_state {
        crtc: idx(s.crtc),
        fb: s.fb.as_ref().map_or(ptr::null_mut(), Fb::raw),
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

/// # Safety
/// `p` is a state of the running callback.
unsafe fn plane_from_c(p: *const sys::drm_rkms_plane_state) -> PlaneState {
    // SAFETY: the caller's contract.
    let p = unsafe { &*p };
    PlaneState {
        crtc: usize::try_from(p.crtc).ok(),
        // SAFETY: a framebuffer of the running callback.
        fb: unsafe { Fb::from_borrowed(p.fb) },
        crtc_x: p.crtc_x,
        crtc_y: p.crtc_y,
        crtc_w: p.crtc_w,
        crtc_h: p.crtc_h,
        src_x: p.src_x,
        src_y: p.src_y,
        src_w: p.src_w,
        src_h: p.src_h,
    }
}

/// # Safety
/// `c` is a state of the running callback.
unsafe fn connector_from_c(c: *const sys::drm_rkms_connector_state) -> ConnectorState {
    // SAFETY: the caller's contract.
    let c = unsafe { &*c };
    ConnectorState {
        crtc: usize::try_from(c.crtc).ok(),
        encoder: usize::try_from(c.encoder).ok(),
        link_status: c.link_status,
    }
}

// --- the thunks -------------------------------------------------------------

/// The driver's display of a device the facade passes.
///
/// # Safety
/// `dev` is a device of driver `D`.
unsafe fn kms_of<'a, D: Driver>(dev: *mut sys::drm_device) -> Option<&'a dyn KmsDriver> {
    // SAFETY: the caller's contract.
    unsafe { Dev::<D>::from_raw(dev) }.driver().kms()
}

unsafe extern "C" fn layout_thunk<D: Driver>(dev: *mut sys::drm_device, sink: *mut sys::drm_rkms_layout_sink) -> c_int {
    // SAFETY: the facade's arguments.
    let Some(k) = (unsafe { kms_of::<D>(dev) }) else { return neg(Errno::ENODEV) };
    let l = k.layout();
    let planes: Vec<sys::drm_rkms_plane_desc> = l
        .planes
        .iter()
        .map(|p| sys::drm_rkms_plane_desc {
            type_: match p.typ {
                PlaneType::Overlay => sys::DRM_RKMS_PLANE_OVERLAY,
                PlaneType::Primary => sys::DRM_RKMS_PLANE_PRIMARY,
                PlaneType::Cursor => sys::DRM_RKMS_PLANE_CURSOR,
            },
            possible_crtcs: p.possible_crtcs,
            formats: p.formats.as_ptr(),
            num_formats: p.formats.len() as u32,
            modifiers: p.modifiers.as_ptr(),
            num_modifiers: p.modifiers.len() as u32,
        })
        .collect();
    let crtcs: Vec<sys::drm_rkms_crtc_desc> = l
        .crtcs
        .iter()
        .map(|c| sys::drm_rkms_crtc_desc { primary: c.primary as u32, cursor: idx(c.cursor), gamma_size: c.gamma_size })
        .collect();
    let encoders: Vec<sys::drm_rkms_encoder_desc> = l
        .encoders
        .iter()
        .map(|e| sys::drm_rkms_encoder_desc {
            type_: e.typ,
            possible_crtcs: e.possible_crtcs,
            possible_clones: e.possible_clones,
        })
        .collect();
    let connectors: Vec<sys::drm_rkms_connector_desc> = l
        .connectors
        .iter()
        .map(|c| sys::drm_rkms_connector_desc {
            type_: c.typ,
            encoders: c.encoders,
            limits: sys::drm_rkms_mode_limits {
                max_width: u32::from(c.limits.max_width),
                max_height: u32::from(c.limits.max_height),
                max_clock: c.limits.max_clock,
                interlace: c.limits.interlace,
                dblscan: c.limits.dblscan,
            },
        })
        .collect();
    let l = sys::drm_rkms_layout {
        min_width: l.min_width,
        max_width: l.max_width,
        min_height: l.min_height,
        max_height: l.max_height,
        planes: planes.as_ptr(),
        num_planes: planes.len() as u32,
        crtcs: crtcs.as_ptr(),
        num_crtcs: crtcs.len() as u32,
        encoders: encoders.as_ptr(),
        num_encoders: encoders.len() as u32,
        connectors: connectors.as_ptr(),
        num_connectors: connectors.len() as u32,
    };
    // SAFETY: the core's sink; it copies the arrays, which live until here.
    unsafe { sys::drm_rkms_layout_set(sink, &l) }
}

unsafe extern "C" fn probe_thunk<D: Driver>(dev: *mut sys::drm_device, connector: u32, out: *mut sys::drm_rkms_probe) {
    // SAFETY: the facade's arguments.
    let Some(k) = (unsafe { kms_of::<D>(dev) }) else { return };
    let p = k.probe(connector as usize);
    let status = match p.status {
        ConnectorStatus::Connected => sys::DRM_RKMS_CONNECTED,
        ConnectorStatus::Disconnected => sys::DRM_RKMS_DISCONNECTED,
        ConnectorStatus::Unknown => sys::DRM_RKMS_UNKNOWN,
    };
    // SAFETY: the core's probe sink, valid during the callback.
    unsafe {
        sys::drm_rkms_probe_set_status(out, status);
        if let Some(e) = &p.edid {
            sys::drm_rkms_probe_set_edid(out, e.as_ptr().cast(), e.len());
        }
        for m in &p.modes {
            let c = mode_to_c(m);
            sys::drm_rkms_probe_add_mode(out, &c);
        }
        sys::drm_rkms_probe_set_size_mm(out, p.width_mm, p.height_mm);
    }
}

unsafe extern "C" fn fmt_thunk<D: Driver>(dev: *mut sys::drm_device, plane: u32, format: u32, modifier: u64) -> bool {
    // SAFETY: the facade's arguments.
    unsafe { kms_of::<D>(dev) }.is_none_or(|k| k.format_mod_supported(plane as usize, format, modifier))
}

unsafe extern "C" fn check_plane_thunk<D: Driver>(
    dev: *mut sys::drm_device,
    plane: u32,
    new: *const sys::drm_rkms_plane_state,
    crtc: *const sys::drm_rkms_crtc_state,
) -> c_int {
    // SAFETY: the facade's arguments, valid during the call.
    let Some(k) = (unsafe { kms_of::<D>(dev) }) else { return 0 };
    // SAFETY: as above.
    let (n, c) = unsafe { (plane_from_c(new), crtc_from_c(crtc)) };
    match k.check_plane(plane as usize, &n, c.as_ref()) {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

unsafe extern "C" fn check_crtc_thunk<D: Driver>(
    dev: *mut sys::drm_device,
    crtc: u32,
    new: *const sys::drm_rkms_crtc_state,
    _view: *const sys::drm_rkms_view,
) -> c_int {
    // SAFETY: as above.
    let Some(k) = (unsafe { kms_of::<D>(dev) }) else { return 0 };
    // SAFETY: as above.
    let Some(n) = (unsafe { crtc_from_c(new) }) else { return neg(Errno::EINVAL) };
    match k.check_crtc(crtc as usize, &n) {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

unsafe extern "C" fn check_connector_thunk<D: Driver>(
    dev: *mut sys::drm_device,
    connector: u32,
    new: *const sys::drm_rkms_connector_state,
) -> c_int {
    // SAFETY: as above.
    let Some(k) = (unsafe { kms_of::<D>(dev) }) else { return 0 };
    // SAFETY: as above.
    let n = unsafe { connector_from_c(new) };
    match k.check_connector(connector as usize, &n) {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

unsafe extern "C" fn commit_crtc_thunk<D: Driver>(
    dev: *mut sys::drm_device,
    crtc: u32,
    old: *const sys::drm_rkms_crtc_state,
    new: *const sys::drm_rkms_crtc_state,
) {
    // SAFETY: as above.
    let Some(k) = (unsafe { kms_of::<D>(dev) }) else { return };
    // SAFETY: as above.
    let (Some(o), Some(n)) = (unsafe { crtc_from_c(old) }, unsafe { crtc_from_c(new) }) else { return };
    k.commit_crtc(crtc as usize, &o, &n);
}

unsafe extern "C" fn commit_plane_thunk<D: Driver>(
    dev: *mut sys::drm_device,
    plane: u32,
    old: *const sys::drm_rkms_plane_state,
    new: *const sys::drm_rkms_plane_state,
) {
    // SAFETY: as above.
    let Some(k) = (unsafe { kms_of::<D>(dev) }) else { return };
    // SAFETY: as above.
    let (o, n) = unsafe { (plane_from_c(old), plane_from_c(new)) };
    k.commit_plane(plane as usize, &o, &n);
}

unsafe extern "C" fn commit_connector_thunk<D: Driver>(
    dev: *mut sys::drm_device,
    connector: u32,
    old: *const sys::drm_rkms_connector_state,
    new: *const sys::drm_rkms_connector_state,
) {
    // SAFETY: as above.
    let Some(k) = (unsafe { kms_of::<D>(dev) }) else { return };
    // SAFETY: as above.
    let (o, n) = unsafe { (connector_from_c(old), connector_from_c(new)) };
    k.commit_connector(connector as usize, &o, &n);
}

unsafe extern "C" fn gamma_thunk<D: Driver>(
    dev: *mut sys::drm_device,
    crtc: u32,
    r: *const u16,
    g: *const u16,
    b: *const u16,
    size: u32,
) {
    // SAFETY: as above.
    let Some(k) = (unsafe { kms_of::<D>(dev) }) else { return };
    let n = size as usize;
    if r.is_null() || g.is_null() || b.is_null() {
        return;
    }
    // SAFETY: three arrays of `size` entries.
    let (r, g, b) = unsafe {
        (core::slice::from_raw_parts(r, n), core::slice::from_raw_parts(g, n), core::slice::from_raw_parts(b, n))
    };
    k.set_gamma(crtc as usize, r, g, b);
}

unsafe extern "C" fn dirty_thunk<D: Driver>(
    dev: *mut sys::drm_device,
    fb: *mut sys::drm_rkms_fb,
    planes: *const u32,
    num_planes: u32,
    clips: *const sys::drm_rkms_rect,
    num_clips: u32,
) -> c_int {
    // SAFETY: as above.
    let Some(k) = (unsafe { kms_of::<D>(dev) }) else { return neg(crate::ENOSYS) };
    // SAFETY: a framebuffer of the running callback.
    let Some(f) = (unsafe { Fb::from_borrowed(fb) }) else { return neg(Errno::EINVAL) };
    // SAFETY: arrays of the given lengths.
    let ps: Vec<usize> = unsafe { slice(planes, num_planes) }.iter().map(|p| *p as usize).collect();
    // SAFETY: as above.
    let cs: Vec<DirtyRect> = unsafe { slice(clips, num_clips) }
        .iter()
        .map(|c| DirtyRect { x1: c.x1, y1: c.y1, x2: c.x2, y2: c.y2 })
        .collect();
    match k.dirty_fb(&f, &ps, &cs) {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

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

/// The facade's mode-setting callbacks of a driver type.
trait Funcs: Driver {
    const FUNCS: sys::drm_rkms_funcs;
}

impl<D: Driver> Funcs for D {
    const FUNCS: sys::drm_rkms_funcs = sys::drm_rkms_funcs {
        layout: Some(layout_thunk::<D>),
        probe: Some(probe_thunk::<D>),
        format_mod_supported: Some(fmt_thunk::<D>),
        check_plane: Some(check_plane_thunk::<D>),
        check_crtc: Some(check_crtc_thunk::<D>),
        check_connector: Some(check_connector_thunk::<D>),
        commit_crtc: Some(commit_crtc_thunk::<D>),
        commit_plane: Some(commit_plane_thunk::<D>),
        commit_connector: Some(commit_connector_thunk::<D>),
        set_gamma: Some(gamma_thunk::<D>),
        dirty_fb: Some(dirty_thunk::<D>),
    };
}

/// The callbacks for a driver's table.
pub(crate) const fn funcs<D: Driver>() -> *const sys::drm_rkms_funcs {
    &<D as Funcs>::FUNCS
}
