// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The legacy mode-setting ioctls as atomic updates (`drm_crtc.c`,
//! `drm_plane.c`, `drm_atomic_helper.c`), so that clients that never
//! enable the atomic capability, such as the Xorg modesetting driver and
//! `modetest -s`, work: `SETCRTC` ([`KmsCore::setcrtc`]), `PAGE_FLIP`
//! ([`KmsCore::page_flip`]), `SETPLANE` ([`KmsCore::setplane`]),
//! `CURSOR`/`CURSOR2` ([`KmsCore::cursor`]), the `DPMS` property
//! ([`KmsCore::dpms`]) and the gamma table. Each builds an
//! [`AtomicState`] that goes through the same check and commit as the
//! atomic ioctl; the argument checks and their errnos are Linux's.

use alloc::vec::Vec;

use ddi::{Errno, Sleepable};
use drm_uapi as uapi;

use super::atomic::check_src_coords;
use super::core::KmsCore;
use super::modes::DisplayMode;
use super::objects::{Blob, FbRef, ObjKind, mode_blob_data};
use super::state::{AtomicState, CommitFlags, FbBacking, KmsDriver};

/// Legacy updates may always change modes (Linux's
/// `drm_atomic_state_init` sets `allow_modeset`).
const LEGACY: CommitFlags = CommitFlags { allow_modeset: true, nonblock: false, cursor: false };

/// The red, green and blue tables of a CRTC's legacy gamma.
pub type Gamma<'a> = (&'a [u16], &'a [u16], &'a [u16]);

/// A `SETCRTC` request after the argument copy-in.
pub struct SetCrtc<'a> {
    /// The CRTC.
    pub crtc_id: u32,
    /// The framebuffer; `u32::MAX` (-1) keeps the current one.
    pub fb_id: u32,
    /// Position in the framebuffer.
    pub x: u32,
    /// Position in the framebuffer.
    pub y: u32,
    /// The mode, if `mode_valid`.
    pub mode: Option<DisplayMode>,
    /// The connectors to drive.
    pub connectors: &'a [u32],
}

impl<B: FbBacking> KmsCore<B> {
    /// `SETCRTC` (`drm_mode_setcrtc` and `__drm_atomic_helper_set_config`).
    /// The caller has refused positions above 16 bits (`ERANGE`) and
    /// converted the mode.
    pub fn setcrtc(
        &mut self,
        r: &SetCrtc<'_>,
        drv: &dyn KmsDriver<B>,
        ctx: &Sleepable,
    ) -> Result<AtomicState<B>, Errno> {
        let c = self.crtc_index(r.crtc_id).ok_or(Errno::ENOENT)?;
        let desc = self.layout.crtcs.get(c).ok_or(Errno::ENOENT)?;
        let primary = desc.primary;
        let mut fb: Option<FbRef<B>> = None;
        if let Some(m) = &r.mode {
            let f = if r.fb_id == u32::MAX {
                self.cur.planes.get(primary).and_then(|p| p.fb.clone()).ok_or(Errno::EINVAL)?
            } else {
                self.fb(r.fb_id).cloned().ok_or(Errno::ENOENT)?
            };
            if !self.plane_has_format(drv, primary, f.format, f.modifier) {
                return Err(Errno::EINVAL);
            }
            check_src_coords(
                r.x << 16,
                r.y << 16,
                u32::from(m.hdisplay) << 16,
                u32::from(m.vdisplay) << 16,
                f.width,
                f.height,
            )?;
            fb = Some(f);
        }
        if r.connectors.is_empty() && r.mode.is_some() {
            return Err(Errno::EINVAL);
        }
        if !r.connectors.is_empty() && fb.is_none() {
            return Err(Errno::EINVAL);
        }
        if r.connectors.len() > self.layout.connectors.len() {
            return Err(Errno::EINVAL);
        }
        let mut conns = Vec::with_capacity(r.connectors.len());
        for id in r.connectors {
            conns.push(self.connector_index(*id).ok_or(Errno::ENOENT)?);
        }
        // The mode becomes a blob, as drm_atomic_set_mode_for_crtc makes
        // one: MODE_ID then names it. No file owns it; it goes away with
        // the last state that uses it.
        let blob = match &r.mode {
            Some(m) => {
                let id = self.ids.alloc(ObjKind::Blob, ctx)?;
                let b = alloc::sync::Arc::new(Blob { id, data: mode_blob_data(m) });
                self.blobs.insert(id, b.clone());
                self.orphan_blobs.insert(id);
                Some(b)
            }
            None => None,
        };
        let mut st = self.begin(LEGACY);
        {
            let s = st.crtc_mut(&self.cur, c).ok_or(Errno::ENOENT)?;
            s.active = r.mode.is_some();
            s.mode = r.mode;
            s.mode_blob = blob;
        }
        {
            let p = st.plane_mut(&self.cur, primary).ok_or(Errno::ENOENT)?;
            match (&r.mode, fb) {
                (Some(m), Some(f)) => {
                    p.crtc = Some(c);
                    p.fb = Some(f);
                    p.crtc_x = 0;
                    p.crtc_y = 0;
                    p.crtc_w = u32::from(m.hdisplay);
                    p.crtc_h = u32::from(m.vdisplay);
                    p.src_x = r.x << 16;
                    p.src_y = r.y << 16;
                    p.src_w = u32::from(m.hdisplay) << 16;
                    p.src_h = u32::from(m.vdisplay) << 16;
                }
                _ => {
                    p.crtc = None;
                    p.fb = None;
                }
            }
        }
        // update_output_state: connectors of this CRTC that are not in the
        // set are detached, those in the set are attached (stolen from their
        // CRTCs if need be), and other CRTCs left without connectors are
        // turned off.
        for k in 0..self.layout.connectors.len() {
            let in_set = conns.contains(&k);
            let on_this = self.cur.connectors.get(k).is_some_and(|s| s.crtc == Some(c));
            if in_set || on_this {
                let old = self.cur.connectors.get(k).and_then(|s| s.crtc);
                let s = st.connector_mut(&self.cur, k).ok_or(Errno::EINVAL)?;
                s.crtc = if in_set { Some(c) } else { None };
                if let Some(o) = old.filter(|o| *o != c) {
                    st.crtc_mut(&self.cur, o);
                }
            }
        }
        for o in 0..self.layout.crtcs.len() {
            if o == c || st.crtcs.get(o).is_none_or(Option::is_none) {
                continue;
            }
            let still = (0..self.layout.connectors.len()).any(|k| {
                let s = st.connectors.get(k).and_then(Option::as_ref).or_else(|| self.cur.connectors.get(k));
                s.is_some_and(|s| s.crtc == Some(o))
            });
            if !still && let Some(s) = st.crtc_mut(&self.cur, o) {
                s.active = false;
                s.mode = None;
                s.mode_blob = None;
            }
        }
        Ok(st)
    }

    /// `PAGE_FLIP` (`drm_mode_page_flip_ioctl` and
    /// `drm_atomic_helper_page_flip`): a non-blocking update of the primary
    /// plane's framebuffer. The caller checked the flags.
    pub fn page_flip(&self, crtc_id: u32, fb_id: u32) -> Result<AtomicState<B>, Errno> {
        let c = self.crtc_index(crtc_id).ok_or(Errno::ENOENT)?;
        let primary = self.layout.crtcs.get(c).ok_or(Errno::ENOENT)?.primary;
        let cur = self.cur.planes.get(primary).ok_or(Errno::ENOENT)?;
        // The framebuffer is unbound, presumably by a hotplug event that
        // userland has not seen yet.
        let old_fb = cur.fb.as_ref().ok_or(Errno::EBUSY)?;
        let fb = self.fb(fb_id).cloned().ok_or(Errno::ENOENT)?;
        check_src_coords(cur.src_x, cur.src_y, cur.src_w, cur.src_h, fb.width, fb.height)?;
        if old_fb.format != fb.format {
            return Err(Errno::EINVAL);
        }
        let mut st = self.begin(CommitFlags { allow_modeset: false, nonblock: true, cursor: false });
        let p = st.plane_mut(&self.cur, primary).ok_or(Errno::ENOENT)?;
        p.fb = Some(fb);
        // The flip is for this CRTC even if the plane is shown elsewhere.
        st.crtc_mut(&self.cur, c);
        Ok(st)
    }

    /// `SETPLANE` (`drm_mode_setplane`, `__setplane_check`,
    /// `drm_atomic_helper_update_plane`). A zero `fb_id` turns the plane
    /// off.
    #[allow(clippy::too_many_arguments, reason = "the fields of drm_mode_set_plane")]
    pub fn setplane(
        &self,
        plane_id: u32,
        crtc_id: u32,
        fb_id: u32,
        dst: (i32, i32, u32, u32),
        src: (u32, u32, u32, u32),
        drv: &dyn KmsDriver<B>,
    ) -> Result<AtomicState<B>, Errno> {
        let p = self.plane_index(plane_id).ok_or(Errno::ENOENT)?;
        let mut st = self.begin(LEGACY);
        if fb_id == 0 {
            let s = st.plane_mut(&self.cur, p).ok_or(Errno::ENOENT)?;
            s.crtc = None;
            s.fb = None;
            return Ok(st);
        }
        let fb = self.fb(fb_id).cloned().ok_or(Errno::ENOENT)?;
        let c = self.crtc_index(crtc_id).ok_or(Errno::ENOENT)?;
        self.setplane_check(p, c, &fb, dst, src, drv)?;
        let s = st.plane_mut(&self.cur, p).ok_or(Errno::ENOENT)?;
        s.crtc = Some(c);
        s.fb = Some(fb);
        (s.crtc_x, s.crtc_y, s.crtc_w, s.crtc_h) = dst;
        (s.src_x, s.src_y, s.src_w, s.src_h) = src;
        st.crtc_mut(&self.cur, c);
        Ok(st)
    }

    fn setplane_check(
        &self,
        p: usize,
        c: usize,
        fb: &FbRef<B>,
        dst: (i32, i32, u32, u32),
        src: (u32, u32, u32, u32),
        drv: &dyn KmsDriver<B>,
    ) -> Result<(), Errno> {
        if self.layout.planes.get(p).is_none_or(|d| d.possible_crtcs & (1 << c) == 0) {
            return Err(Errno::EINVAL);
        }
        if !self.plane_has_format(drv, p, fb.format, fb.modifier) {
            return Err(Errno::EINVAL);
        }
        let max = i32::MAX as i64;
        let (x, y, w, h) = (i64::from(dst.0), i64::from(dst.1), i64::from(dst.2), i64::from(dst.3));
        if w > max || h > max || x > max - w || y > max - h {
            return Err(super::modes::ERANGE);
        }
        check_src_coords(src.0, src.1, src.2, src.3, fb.width, fb.height)
    }

    /// The cursor plane of a CRTC: `ENOENT` for an unknown CRTC, `ENXIO`
    /// for a CRTC without one (Linux: no `cursor_set`).
    pub fn cursor_plane(&self, crtc_id: u32) -> Result<(usize, usize), Errno> {
        let c = self.crtc_index(crtc_id).ok_or(Errno::ENOENT)?;
        let p = self.layout.crtcs.get(c).and_then(|d| d.cursor).ok_or(Errno::ENXIO)?;
        Ok((c, p))
    }

    /// `CURSOR` and `CURSOR2` through the cursor plane
    /// (`drm_mode_cursor_universal`). `bo` is `Some(new framebuffer or
    /// None to hide)` with `DRM_MODE_CURSOR_BO`, `None` to keep the current
    /// image; `pos` is the new position with `DRM_MODE_CURSOR_MOVE`. The
    /// update never waits and never stalls on a pending flip.
    pub fn cursor(
        &self,
        crtc_id: u32,
        bo: Option<Option<FbRef<B>>>,
        pos: Option<(i32, i32)>,
    ) -> Result<AtomicState<B>, Errno> {
        let (c, p) = self.cursor_plane(crtc_id)?;
        let fb = match bo {
            Some(f) => f,
            None => self.cur.planes.get(p).and_then(|s| s.fb.clone()),
        };
        let (x, y) = pos.unwrap_or_else(|| self.cursor_pos.get(c).copied().unwrap_or((0, 0)));
        let mut st = self.begin(CommitFlags { allow_modeset: true, nonblock: true, cursor: true });
        let s = st.plane_mut(&self.cur, p).ok_or(Errno::ENOENT)?;
        match fb {
            Some(f) => {
                s.crtc = Some(c);
                s.crtc_x = x;
                s.crtc_y = y;
                s.crtc_w = f.width;
                s.crtc_h = f.height;
                s.src_x = 0;
                s.src_y = 0;
                s.src_w = f.width << 16;
                s.src_h = f.height << 16;
                s.fb = Some(f);
            }
            None => {
                s.crtc = None;
                s.fb = None;
            }
        }
        Ok(st)
    }

    /// Records the cursor position after a successful `CURSOR_MOVE`.
    pub fn set_cursor_pos(&mut self, crtc_id: u32, pos: (i32, i32)) {
        if let Some(c) = self.crtc_index(crtc_id)
            && let Some(p) = self.cursor_pos.get_mut(c)
        {
            *p = pos;
        }
    }

    /// Setting the legacy `DPMS` property (`drm_atomic_connector_commit_dpms`):
    /// anything but On is Off; the connector's CRTC stays active while any
    /// of its connectors is On.
    pub fn dpms(&self, connector: usize, mode: u64) -> Result<AtomicState<B>, Errno> {
        let mode =
            if mode == u64::from(uapi::DRM_MODE_DPMS_ON) { uapi::DRM_MODE_DPMS_ON } else { uapi::DRM_MODE_DPMS_OFF };
        let mut st = self.begin(LEGACY);
        *st.dpms.get_mut(connector).ok_or(Errno::EINVAL)? = Some(mode);
        let Some(c) = self.cur.connectors.get(connector).and_then(|s| s.crtc) else { return Ok(st) };
        let active = (0..self.layout.connectors.len()).any(|k| {
            self.cur.connectors.get(k).is_some_and(|s| s.crtc == Some(c))
                && if k == connector {
                    mode == uapi::DRM_MODE_DPMS_ON
                } else {
                    self.conns.get(k).is_some_and(|i| i.dpms == uapi::DRM_MODE_DPMS_ON)
                }
        });
        st.crtc_mut(&self.cur, c).ok_or(Errno::EINVAL)?.active = active;
        Ok(st)
    }

    /// `GETGAMMA`: `ENOENT` for an unknown CRTC, `EINVAL` unless `size` is
    /// the CRTC's gamma size. Returns red, green and blue.
    pub fn get_gamma(&self, crtc_id: u32, size: u32) -> Result<Gamma<'_>, Errno> {
        let c = self.crtc_index(crtc_id).ok_or(Errno::ENOENT)?;
        let n = self.layout.crtcs.get(c).map_or(0, |d| d.gamma_size);
        if size != n || n == 0 {
            return Err(Errno::EINVAL);
        }
        let g = self.gamma.get(c).ok_or(Errno::EINVAL)?;
        let n = n as usize;
        Ok((g.get(..n).unwrap_or(&[]), g.get(n..2 * n).unwrap_or(&[]), g.get(2 * n..).unwrap_or(&[])))
    }

    /// `SETGAMMA`: as `GETGAMMA`; `lut` holds red, green and blue, `size`
    /// entries each.
    pub fn set_gamma(&mut self, crtc_id: u32, size: u32, lut: &[u16], drv: &dyn KmsDriver<B>) -> Result<(), Errno> {
        let c = self.crtc_index(crtc_id).ok_or(Errno::ENOENT)?;
        let n = self.layout.crtcs.get(c).map_or(0, |d| d.gamma_size);
        if size != n || n == 0 || lut.len() != 3 * n as usize {
            return Err(Errno::EINVAL);
        }
        let g = self.gamma.get_mut(c).ok_or(Errno::EINVAL)?;
        g.copy_from_slice(lut);
        let n = n as usize;
        drv.set_gamma(
            c,
            lut.get(..n).unwrap_or(&[]),
            lut.get(n..2 * n).unwrap_or(&[]),
            lut.get(2 * n..).unwrap_or(&[]),
        );
        Ok(())
    }
}
