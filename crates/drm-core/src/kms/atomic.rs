// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Atomic updates: setting properties, checking and committing
//! (`drm_atomic_uapi.c`, `drm_atomic.c`, `drm_atomic_helper.c`).
//!
//! # Checks
//!
//! [`KmsCore::check`] applies, in this order:
//!
//! 1. Connector routing (Linux's `update_connector_routing`): a connector
//!    with a CRTC gets the first of its encoders that can drive that CRTC
//!    and that no other connector uses; `EINVAL` if there is none. The old
//!    and new CRTCs of a connector join the update.
//! 2. CRTCs: `ACTIVE` without a mode (`EINVAL`), a mode without connectors
//!    or connectors without a mode (`EINVAL`), an event on a CRTC that is
//!    off before and after (`EINVAL`), a change that needs a full mode set
//!    without `ALLOW_MODESET` (`EINVAL`). A CRTC that needs a full mode set
//!    drags its planes and connectors into the update, so the driver sees
//!    them against the new mode.
//! 3. Planes (`drm_atomic_plane_check`): a CRTC without a framebuffer or
//!    the reverse (`EINVAL`), a CRTC the plane cannot be on (`EINVAL`), a
//!    format or modifier the plane cannot scan out (`EINVAL`), a
//!    destination past `INT_MAX` (`ERANGE`), a source outside the
//!    framebuffer (`ENOSPC`).
//! 4. The driver's hooks for every connector, CRTC and plane in the update.
//!
//! # Commit
//!
//! [`KmsCore::commit`] calls the driver's commit hooks (CRTCs that end
//! inactive, planes, CRTCs that end active, connectors), swaps the new
//! states in and returns the old ones with the masks the event layer
//! needs. It records vblank on/off transitions for the kernel layer to
//! apply ([`KmsCore::take_transitions`]).

use alloc::vec::Vec;

use ddi::{Errno, Sleepable};
use drm_uapi as uapi;

use super::core::{KmsCore, props_of, release_old};
use super::objects::{FbRef, ObjKind, PlaneType};
use super::props::Prop;
use super::state::{AtomicState, CommitFlags, FbBacking, KmsDriver, OldState, StateView};

/// `ENOSPC`, for source rectangles outside their framebuffer.
pub const ENOSPC: Errno = Errno::ENOSPC;

/// A vblank on/off change of a CRTC for the event layer: the CRTC index,
/// whether it is now active, and the frame length of its mode in ns.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Transition {
    /// The CRTC index.
    pub crtc: usize,
    /// Whether vblanks now arrive.
    pub on: bool,
    /// The frame length of the new mode.
    pub frame_ns: u64,
}

/// What a commit changed, for the event layer.
#[derive(Debug)]
pub struct Committed<B> {
    /// The replaced states, to release in thread context.
    pub old: OldState<B>,
    /// Touched CRTCs that are active now: their flip completes at their
    /// next vblank.
    pub flip_mask: u32,
    /// Touched CRTCs that are inactive now: their events go out at once.
    pub off_mask: u32,
}

impl<B: FbBacking> KmsCore<B> {
    /// Starts an update.
    pub fn begin(&self, flags: CommitFlags) -> AtomicState<B> {
        AtomicState::new(&self.layout, flags)
    }

    /// Looks up an object for the atomic ioctl: `ENOENT` if unknown or if
    /// it has no properties.
    pub fn atomic_object(&self, id: u32) -> Result<ObjKind, Errno> {
        let k = self.find(id).ok_or(Errno::ENOENT)?;
        props_of(k).ok_or(Errno::ENOENT)?;
        Ok(k)
    }

    /// Sets property `prop_id` of object `kind` to `value` in an update
    /// (`drm_atomic_set_property`). A property the object does not have
    /// fails with `not_attached` (`ENOENT` for the atomic ioctl, `EINVAL`
    /// for `OBJ_SETPROPERTY`). The fence properties `IN_FENCE_FD` and
    /// `OUT_FENCE_PTR` fail with `EINVAL`: fences do not exist before
    /// milestone R2, and a silent success would lie about them.
    pub fn set_prop(
        &self,
        st: &mut AtomicState<B>,
        kind: ObjKind,
        prop_id: u32,
        value: u64,
        not_attached: Errno,
    ) -> Result<(), Errno> {
        let prop = match self.find(prop_id) {
            Some(ObjKind::Property(i)) => *Prop::ALL.get(i).ok_or(not_attached)?,
            _ => return Err(not_attached),
        };
        if prop.is_fence() {
            return Err(Errno::EINVAL);
        }
        if !props_of(kind).is_some_and(|l| l.contains(&prop)) {
            return Err(not_attached);
        }
        let def = prop.def();
        if !def.value_ok(value) || def.immutable {
            return Err(Errno::EINVAL);
        }
        let id = u32::try_from(value).ok();
        let crtc_arg = |v: u64| -> Result<Option<usize>, Errno> {
            if v == 0 {
                return Ok(None);
            }
            id.and_then(|i| self.crtc_index(i)).map(Some).ok_or(Errno::EINVAL)
        };
        match (kind, prop) {
            (ObjKind::Crtc(i), Prop::Active) => {
                st.crtc_mut(&self.cur, i).ok_or(Errno::EINVAL)?.active = value != 0;
            }
            (ObjKind::Crtc(i), Prop::ModeId) => {
                let blob = match value {
                    0 => None,
                    _ => Some(id.and_then(|b| self.blob(b)).ok_or(Errno::EINVAL)?.clone()),
                };
                let mode = blob.as_ref().map(|b| b.mode()).transpose()?;
                let c = st.crtc_mut(&self.cur, i).ok_or(Errno::EINVAL)?;
                let same = match (&c.mode_blob, &blob) {
                    (Some(a), Some(b)) => alloc::sync::Arc::ptr_eq(a, b),
                    (None, None) => true,
                    _ => false,
                };
                if !same {
                    c.mode = mode;
                    c.mode_blob = blob;
                }
            }
            (ObjKind::Plane(i), Prop::FbId) => {
                let fb: Option<FbRef<B>> = match value {
                    0 => None,
                    _ => Some(id.and_then(|f| self.fb(f)).ok_or(Errno::EINVAL)?.clone()),
                };
                st.plane_mut(&self.cur, i).ok_or(Errno::EINVAL)?.fb = fb;
            }
            (ObjKind::Plane(i), Prop::CrtcId) => {
                let c = crtc_arg(value)?;
                st.plane_mut(&self.cur, i).ok_or(Errno::EINVAL)?.crtc = c;
                if let Some(c) = c {
                    st.crtc_mut(&self.cur, c);
                }
            }
            (ObjKind::Plane(i), p) => {
                let s = st.plane_mut(&self.cur, i).ok_or(Errno::EINVAL)?;
                match p {
                    Prop::CrtcX => s.crtc_x = value as i64 as i32,
                    Prop::CrtcY => s.crtc_y = value as i64 as i32,
                    Prop::CrtcW => s.crtc_w = value as u32,
                    Prop::CrtcH => s.crtc_h = value as u32,
                    Prop::SrcX => s.src_x = value as u32,
                    Prop::SrcY => s.src_y = value as u32,
                    Prop::SrcW => s.src_w = value as u32,
                    Prop::SrcH => s.src_h = value as u32,
                    _ => return Err(Errno::EINVAL),
                }
            }
            (ObjKind::Connector(i), Prop::CrtcId) => {
                let c = crtc_arg(value)?;
                let old = st.connector_mut(&self.cur, i).ok_or(Errno::EINVAL)?.crtc;
                if let Some(o) = old {
                    st.crtc_mut(&self.cur, o);
                }
                if let Some(n) = c {
                    st.crtc_mut(&self.cur, n);
                }
                st.connector_mut(&self.cur, i).ok_or(Errno::EINVAL)?.crtc = c;
            }
            (ObjKind::Connector(i), Prop::LinkStatus) => {
                let s = st.connector_mut(&self.cur, i).ok_or(Errno::EINVAL)?;
                // Userland may restore GOOD but never downgrade to BAD; the
                // attempt is ignored, as in Linux.
                if s.link_status != uapi::DRM_MODE_LINK_STATUS_GOOD {
                    s.link_status = value as u32;
                }
            }
            // DPMS is legacy-only; Linux refuses atomic writes to it.
            _ => return Err(Errno::EINVAL),
        }
        Ok(())
    }

    /// Checks an update; see the module documentation. `event_mask` names
    /// the CRTCs the caller wants events for.
    pub fn check(&self, st: &mut AtomicState<B>, event_mask: u32, drv: &dyn KmsDriver<B>) -> Result<(), Errno> {
        self.route_connectors(st)?;
        let ncrtc = self.layout.crtcs.len();
        for i in 0..ncrtc {
            let Some(new) = st.crtcs.get(i).and_then(Option::as_ref) else { continue };
            let old = self.cur.crtcs.get(i).ok_or(Errno::EINVAL)?;
            if new.active && !new.enable() {
                return Err(Errno::EINVAL);
            }
            if event_mask & (1 << i) != 0 && !new.active && !old.active {
                return Err(Errno::EINVAL);
            }
            let view = StateView { cur: &self.cur, new: st };
            let has_conn =
                (0..self.layout.connectors.len()).any(|c| view.connector(c).is_some_and(|s| s.crtc == Some(i)));
            if new.enable() != has_conn {
                return Err(Errno::EINVAL);
            }
            if view.needs_modeset(i) {
                if !st.flags.allow_modeset {
                    return Err(Errno::EINVAL);
                }
                for p in 0..self.layout.planes.len() {
                    let on = |s: Option<&super::state::PlaneState<B>>| s.is_some_and(|s| s.crtc == Some(i));
                    if on(self.cur.planes.get(p)) || on(st.planes.get(p).and_then(Option::as_ref)) {
                        st.plane_mut(&self.cur, p);
                    }
                }
                for c in 0..self.layout.connectors.len() {
                    if self.cur.connectors.get(c).is_some_and(|s| s.crtc == Some(i)) {
                        st.connector_mut(&self.cur, c);
                    }
                }
            }
        }
        for p in 0..self.layout.planes.len() {
            let Some(new) = st.planes.get(p).and_then(Option::as_ref) else { continue };
            self.check_plane_core(p, new, drv)?;
        }
        let view = StateView { cur: &self.cur, new: st };
        for (c, s) in st.connectors.iter().enumerate() {
            if let Some(s) = s {
                drv.check_connector(c, s)?;
            }
        }
        for (i, s) in st.crtcs.iter().enumerate() {
            if let Some(s) = s {
                drv.check_crtc(i, s, &view)?;
            }
        }
        for (p, s) in st.planes.iter().enumerate() {
            if let Some(s) = s {
                drv.check_plane(p, s, s.crtc.and_then(|c| view.crtc(c)))?;
            }
        }
        Ok(())
    }

    fn check_plane_core(
        &self,
        p: usize,
        new: &super::state::PlaneState<B>,
        drv: &dyn KmsDriver<B>,
    ) -> Result<(), Errno> {
        let (Some(c), Some(fb)) = (new.crtc, new.fb.as_ref()) else {
            return if new.crtc.is_some() || new.fb.is_some() { Err(Errno::EINVAL) } else { Ok(()) };
        };
        let d = self.layout.planes.get(p).ok_or(Errno::EINVAL)?;
        if d.possible_crtcs & (1 << c) == 0 {
            return Err(Errno::EINVAL);
        }
        if !self.plane_has_format(drv, p, fb.format, fb.modifier) {
            return Err(Errno::EINVAL);
        }
        let max = i32::MAX as i64;
        let (x, y, w, h) = (i64::from(new.crtc_x), i64::from(new.crtc_y), i64::from(new.crtc_w), i64::from(new.crtc_h));
        if w > max || h > max || x > max - w || y > max - h {
            return Err(super::modes::ERANGE);
        }
        check_src_coords(new.src_x, new.src_y, new.src_w, new.src_h, fb.width, fb.height)
    }

    /// Chooses encoders for the connectors of an update.
    fn route_connectors(&self, st: &mut AtomicState<B>) -> Result<(), Errno> {
        for c in 0..self.layout.connectors.len() {
            let Some(new) = st.connectors.get(c).and_then(Option::as_ref) else { continue };
            let Some(crtc) = new.crtc else {
                if let Some(s) = st.connectors.get_mut(c).and_then(Option::as_mut) {
                    s.encoder = None;
                }
                continue;
            };
            let d = self.layout.connectors.get(c).ok_or(Errno::EINVAL)?;
            let view = StateView { cur: &self.cur, new: st };
            let taken = |e: usize| {
                (0..self.layout.connectors.len())
                    .any(|o| o != c && view.connector(o).is_some_and(|s| s.encoder == Some(e)))
            };
            let enc = (0..self.layout.encoders.len()).find(|&e| {
                d.encoders & (1 << e) != 0
                    && self.layout.encoders.get(e).is_some_and(|x| x.possible_crtcs & (1 << crtc) != 0)
                    && !taken(e)
            });
            let enc = enc.ok_or(Errno::EINVAL)?;
            if let Some(s) = st.connectors.get_mut(c).and_then(Option::as_mut) {
                s.encoder = Some(enc);
            }
            let old = self.cur.connectors.get(c).and_then(|s| s.crtc);
            if let Some(o) = old {
                st.crtc_mut(&self.cur, o);
            }
            st.crtc_mut(&self.cur, crtc);
        }
        // A connector that moves away leaves its old encoder free; one that
        // other connectors keep is checked above.
        Ok(())
    }

    /// Applies a checked update. See the module documentation.
    pub fn commit(&mut self, mut st: AtomicState<B>, drv: &dyn KmsDriver<B>, ctx: &Sleepable) -> Committed<B> {
        let ncrtc = self.layout.crtcs.len();
        let (modeset, plane_changed): (Vec<bool>, Vec<bool>) = {
            let view = StateView { cur: &self.cur, new: &st };
            (
                (0..ncrtc).map(|i| view.needs_modeset(i)).collect(),
                (0..self.layout.planes.len()).map(|p| view.plane_changed(p)).collect(),
            )
        };
        // The driver's hooks, in order.
        for (i, new) in st.crtcs.iter().enumerate() {
            if let (Some(n), Some(o)) = (new, self.cur.crtcs.get(i))
                && !n.active
            {
                drv.commit_crtc(i, o, n, ctx);
            }
        }
        for (p, new) in st.planes.iter().enumerate() {
            if let (Some(n), Some(o)) = (new, self.cur.planes.get(p))
                && plane_changed.get(p).copied().unwrap_or(true)
            {
                drv.commit_plane(p, o, n, ctx);
            }
        }
        for (i, new) in st.crtcs.iter().enumerate() {
            if let (Some(n), Some(o)) = (new, self.cur.crtcs.get(i))
                && n.active
            {
                drv.commit_crtc(i, o, n, ctx);
            }
        }
        for (c, new) in st.connectors.iter().enumerate() {
            if let (Some(n), Some(o)) = (new, self.cur.connectors.get(c)) {
                drv.commit_connector(c, o, n, ctx);
            }
        }
        // Swap the new states in.
        let mut old = OldState { crtcs: Vec::new(), planes: Vec::new() };
        let (mut flip_mask, mut off_mask) = (0u32, 0u32);
        for (i, new) in st.crtcs.iter_mut().enumerate() {
            let (Some(n), Some(cur)) = (new.take(), self.cur.crtcs.get_mut(i)) else { continue };
            let was = cur.active;
            let changed_mode = modeset.get(i).copied().unwrap_or(false);
            if n.active {
                flip_mask |= 1 << i;
            } else {
                off_mask |= 1 << i;
            }
            if was != n.active || changed_mode {
                let frame_ns = n.mode.and_then(|m| m.frame_ns()).unwrap_or(0);
                self.transitions.push(Transition { crtc: i, on: n.active, frame_ns });
            }
            old.crtcs.push(core::mem::replace(cur, n));
        }
        for (p, new) in st.planes.iter_mut().enumerate() {
            if let (Some(n), Some(cur)) = (new.take(), self.cur.planes.get_mut(p)) {
                old.planes.push(core::mem::replace(cur, n));
            }
        }
        let mut conn_old = Vec::new();
        for (c, new) in st.connectors.iter_mut().enumerate() {
            if let (Some(n), Some(cur)) = (new.take(), self.cur.connectors.get_mut(c)) {
                conn_old.push((c, core::mem::replace(cur, n)));
            }
        }
        // Legacy DPMS bookkeeping (drm_atomic_helper_update_legacy_modeset_state).
        for (c, o) in conn_old {
            let crtc = self.cur.connectors.get(c).and_then(|s| s.crtc);
            let modeset_here = crtc.is_some_and(|k| modeset.get(k).copied().unwrap_or(false));
            if (crtc.is_none() && o.crtc.is_some()) || modeset_here {
                let on = crtc.and_then(|k| self.cur.crtcs.get(k)).is_some_and(|s| s.active);
                if let Some(info) = self.conns.get_mut(c) {
                    info.dpms = if on { uapi::DRM_MODE_DPMS_ON } else { uapi::DRM_MODE_DPMS_OFF };
                }
            }
        }
        for (c, d) in st.dpms.iter().enumerate() {
            if let (Some(d), Some(info)) = (d, self.conns.get_mut(c)) {
                info.dpms = *d;
            }
        }
        Committed { old, flip_mask, off_mask }
    }

    /// Takes the vblank transitions of the commits since the last call.
    pub fn take_transitions(&mut self) -> Vec<Transition> {
        core::mem::take(&mut self.transitions)
    }

    /// Turns off every plane that shows one of `fbs`, and the CRTC of each
    /// such primary plane (Linux's `atomic_remove_fb`). The update is
    /// always allowed and skips the driver's checks: turning things off
    /// cannot be refused.
    pub(super) fn disable_fbs(
        &mut self,
        fbs: &[FbRef<B>],
        drv: &dyn KmsDriver<B>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let mut st = self.begin(CommitFlags { allow_modeset: true, ..CommitFlags::default() });
        for p in 0..self.layout.planes.len() {
            let uses = self
                .cur
                .planes
                .get(p)
                .and_then(|s| s.fb.as_ref())
                .is_some_and(|f| fbs.iter().any(|x| alloc::sync::Arc::ptr_eq(x, f)));
            if !uses {
                continue;
            }
            let crtc = self.cur.planes.get(p).and_then(|s| s.crtc);
            if let Some(s) = st.plane_mut(&self.cur, p) {
                s.crtc = None;
                s.fb = None;
            }
            let primary_of = crtc.filter(|c| self.layout.crtcs.get(*c).is_some_and(|d| d.primary == p));
            if let Some(c) = primary_of {
                if let Some(s) = st.crtc_mut(&self.cur, c) {
                    s.active = false;
                    s.mode = None;
                    s.mode_blob = None;
                }
                // The CRTC goes off, so nothing else on it can be shown
                // either: its other planes go off with it. Otherwise a legacy
                // cursor, whose framebuffer belongs to no file, would stay on
                // a dark CRTC and keep its buffer referenced after every file
                // is closed, which refuses the device's detach.
                for q in 0..self.layout.planes.len() {
                    if q != p
                        && self.cur.planes.get(q).is_some_and(|s| s.crtc == Some(c))
                        && let Some(s) = st.plane_mut(&self.cur, q)
                    {
                        s.crtc = None;
                        s.fb = None;
                    }
                }
                for k in 0..self.layout.connectors.len() {
                    if self.cur.connectors.get(k).is_some_and(|s| s.crtc == Some(c))
                        && let Some(s) = st.connector_mut(&self.cur, k)
                    {
                        s.crtc = None;
                        s.encoder = None;
                    }
                }
            }
        }
        if st.is_empty() {
            return Ok(());
        }
        let done = self.commit(st, drv, ctx);
        release_old(done.old, ctx);
        Ok(())
    }

    /// The type of plane `p`.
    pub fn plane_type(&self, p: usize) -> Option<PlaneType> {
        self.layout.planes.get(p).map(|d| d.typ)
    }
}

/// Checks a source rectangle against a framebuffer
/// (`drm_framebuffer_check_src_coords`): `ENOSPC` if it leaves the image.
pub fn check_src_coords(x: u32, y: u32, w: u32, h: u32, fb_w: u32, fb_h: u32) -> Result<(), Errno> {
    let (fw, fh) = (u64::from(fb_w) << 16, u64::from(fb_h) << 16);
    let (x, y, w, h) = (u64::from(x), u64::from(y), u64::from(w), u64::from(h));
    if w > fw || x > fw - w || h > fh || y > fh - h {
        return Err(ENOSPC);
    }
    Ok(())
}

/// Releases a failed or test-only update in thread context.
pub fn release_state<B: FbBacking>(st: AtomicState<B>, ctx: &Sleepable) {
    for p in st.planes.into_iter().flatten() {
        if let Some(fb) = p.fb {
            super::core::release_fb(fb, ctx);
        }
    }
}
