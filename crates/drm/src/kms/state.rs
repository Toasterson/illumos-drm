// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Object state and atomic updates.
//!
//! The current state of every CRTC, plane and connector is a value
//! ([`CrtcState`], [`PlaneState`], [`ConnectorState`]). An atomic update
//! ([`AtomicState`]) starts empty; the first change to an object copies
//! the object's current state into it (duplication, Linux's
//! `atomic_duplicate_state`), and later changes edit the copy. Checking
//! reads the copies and the current state of untouched objects through a
//! [`StateView`]; committing swaps the copies in and hands the replaced
//! states back ([`OldState`]) so that the caller releases their references
//! in thread context. Dropping an `AtomicState` without committing is the
//! rollback: nothing current was ever changed.
//!
//! [`KmsDriver`] is what a driver with display hardware implements: its
//! [`Layout`], connector probing, and check and commit hooks per CRTC,
//! plane and connector.

use alloc::vec::Vec;

use ddi::{Errno, Sleepable};

use super::modes::DisplayMode;
use super::objects::{BlobRef, FbRef, Layout};

/// The state of a CRTC.
#[derive(Clone, Debug, Default)]
pub struct CrtcState {
    /// Whether the CRTC scans out (`ACTIVE`).
    pub active: bool,
    /// The mode, decoded from `mode_blob`.
    pub mode: Option<DisplayMode>,
    /// The `MODE_ID` blob; a CRTC is enabled when it has one.
    pub mode_blob: Option<BlobRef>,
}

impl CrtcState {
    /// Whether the CRTC has a mode (Linux's `crtc_state->enable`).
    pub fn enable(&self) -> bool {
        self.mode_blob.is_some()
    }

    fn same_mode(&self, other: &CrtcState) -> bool {
        match (&self.mode, &other.mode) {
            (Some(a), Some(b)) => a.same_timings(b),
            (None, None) => true,
            _ => false,
        }
    }
}

/// The state of a plane. Coordinates are those of the uAPI: `crtc_*` in
/// pixels on the CRTC, `src_*` in 16.16 fixed point in the framebuffer.
#[derive(Debug)]
pub struct PlaneState<B> {
    /// The CRTC it is shown on (`CRTC_ID`), by index.
    pub crtc: Option<usize>,
    /// The framebuffer (`FB_ID`).
    pub fb: Option<FbRef<B>>,
    /// `CRTC_X`.
    pub crtc_x: i32,
    /// `CRTC_Y`.
    pub crtc_y: i32,
    /// `CRTC_W`.
    pub crtc_w: u32,
    /// `CRTC_H`.
    pub crtc_h: u32,
    /// `SRC_X`.
    pub src_x: u32,
    /// `SRC_Y`.
    pub src_y: u32,
    /// `SRC_W`.
    pub src_w: u32,
    /// `SRC_H`.
    pub src_h: u32,
}

impl<B> Clone for PlaneState<B> {
    fn clone(&self) -> Self {
        PlaneState {
            crtc: self.crtc,
            fb: self.fb.clone(),
            crtc_x: self.crtc_x,
            crtc_y: self.crtc_y,
            crtc_w: self.crtc_w,
            crtc_h: self.crtc_h,
            src_x: self.src_x,
            src_y: self.src_y,
            src_w: self.src_w,
            src_h: self.src_h,
        }
    }
}

impl<B> Default for PlaneState<B> {
    fn default() -> Self {
        PlaneState {
            crtc: None,
            fb: None,
            crtc_x: 0,
            crtc_y: 0,
            crtc_w: 0,
            crtc_h: 0,
            src_x: 0,
            src_y: 0,
            src_w: 0,
            src_h: 0,
        }
    }
}

impl<B> PlaneState<B> {
    /// Whether the plane shows something: a framebuffer on a CRTC.
    pub fn visible(&self) -> bool {
        self.crtc.is_some() && self.fb.is_some()
    }

    fn same(&self, o: &PlaneState<B>) -> bool {
        let fb_eq = match (&self.fb, &o.fb) {
            (Some(a), Some(b)) => alloc::sync::Arc::ptr_eq(a, b),
            (None, None) => true,
            _ => false,
        };
        fb_eq
            && self.crtc == o.crtc
            && (self.crtc_x, self.crtc_y, self.crtc_w, self.crtc_h) == (o.crtc_x, o.crtc_y, o.crtc_w, o.crtc_h)
            && (self.src_x, self.src_y, self.src_w, self.src_h) == (o.src_x, o.src_y, o.src_w, o.src_h)
    }
}

/// The state of a connector.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ConnectorState {
    /// The CRTC it is driven by (`CRTC_ID`), by index.
    pub crtc: Option<usize>,
    /// The encoder between them, chosen by the core during the check.
    pub encoder: Option<usize>,
    /// `link-status`.
    pub link_status: u32,
}

/// The current state of all objects.
#[derive(Debug)]
pub struct Current<B> {
    /// Per CRTC.
    pub crtcs: Vec<CrtcState>,
    /// Per plane.
    pub planes: Vec<PlaneState<B>>,
    /// Per connector.
    pub connectors: Vec<ConnectorState>,
}

impl<B> Current<B> {
    /// Everything off, for a layout.
    pub fn new(l: &Layout) -> Current<B> {
        Current {
            crtcs: (0..l.crtcs.len()).map(|_| CrtcState::default()).collect(),
            planes: (0..l.planes.len()).map(|_| PlaneState::default()).collect(),
            connectors: (0..l.connectors.len()).map(|_| ConnectorState::default()).collect(),
        }
    }
}

/// How an update was requested, which changes what it may do and how it
/// completes.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CommitFlags {
    /// Changes that need a full mode set are allowed
    /// (`DRM_MODE_ATOMIC_ALLOW_MODESET`; always for legacy and internal
    /// updates).
    pub allow_modeset: bool,
    /// Return without waiting for the update to reach the screen
    /// (`DRM_MODE_ATOMIC_NONBLOCK`, legacy page flips).
    pub nonblock: bool,
    /// A legacy cursor update: never waits and never stalls on a pending
    /// flip.
    pub cursor: bool,
}

/// An atomic update: copies of the objects it changes.
#[derive(Debug)]
pub struct AtomicState<B> {
    /// New CRTC states, for the CRTCs the update touches.
    pub crtcs: Vec<Option<CrtcState>>,
    /// New plane states.
    pub planes: Vec<Option<PlaneState<B>>>,
    /// New connector states.
    pub connectors: Vec<Option<ConnectorState>>,
    /// How the update was requested.
    pub flags: CommitFlags,
    /// Legacy DPMS values to store when the update commits.
    pub dpms: Vec<Option<u32>>,
}

impl<B> AtomicState<B> {
    /// An empty update for a layout.
    pub fn new(l: &Layout, flags: CommitFlags) -> AtomicState<B> {
        AtomicState {
            crtcs: (0..l.crtcs.len()).map(|_| None).collect(),
            planes: (0..l.planes.len()).map(|_| None).collect(),
            connectors: (0..l.connectors.len()).map(|_| None).collect(),
            flags,
            dpms: (0..l.connectors.len()).map(|_| None).collect(),
        }
    }

    /// The new state of CRTC `i`, duplicated from `cur` on first use.
    pub fn crtc_mut(&mut self, cur: &Current<B>, i: usize) -> Option<&mut CrtcState> {
        let slot = self.crtcs.get_mut(i)?;
        if slot.is_none() {
            *slot = Some(cur.crtcs.get(i)?.clone());
        }
        slot.as_mut()
    }

    /// The new state of plane `i`, duplicated from `cur` on first use.
    /// The CRTC the plane is on now is added to the update too, so that
    /// events and checks see it.
    pub fn plane_mut(&mut self, cur: &Current<B>, i: usize) -> Option<&mut PlaneState<B>> {
        if self.planes.get(i)?.is_none() {
            let dup = cur.planes.get(i)?.clone();
            if let Some(c) = dup.crtc {
                self.crtc_mut(cur, c);
            }
            *self.planes.get_mut(i)? = Some(dup);
        }
        self.planes.get_mut(i)?.as_mut()
    }

    /// The new state of connector `i`, duplicated from `cur` on first use.
    pub fn connector_mut(&mut self, cur: &Current<B>, i: usize) -> Option<&mut ConnectorState> {
        let slot = self.connectors.get_mut(i)?;
        if slot.is_none() {
            *slot = Some(cur.connectors.get(i)?.clone());
        }
        slot.as_mut()
    }

    /// Mask of the CRTCs the update touches.
    pub fn crtc_mask(&self) -> u32 {
        self.crtcs.iter().enumerate().filter(|(_, c)| c.is_some()).fold(0, |m, (i, _)| m | 1 << i)
    }

    /// Whether the update touches nothing.
    pub fn is_empty(&self) -> bool {
        self.crtcs.iter().all(Option::is_none)
            && self.planes.iter().all(Option::is_none)
            && self.connectors.iter().all(Option::is_none)
            && self.dpms.iter().all(Option::is_none)
    }
}

/// The states an update replaced, returned by the commit so that the
/// caller releases them (framebuffer references) in thread context.
#[derive(Debug)]
pub struct OldState<B> {
    /// Replaced CRTC states.
    pub crtcs: Vec<CrtcState>,
    /// Replaced plane states.
    pub planes: Vec<PlaneState<B>>,
}

/// The state an update would produce: its copies over the current state.
pub struct StateView<'a, B> {
    /// The current state.
    pub cur: &'a Current<B>,
    /// The update.
    pub new: &'a AtomicState<B>,
}

impl<B> StateView<'_, B> {
    /// The new state of CRTC `i`.
    pub fn crtc(&self, i: usize) -> Option<&CrtcState> {
        self.new.crtcs.get(i).and_then(Option::as_ref).or_else(|| self.cur.crtcs.get(i))
    }
    /// The new state of plane `i`.
    pub fn plane(&self, i: usize) -> Option<&PlaneState<B>> {
        self.new.planes.get(i).and_then(Option::as_ref).or_else(|| self.cur.planes.get(i))
    }
    /// The new state of connector `i`.
    pub fn connector(&self, i: usize) -> Option<&ConnectorState> {
        self.new.connectors.get(i).and_then(Option::as_ref).or_else(|| self.cur.connectors.get(i))
    }
    /// Whether CRTC `i` needs a full mode set: its mode, `ACTIVE` or its
    /// connectors change (Linux's `drm_atomic_crtc_needs_modeset`).
    pub fn needs_modeset(&self, i: usize) -> bool {
        let (Some(old), Some(new)) = (self.cur.crtcs.get(i), self.new.crtcs.get(i).and_then(Option::as_ref)) else {
            return false;
        };
        if old.active != new.active || old.enable() != new.enable() || !old.same_mode(new) {
            return true;
        }
        let routed = |s: &ConnectorState| s.crtc == Some(i);
        self.cur.connectors.iter().enumerate().any(|(c, old)| self.connector(c).is_some_and(|n| routed(old) != routed(n)))
    }
    /// Whether plane `i` changes in this update.
    pub fn plane_changed(&self, i: usize) -> bool {
        match (self.cur.planes.get(i), self.new.planes.get(i).and_then(Option::as_ref)) {
            (Some(o), Some(n)) => !o.same(n),
            _ => false,
        }
    }
}

/// Whether a connector reports a display (`drm_connector_status`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ConnectorStatus {
    /// A display is connected.
    Connected = 1,
    /// Nothing is connected.
    Disconnected = 2,
    /// Cannot tell.
    #[default]
    Unknown = 3,
}

/// What probing a connector found.
#[derive(Clone, Debug, Default)]
pub struct Probe {
    /// Whether a display is connected.
    pub status: ConnectorStatus,
    /// The display's EDID, if it has one; the core parses its modes and
    /// publishes it as the `EDID` property.
    pub edid: Option<Vec<u8>>,
    /// Modes the driver adds to those of the EDID (all modes of a display
    /// without EDID). With neither EDID nor modes, a connected connector
    /// gets the DMT modes up to 1024 by 768.
    pub modes: Vec<DisplayMode>,
    /// Physical size in mm, if not in the EDID.
    pub width_mm: u32,
    /// Physical size in mm, if not in the EDID.
    pub height_mm: u32,
}

/// A buffer that backs framebuffers: `drm::GemRef` in the kernel.
pub trait FbBacking: Send + Sync + Sized + 'static {
    /// The size of the buffer in bytes.
    fn size(&self) -> u64;
    /// Drops the reference in thread context.
    fn release(self, ctx: &Sleepable);
}

/// The display side of a driver. [`crate::Driver::kms`] returns it for a
/// driver that has display hardware; a render-only driver implements
/// nothing here.
///
/// The check hooks run after the core's own checks, for the objects an
/// update touches (and those it drags in: the planes and connectors of a
/// CRTC that needs a full mode set), and may refuse the update with any
/// errno, usually `EINVAL`. The commit hooks run only for updates that
/// passed every check and cannot fail. They run in this order: CRTCs that
/// end inactive, then planes, then CRTCs that end active, then connectors;
/// each hook sees the old and the new state. The core calls all hooks with
/// its mode-setting lock held, in thread context, never concurrently.
pub trait KmsDriver<B: FbBacking>: Send + Sync {
    /// Describes the display pipeline; called once when the device
    /// attaches.
    fn layout(&self) -> Layout;

    /// Probes connector `i`: status, EDID, modes.
    fn probe(&self, connector: usize, ctx: &Sleepable) -> Probe;

    /// Whether plane `plane` can scan out `format` with `modifier`, beyond
    /// being listed in its [`super::objects::PlaneDesc`]. The default
    /// accepts every listed combination.
    fn format_mod_supported(&self, plane: usize, format: u32, modifier: u64) -> bool {
        let _ = (plane, format, modifier);
        true
    }

    /// Checks the new state of a plane. `crtc` is the new state of the
    /// CRTC it is on, if any.
    fn check_plane(&self, plane: usize, new: &PlaneState<B>, crtc: Option<&CrtcState>) -> Result<(), Errno> {
        let _ = (plane, new, crtc);
        Ok(())
    }

    /// Checks the new state of a CRTC.
    fn check_crtc(&self, crtc: usize, new: &CrtcState, view: &StateView<'_, B>) -> Result<(), Errno> {
        let _ = (crtc, new, view);
        Ok(())
    }

    /// Checks the new state of a connector.
    fn check_connector(&self, connector: usize, new: &ConnectorState) -> Result<(), Errno> {
        let _ = (connector, new);
        Ok(())
    }

    /// Applies a CRTC's new state.
    fn commit_crtc(&self, crtc: usize, old: &CrtcState, new: &CrtcState, ctx: &Sleepable) {
        let _ = (crtc, old, new, ctx);
    }

    /// Applies a plane's new state.
    fn commit_plane(&self, plane: usize, old: &PlaneState<B>, new: &PlaneState<B>, ctx: &Sleepable) {
        let _ = (plane, old, new, ctx);
    }

    /// Applies a connector's new state.
    fn commit_connector(&self, connector: usize, old: &ConnectorState, new: &ConnectorState, ctx: &Sleepable) {
        let _ = (connector, old, new, ctx);
    }

    /// Loads a legacy gamma table (`SETGAMMA`); the core has stored it
    /// already. The default does nothing.
    fn set_gamma(&self, crtc: usize, r: &[u16], g: &[u16], b: &[u16]) {
        let _ = (crtc, r, g, b);
    }
}

/// A plane check for drivers, after Linux's
/// `drm_atomic_helper_check_plane_state`: a visible plane must lie on its
/// CRTC's active area; without `can_position` it must cover it exactly,
/// and without `can_scale` the source and destination sizes must match.
/// A plane on a disabled CRTC passes (it is invisible). Failures are
/// `EINVAL`.
pub fn check_plane_helper<B>(
    new: &PlaneState<B>,
    crtc: Option<&CrtcState>,
    can_position: bool,
    can_scale: bool,
) -> Result<(), Errno> {
    let (Some(_), Some(c)) = (&new.fb, crtc) else { return Ok(()) };
    let Some(mode) = c.mode.filter(|_| c.enable()) else { return Ok(()) };
    let (w, h) = (i64::from(mode.hdisplay), i64::from(mode.vdisplay));
    let (x, y, cw, ch) = (i64::from(new.crtc_x), i64::from(new.crtc_y), i64::from(new.crtc_w), i64::from(new.crtc_h));
    if !can_scale && (u64::from(new.src_w) != (cw as u64) << 16 || u64::from(new.src_h) != (ch as u64) << 16) {
        return Err(Errno::EINVAL);
    }
    if !can_position && (x != 0 || y != 0 || cw != w || ch != h) {
        return Err(Errno::EINVAL);
    }
    // A positioned plane must still touch the screen to be visible; Linux
    // clips it and marks it invisible instead, which is only a question of
    // what the hardware shows. Planes entirely off screen are accepted.
    Ok(())
}
