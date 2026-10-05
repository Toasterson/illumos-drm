// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The display: one CRTC, primary plane, cursor plane, encoder and
//! connector per scanout the device reports.
//!
//! A connector is connected when the device reports its scanout enabled
//! (`GET_DISPLAY_INFO`, asked again after the device signals a display
//! change); its modes come from the device's EDID (`GET_EDID`, with
//! `VIRTIO_GPU_F_EDID`), or else from the scanout's preferred size (a CVT
//! mode) and the standard modes.
//!
//! The primary plane shows a resource with `SET_SCANOUT` and updates the
//! host's copy with `RESOURCE_FLUSH`; a 2D resource (a dumb buffer) is
//! copied from guest memory first with `TRANSFER_TO_HOST_2D`. `DIRTYFB`
//! does the same for the changed rectangles. The cursor plane goes through
//! the cursor queue (`UPDATE_CURSOR`, `MOVE_CURSOR`). The device has no
//! vblank interrupt, so each active CRTC gets a software vblank at its
//! mode's refresh rate (`drm::kms::soft`), which completes page flips and
//! delivers their events.

use alloc::vec::Vec;
use core::mem::size_of;

use ddi::prelude::*;
use drm::gem::GemRef;
use drm::kms::formats::{ARGB8888, MOD_LINEAR, XRGB8888};
use drm::kms::modes::{self, ModeLimits};
use drm::kms::objects::FbRef;
use drm::kms::soft::SoftVblank;
use drm::kms::{
    ConnectorDesc, ConnectorStatus, CrtcDesc, CrtcState, DirtyRect, EncoderDesc, KmsDriver, Layout, PlaneDesc,
    PlaneState, PlaneType, Probe, StateView, VblankHandle, check_plane_helper,
};
use drm_uapi as uapi;

use crate::gpu::{Q, dev_warn};
use crate::object::{DevRef, Kind, VgObject};
use crate::proto::{self, CtrlHdr, Rect};

type Fb = FbRef<GemRef<VgObject>>;

/// The largest scanout.
const MAX_SIZE: u32 = 8192;
/// The largest cursor, as `DRM_CAP_CURSOR_WIDTH` and `HEIGHT` report.
pub const CURSOR_MAX: u32 = 64;

const LIMITS: ModeLimits = ModeLimits {
    max_width: MAX_SIZE as u16,
    max_height: MAX_SIZE as u16,
    max_clock: 2_000_000,
    interlace: false,
    dblscan: false,
};

/// What one scanout shows.
#[derive(Default, Clone, Copy)]
struct Scanout {
    /// The resource on the scanout, 0 for none.
    res: u32,
    /// The source rectangle.
    rect: Rect,
    /// The cursor resource, 0 for none.
    cursor: u32,
    /// The cursor position.
    cursor_pos: (i32, i32),
}

/// The display side of the driver.
pub struct GpuKms {
    dr: DevRef,
    scanouts: usize,
    vblank: Mutex<Option<VblankHandle>>,
    sources: Mutex<Vec<Option<SoftVblank>>>,
    shown: Mutex<Vec<Scanout>>,
    display: Mutex<Option<proto::RespDisplayInfo>>,
}

impl GpuKms {
    /// The display of a device with `scanouts` scanouts (1 to 16).
    pub fn new(dr: DevRef, scanouts: usize) -> GpuKms {
        let n = scanouts.clamp(1, proto::MAX_SCANOUTS);
        let mut sources = Vec::new();
        sources.resize_with(n, || None);
        GpuKms {
            dr,
            scanouts: n,
            vblank: Mutex::new(None),
            sources: Mutex::new(sources),
            shown: Mutex::new(alloc::vec![Scanout::default(); n]),
            display: Mutex::new(None),
        }
    }

    /// Gives the display the device's vblank handle once it has attached.
    pub fn set_vblank(&self, h: VblankHandle) {
        *self.vblank.lock() = Some(h);
    }

    /// Stops every software vblank and turns the scanouts off, before the
    /// device detaches.
    pub fn shutdown(&self, ctx: &Sleepable) {
        let s = core::mem::take(&mut *self.sources.lock());
        drop(s);
        *self.vblank.lock() = None;
        let shown = core::mem::take(&mut *self.shown.lock());
        for (i, s) in shown.iter().enumerate() {
            if s.res != 0 {
                let _ = self.set_scanout(i, 0, Rect::default(), ctx);
            }
        }
    }

    fn set_scanout(&self, scanout: usize, res: u32, r: Rect, ctx: &Sleepable) -> Result<(), Errno> {
        let req = proto::SetScanout {
            hdr: CtrlHdr::new(proto::CMD_SET_SCANOUT),
            r,
            scanout_id: scanout as u32,
            resource_id: res,
        };
        self.dr.gpu().run(self.dr.dev(), &req, size_of::<CtrlHdr>(), 0, ctx).map(drop)
    }

    /// Copies rectangle `r` of a 2D resource from guest memory (if
    /// `transfer`) and flushes it to the display; waits for the flush.
    fn flush(&self, res: u32, r: Rect, offset: Option<u64>, ctx: &Sleepable) -> Result<(), Errno> {
        let gpu = self.dr.gpu();
        let dev = self.dr.dev();
        if let Some(offset) = offset {
            let t = proto::TransferToHost2d {
                hdr: CtrlHdr::new(proto::CMD_TRANSFER_TO_HOST_2D),
                r,
                offset,
                resource_id: res,
                padding: 0,
            };
            gpu.fire(dev, &t, ctx)?;
        }
        let f = proto::ResourceFlush { hdr: CtrlHdr::new(proto::CMD_RESOURCE_FLUSH), r, resource_id: res, padding: 0 };
        gpu.run(dev, &f, size_of::<CtrlHdr>(), 0, ctx).map(drop)
    }

    fn start_vblank(&self, crtc: usize, frame_ns: u64, ctx: &Sleepable) {
        let Some(h) = self.vblank.lock().clone() else { return };
        let src = SoftVblank::start(frame_ns, move |t, ctx| h.vblank(crtc, t, ctx), ctx);
        let old = self.sources.lock().get_mut(crtc).and_then(|s| s.replace(src));
        drop(old);
    }

    fn stop_vblank(&self, crtc: usize) {
        let old = self.sources.lock().get_mut(crtc).and_then(Option::take);
        // Waits for a running callback; no lock it takes is held here.
        drop(old);
    }

    fn cursor(&self, scanout: usize, req: proto::UpdateCursor, ctx: &Sleepable) {
        let gpu = self.dr.gpu();
        let dev = self.dr.dev();
        let r = gpu.cmd(dev, &req, 0, 0, Q::Cursor, ctx).and_then(|c| gpu.submit(dev, &c, None, ctx));
        if let Err(e) = r {
            dev_warn(gpu.instance, format_args!("cursor update of scanout {scanout}: {e:?}"));
        }
    }

    fn display_info(&self, ctx: &Sleepable) -> Option<proto::RespDisplayInfo> {
        let gpu = self.dr.gpu();
        let stale = gpu.display_changed.swap(false, core::sync::atomic::Ordering::AcqRel);
        if stale || self.display.lock().is_none() {
            match gpu.display_info(self.dr.dev(), ctx) {
                Ok(d) => *self.display.lock() = Some(d),
                Err(e) => {
                    gpu.display_changed.store(true, core::sync::atomic::Ordering::Release);
                    dev_warn(gpu.instance, format_args!("GET_DISPLAY_INFO: {e:?}"));
                }
            }
        }
        *self.display.lock()
    }
}

/// The resource of a framebuffer, with its object.
fn fb_object(fb: &Fb) -> Option<&VgObject> {
    fb.objs.first().and_then(Option::as_ref).map(GemRef::object)
}

/// The byte offset of pixel (`x`, `y`) of a 32-bit framebuffer.
fn pixel_offset(fb: &Fb, x: u32, y: u32) -> u64 {
    u64::from(fb.offsets[0]) + u64::from(y) * u64::from(fb.pitches[0]) + u64::from(x) * 4
}

/// Whether a framebuffer on a 2D resource has the layout the device
/// assumes: the resource's own stride, no offset, no larger than it.
fn fb_fits(fb: &Fb) -> bool {
    match fb_object(fb).map(VgObject::kind) {
        Some(Kind::Res2d { width, height }) => {
            fb.pitches[0] == width.saturating_mul(4) && fb.offsets[0] == 0 && fb.width <= width && fb.height <= height
        }
        Some(Kind::Res3d) => true,
        // A blob needs SET_SCANOUT_BLOB, which this driver does not send;
        // with HOST_VISIBLE 0, Mesa does not scan out blobs.
        Some(Kind::Blob { .. } | Kind::None) | None => false,
    }
}

impl KmsDriver<GemRef<VgObject>> for GpuKms {
    fn layout(&self) -> Layout {
        let mut planes = Vec::new();
        let mut crtcs = Vec::new();
        let mut encoders = Vec::new();
        let mut connectors = Vec::new();
        for i in 0..self.scanouts {
            let bit = 1u32 << i;
            planes.push(PlaneDesc {
                typ: PlaneType::Primary,
                possible_crtcs: bit,
                formats: alloc::vec![XRGB8888, ARGB8888],
                modifiers: alloc::vec![MOD_LINEAR],
            });
            planes.push(PlaneDesc {
                typ: PlaneType::Cursor,
                possible_crtcs: bit,
                formats: alloc::vec![ARGB8888],
                modifiers: alloc::vec![MOD_LINEAR],
            });
            crtcs.push(CrtcDesc { primary: 2 * i, cursor: Some(2 * i + 1), gamma_size: 256 });
            encoders.push(EncoderDesc { typ: uapi::DRM_MODE_ENCODER_VIRTUAL, possible_crtcs: bit, possible_clones: 0 });
            connectors.push(ConnectorDesc { typ: uapi::DRM_MODE_CONNECTOR_VIRTUAL, encoders: bit, limits: LIMITS });
        }
        Layout {
            min_width: 32,
            max_width: MAX_SIZE,
            min_height: 32,
            max_height: MAX_SIZE,
            planes,
            crtcs,
            encoders,
            connectors,
        }
    }

    fn probe(&self, connector: usize, ctx: &Sleepable) -> Probe {
        let Some(info) = self.display_info(ctx) else { return Probe::default() };
        let Some(one) = info.pmodes.get(connector) else { return Probe::default() };
        if one.enabled == 0 {
            return Probe { status: ConnectorStatus::Disconnected, ..Probe::default() };
        }
        let gpu = self.dr.gpu();
        let edid = if gpu.caps.lock().edid {
            match gpu.edid(self.dr.dev(), connector as u32, ctx) {
                Ok(e) => e,
                Err(e) => {
                    dev_warn(gpu.instance, format_args!("GET_EDID of scanout {connector}: {e:?}"));
                    None
                }
            }
        } else {
            None
        };
        let mut modes = Vec::new();
        if edid.is_none() {
            // Linux: the standard modes, and the scanout's size preferred.
            let (w, h) = (one.r.width.min(MAX_SIZE) as u16, one.r.height.min(MAX_SIZE) as u16);
            if let Some(mut m) = modes::cvt_mode(w, h, 60) {
                m.typ |= uapi::DRM_MODE_TYPE_PREFERRED;
                modes.push(m);
            }
            modes.extend(modes::noedid_modes(MAX_SIZE as u16, MAX_SIZE as u16).into_iter().map(|mut m| {
                m.typ &= !uapi::DRM_MODE_TYPE_PREFERRED;
                m
            }));
        }
        Probe { status: ConnectorStatus::Connected, edid, modes, ..Probe::default() }
    }

    fn check_plane(
        &self,
        plane: usize,
        new: &PlaneState<GemRef<VgObject>>,
        crtc: Option<&CrtcState>,
    ) -> Result<(), Errno> {
        if let Some(fb) = &new.fb
            && !fb_fits(fb)
        {
            return Err(Errno::EINVAL);
        }
        if plane % 2 == 1 {
            if new.fb.as_ref().is_some_and(|f| f.width > CURSOR_MAX || f.height > CURSOR_MAX) {
                return Err(Errno::EINVAL);
            }
            return check_plane_helper(new, crtc, true, false);
        }
        check_plane_helper(new, crtc, false, false)
    }

    fn check_crtc(&self, _crtc: usize, new: &CrtcState, _view: &StateView<'_, GemRef<VgObject>>) -> Result<(), Errno> {
        match &new.mode {
            Some(m) if !LIMITS.allows(m) || m.vscan > 1 => Err(Errno::EINVAL),
            _ => Ok(()),
        }
    }

    fn commit_crtc(&self, crtc: usize, old: &CrtcState, new: &CrtcState, ctx: &Sleepable) {
        let same_mode = match (&old.mode, &new.mode) {
            (Some(a), Some(b)) => a.same_timings(b),
            _ => false,
        };
        if new.active && old.active && same_mode {
            return;
        }
        self.stop_vblank(crtc);
        if new.active {
            if let Some(ns) = new.mode.and_then(|m| m.frame_ns()) {
                self.start_vblank(crtc, ns, ctx);
            }
        } else {
            let res = self.shown.lock().get(crtc).map_or(0, |s| s.res);
            if res != 0 {
                if let Err(e) = self.set_scanout(crtc, 0, Rect::default(), ctx) {
                    dev_warn(self.dr.gpu().instance, format_args!("SET_SCANOUT off on {crtc}: {e:?}"));
                }
                if let Some(s) = self.shown.lock().get_mut(crtc) {
                    s.res = 0;
                }
            }
        }
    }

    fn commit_plane(
        &self,
        plane: usize,
        old: &PlaneState<GemRef<VgObject>>,
        new: &PlaneState<GemRef<VgObject>>,
        ctx: &Sleepable,
    ) {
        let scanout = plane / 2;
        let instance = self.dr.gpu().instance;
        let cur = self.shown.lock().get(scanout).copied().unwrap_or_default();
        if plane % 2 == 1 {
            // The cursor.
            let shown = new.fb.as_ref().filter(|_| new.crtc.is_some());
            let pos = (new.crtc_x, new.crtc_y);
            let mut next = cur;
            match shown.and_then(|fb| fb_object(fb).map(|o| (fb, o))) {
                Some((fb, obj)) => {
                    let changed =
                        !old.fb.as_ref().is_some_and(|o| alloc::sync::Arc::ptr_eq(o, fb)) || cur.cursor != obj.res();
                    let mut req = proto::UpdateCursor {
                        hdr: CtrlHdr::new(proto::CMD_MOVE_CURSOR),
                        pos: proto::CursorPos {
                            scanout_id: scanout as u32,
                            x: pos.0 as u32,
                            y: pos.1 as u32,
                            padding: 0,
                        },
                        resource_id: obj.res(),
                        hot_x: fb.hot_x.max(0) as u32,
                        hot_y: fb.hot_y.max(0) as u32,
                        padding: 0,
                    };
                    if changed {
                        if matches!(obj.kind(), Kind::Res2d { .. }) {
                            let r = Rect { x: 0, y: 0, width: fb.width, height: fb.height };
                            let t = proto::TransferToHost2d {
                                hdr: CtrlHdr::new(proto::CMD_TRANSFER_TO_HOST_2D),
                                r,
                                offset: 0,
                                resource_id: obj.res(),
                                padding: 0,
                            };
                            // The image must be on the host before the
                            // cursor queue shows it.
                            if let Err(e) = self.dr.gpu().run(self.dr.dev(), &t, size_of::<CtrlHdr>(), 0, ctx) {
                                dev_warn(instance, format_args!("cursor transfer: {e:?}"));
                            }
                        }
                        req.hdr = CtrlHdr::new(proto::CMD_UPDATE_CURSOR);
                        self.cursor(scanout, req, ctx);
                    } else if pos != cur.cursor_pos {
                        self.cursor(scanout, req, ctx);
                    }
                    next.cursor = obj.res();
                    next.cursor_pos = pos;
                }
                None => {
                    if cur.cursor != 0 {
                        let req = proto::UpdateCursor {
                            hdr: CtrlHdr::new(proto::CMD_UPDATE_CURSOR),
                            pos: proto::CursorPos { scanout_id: scanout as u32, x: 0, y: 0, padding: 0 },
                            ..proto::UpdateCursor::default()
                        };
                        self.cursor(scanout, req, ctx);
                    }
                    next.cursor = 0;
                }
            }
            if let Some(s) = self.shown.lock().get_mut(scanout) {
                *s = next;
            }
            return;
        }
        // The primary plane.
        let shown = new.fb.as_ref().filter(|_| new.crtc.is_some());
        match shown.and_then(|fb| fb_object(fb).map(|o| (fb, o))) {
            Some((fb, obj)) => {
                let r =
                    Rect { x: new.src_x >> 16, y: new.src_y >> 16, width: new.src_w >> 16, height: new.src_h >> 16 };
                if (cur.res != obj.res() || cur.rect != r)
                    && let Err(e) = self.set_scanout(scanout, obj.res(), r, ctx)
                {
                    dev_warn(instance, format_args!("SET_SCANOUT of resource {} on {scanout}: {e:?}", obj.res()));
                }
                let offset = matches!(obj.kind(), Kind::Res2d { .. }).then(|| pixel_offset(fb, r.x, r.y));
                if let Err(e) = self.flush(obj.res(), r, offset, ctx) {
                    dev_warn(instance, format_args!("flush of resource {} on {scanout}: {e:?}", obj.res()));
                }
                if let Some(s) = self.shown.lock().get_mut(scanout) {
                    s.res = obj.res();
                    s.rect = r;
                }
            }
            None => {
                if cur.res != 0 {
                    if let Err(e) = self.set_scanout(scanout, 0, Rect::default(), ctx) {
                        dev_warn(instance, format_args!("SET_SCANOUT off on {scanout}: {e:?}"));
                    }
                    if let Some(s) = self.shown.lock().get_mut(scanout) {
                        s.res = 0;
                    }
                }
            }
        }
    }

    /// Transfers and flushes the bounding box of the damage once, as
    /// Linux merges plane damage (`drm_atomic_helper_damage_merged`): one
    /// `RESOURCE_FLUSH` updates every scanout that shows the resource. A
    /// cursor is sent again by its next update.
    fn dirty_fb(&self, fb: &Fb, planes: &[usize], clips: &[DirtyRect], ctx: &Sleepable) -> Result<(), Errno> {
        let Some(obj) = fb_object(fb) else { return Ok(()) };
        if !planes.iter().any(|p| p % 2 == 0) {
            return Ok(());
        }
        let Some(b) = merge_clips(clips, fb.width, fb.height) else { return Ok(()) };
        let r = Rect { x: b.x1, y: b.y1, width: b.x2 - b.x1, height: b.y2 - b.y1 };
        let offset = matches!(obj.kind(), Kind::Res2d { .. }).then(|| pixel_offset(fb, b.x1, b.y1));
        self.flush(obj.res(), r, offset, ctx)
    }
}

/// The bounding box of `clips` clamped to a `w` by `h` framebuffer (all
/// of it for no clips), `None` if nothing of it is left.
pub fn merge_clips(clips: &[DirtyRect], w: u32, h: u32) -> Option<DirtyRect> {
    if clips.is_empty() {
        return (w > 0 && h > 0).then_some(DirtyRect { x1: 0, y1: 0, x2: w, y2: h });
    }
    clips
        .iter()
        .map(|c| DirtyRect { x1: c.x1.min(w), y1: c.y1.min(h), x2: c.x2.min(w), y2: c.y2.min(h) })
        .filter(|c| c.x2 > c.x1 && c.y2 > c.y1)
        .reduce(|a, c| DirtyRect { x1: a.x1.min(c.x1), y1: a.y1.min(c.y1), x2: a.x2.max(c.x2), y2: a.y2.max(c.y2) })
}
