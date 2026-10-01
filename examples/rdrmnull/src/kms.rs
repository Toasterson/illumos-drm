// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The display of `rdrmnull`: one virtual connector with a fixed EDID, one
//! encoder, one CRTC with a primary and a cursor plane, a software vblank
//! at the mode's refresh rate, and a software "scanout" that a test-only
//! ioctl can checksum.
//!
//! The EDID (`crates/drm-core/tests/edid/rdrmnull.bin`, synthesized by
//! `gen_edid.py`) offers 1024x768 at 60 Hz as preferred mode, plus
//! 1280x1024, 800x600 and 640x480 at 60 Hz; the core parses it into the
//! mode list, so the null driver exercises the EDID path in the kernel.
//!
//! Scanout: the framebuffer the primary plane shows is latched at the
//! vblank after the commit that set it, as hardware would, and then
//! reported by `RDRMNULL_IOCTL_SCANOUT` together with a checksum of its
//! visible pixels, so that a test can tell which image is on "screen".
//! While the CRTC is off there is nothing to latch, and a commit applies at
//! once.

use core::mem;

use alloc::sync::Arc;

use ddi::prelude::*;
use drm_facade::DevHandle;
use drm_facade::kms::formats::{ARGB8888, MOD_LINEAR, RGB565, XRGB8888};
use drm_facade::kms::modes::ModeLimits;
use drm_facade::kms::soft::SoftVblank;
use drm_facade::kms::{
    ConnectorDesc, ConnectorStatus, CrtcDesc, CrtcState, EncoderDesc, Fb, KmsDriver, Layout, PlaneDesc, PlaneState,
    PlaneType, Probe, check_plane_helper,
};
use drm_uapi as uapi;

use crate::NullBo;

/// The EDID of the virtual display.
static EDID: &[u8] = include_bytes!("../../../crates/drm-core/tests/edid/rdrmnull.bin");

/// The modes the virtual connector accepts, from the EDID or from userland
/// (a `MODE_ID` blob or `SETCRTC`).
const LIMITS: ModeLimits =
    ModeLimits { max_width: 2048, max_height: 2048, max_clock: 400_000, interlace: false, dblscan: false };

/// The largest cursor, as `DRM_CAP_CURSOR_WIDTH`/`HEIGHT` report.
const CURSOR_MAX: u32 = 64;

/// What the primary plane shows.
#[derive(Clone)]
struct Image {
    fb: Fb,
    /// The source rectangle in whole pixels.
    src: (u32, u32, u32, u32),
}

#[derive(Default)]
struct Scanout {
    /// The CRTC is active and vblanks run.
    active: bool,
    /// Set by a commit; latched at the next vblank.
    pending: Option<Option<Image>>,
    current: Option<Image>,
    /// Vblanks and latches since the driver attached.
    frames: u64,
    latches: u64,
}

/// The display side of `rdrmnull`.
pub struct NullKms {
    vblank: Mutex<Option<DevHandle>>,
    source: Mutex<Option<SoftVblank>>,
    scan: Arc<Mutex<Scanout>>,
}

/// The reply of `RDRMNULL_IOCTL_SCANOUT`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ScanoutInfo {
    /// The framebuffer on screen, or 0.
    pub fb_id: u32,
    /// 1 while the CRTC is active.
    pub active: u32,
    /// The visible rectangle in the framebuffer, in pixels.
    pub x: u32,
    /// See `x`.
    pub y: u32,
    /// See `x`.
    pub width: u32,
    /// See `x`.
    pub height: u32,
    /// The fourcc format.
    pub format: u32,
    /// Always 0.
    pub pad: u32,
    /// Vblanks so far.
    pub frames: u64,
    /// Framebuffers latched at a vblank so far.
    pub latches: u64,
    /// FNV-1a (64-bit) over the visible pixels, row by row, `width * cpp`
    /// bytes each; 0 if nothing is shown.
    pub checksum: u64,
}

impl NullKms {
    /// The display, off, without a vblank handle yet.
    pub fn new(ctx: &Sleepable) -> Result<NullKms, Errno> {
        let scan = Arc::new(Mutex::new(Scanout::default()));
        let _ = ctx;
        Ok(NullKms { vblank: Mutex::new(None), source: Mutex::new(None), scan })
    }

    /// Gives the display the device's vblank handle, once the device has
    /// attached.
    pub fn set_vblank(&self, h: DevHandle) {
        *self.vblank.lock() = Some(h);
    }

    /// Stops the vblank source and drops every framebuffer reference, before
    /// the device detaches.
    pub fn shutdown(&self) {
        let src = self.source.lock().take();
        drop(src);
        let old = {
            let mut s = self.scan.lock();
            s.active = false;
            (s.pending.take(), s.current.take())
        };
        drop(old);
        *self.vblank.lock() = None;
    }

    /// The scanout state and the checksum of what is shown.
    pub fn scanout(&self) -> ScanoutInfo {
        let (img, active, frames, latches) = {
            let s = self.scan.lock();
            (s.current.clone(), s.active, s.frames, s.latches)
        };
        let mut info = ScanoutInfo { active: u32::from(active), frames, latches, ..ScanoutInfo::default() };
        if let Some(img) = img {
            info.fb_id = img.fb.id;
            (info.x, info.y, info.width, info.height) = img.src;
            info.format = img.fb.format;
            info.checksum = checksum(&img).unwrap_or(0);
        }
        info
    }

    fn start_vblank(&self, frame_ns: u64, ctx: &Sleepable) {
        let Some(h) = *self.vblank.lock() else { return };
        let scan = self.scan.clone();
        let src = SoftVblank::start(
            frame_ns,
            move |t, ctx| {
                let old = {
                    let mut s = scan.lock();
                    s.frames += 1;
                    match s.pending.take() {
                        Some(p) => {
                            s.latches += 1;
                            mem::replace(&mut s.current, p)
                        }
                        None => None,
                    }
                };
                // Never the last reference: the core's registry keeps one
                // for as long as a state or this driver may use it.
                drop(old);
                h.vblank(0, t, ctx);
            },
            ctx,
        );
        let old = self.source.lock().replace(src);
        drop(old);
    }

    fn stop_vblank(&self) {
        let src = self.source.lock().take();
        // Waits for a running callback; no lock it takes is held here.
        drop(src);
    }
}

/// FNV-1a over the visible rectangle of an image in system memory.
fn checksum(img: &Image) -> Option<u64> {
    let fb = &img.fb;
    // A dumb buffer; a movable object has no stable kernel view.
    let obj = fb.object::<NullBo>(0)?;
    let cpp = u64::from(fb.cpp0);
    if cpp == 0 {
        return None;
    }
    let (x, y, w, h) = img.src;
    let (pitch, off) = (u64::from(fb.pitches[0]), u64::from(fb.offsets[0]));
    let size = obj.size() as u64;
    let base = obj.vaddr(0);
    if base.is_null() {
        return None;
    }
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for row in u64::from(y)..u64::from(y) + u64::from(h) {
        let start = off + row * pitch + u64::from(x) * cpp;
        let len = u64::from(w) * cpp;
        if start + len > size {
            return None;
        }
        for i in start..start + len {
            // SAFETY: the byte lies inside the object's locked kernel memory
            // (checked above), which lives as long as the framebuffer. A
            // client may write it concurrently through a mapping, so it is
            // read with a volatile load rather than through a reference.
            let b = unsafe { core::ptr::read_volatile(base.add(i as usize)) };
            hash ^= u64::from(b);
            hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
        }
    }
    Some(hash)
}

impl KmsDriver for NullKms {
    fn layout(&self) -> Layout {
        let limits = LIMITS;
        Layout {
            min_width: 1,
            max_width: 2048,
            min_height: 1,
            max_height: 2048,
            planes: alloc::vec![
                PlaneDesc {
                    typ: PlaneType::Primary,
                    possible_crtcs: 1,
                    formats: alloc::vec![XRGB8888, ARGB8888, RGB565],
                    modifiers: alloc::vec![MOD_LINEAR],
                },
                PlaneDesc {
                    typ: PlaneType::Cursor,
                    possible_crtcs: 1,
                    formats: alloc::vec![ARGB8888],
                    modifiers: alloc::vec![MOD_LINEAR],
                },
            ],
            crtcs: alloc::vec![CrtcDesc { primary: 0, cursor: Some(1), gamma_size: 256 }],
            encoders: alloc::vec![EncoderDesc {
                typ: uapi::DRM_MODE_ENCODER_VIRTUAL,
                possible_crtcs: 1,
                possible_clones: 0,
            }],
            connectors: alloc::vec![ConnectorDesc { typ: uapi::DRM_MODE_CONNECTOR_VIRTUAL, encoders: 1, limits }],
        }
    }

    fn probe(&self, _connector: usize) -> Probe {
        Probe { status: ConnectorStatus::Connected, edid: Some(EDID.to_vec()), ..Probe::default() }
    }

    fn check_plane(&self, plane: usize, new: &PlaneState, crtc: Option<&CrtcState>) -> Result<()> {
        if plane == 1 {
            // The cursor: at most 64 by 64, anywhere, unscaled.
            if new.fb.as_ref().is_some_and(|f| f.width > CURSOR_MAX || f.height > CURSOR_MAX) {
                return Err(Errno::EINVAL);
            }
            return check_plane_helper(new, crtc, true, false);
        }
        // The primary plane covers the whole mode, unscaled.
        check_plane_helper(new, crtc, false, false)
    }

    fn check_crtc(&self, _crtc: usize, new: &CrtcState) -> Result<()> {
        // A mode from userland must be one the display could show: within
        // the connector's limits, progressive, not double-scanned, and
        // without line repetition (vscan), which the scanout ignores.
        match &new.mode {
            Some(m) if !LIMITS.allows(m) || m.vscan > 1 => Err(Errno::EINVAL),
            _ => Ok(()),
        }
    }

    fn commit_crtc(&self, _crtc: usize, old: &CrtcState, new: &CrtcState) {
        // SAFETY: the facade commits in thread context.
        let ctx = &unsafe { Sleepable::assume() };
        let same_mode = match (&old.mode, &new.mode) {
            (Some(a), Some(b)) => a.same_timings(b),
            _ => false,
        };
        if new.active && old.active && same_mode {
            return;
        }
        self.stop_vblank();
        if new.active {
            self.scan.lock().active = true;
            if let Some(ns) = new.mode.and_then(|m| m.frame_ns()) {
                self.start_vblank(ns, ctx);
            }
        } else {
            let old = {
                let mut s = self.scan.lock();
                s.active = false;
                (s.pending.take(), s.current.take())
            };
            drop(old);
        }
    }

    fn commit_plane(&self, plane: usize, _old: &PlaneState, new: &PlaneState) {
        if plane != 0 {
            // The cursor is not composed into the checksum.
            return;
        }
        let img = new
            .fb
            .clone()
            .filter(|_| new.crtc.is_some())
            .map(|fb| Image { fb, src: (new.src_x >> 16, new.src_y >> 16, new.src_w >> 16, new.src_h >> 16) });
        let old = {
            let mut s = self.scan.lock();
            if s.active { s.pending.replace(img).flatten() } else { mem::replace(&mut s.current, img) }
        };
        drop(old);
    }
}
