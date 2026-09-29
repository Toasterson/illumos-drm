// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The mode-setting state of one device and the ioctls that read it.
//!
//! [`KmsCore`] holds the mode objects of a device: the static pipeline
//! from the driver's [`Layout`] with their IDs, the property IDs, the
//! connectors' probe results, the current state of every object, the
//! registry of blobs and framebuffers, and per open file the framebuffers
//! and blobs it created. It is plain data: the kernel layer
//! ([`super::dev`]) keeps it behind one mutex, the mode-setting lock, and
//! copies arguments in and out; the host tests drive it directly.
//!
//! Atomic updates are in [`super::atomic`], the legacy ioctls built on
//! them in [`super::legacy`].

use alloc::collections::{BTreeMap, BTreeSet};
use alloc::sync::Arc;
use alloc::vec::Vec;

use ddi::{Errno, Sleepable};
use drm_uapi as uapi;

use super::edid;
use super::formats;
use super::modes::{self, DisplayMode};
use super::objects::{Blob, BlobRef, FbRef, FbRequest, Framebuffer, Ids, Layout, ObjKind, check_fb_request};
use super::props::{CONNECTOR_PROPS, CRTC_PROPS, PLANE_PROPS, Prop, PropDef};
use super::state::{ConnectorStatus, Current, FbBacking, KmsDriver, OldState};

// Blob and framebuffer memory is locked kernel memory and every open file
// may create them without authentication, so, unlike Linux (which bounds a
// blob by INT_MAX and nothing else), the core bounds them per file and per
// device; going past a bound is ENOMEM.

/// The largest blob a client may create.
pub const MAX_BLOB: usize = 1 << 20;
/// The most blob bytes one file may hold.
pub const MAX_FILE_BLOB_BYTES: usize = 4 << 20;
/// The most blobs one file may hold.
pub const MAX_FILE_BLOBS: usize = 256;
/// The most blob bytes of all files of a device together.
pub const MAX_DEVICE_BLOB_BYTES: usize = 16 << 20;
/// The most blobs a device holds, its own (IN_FORMATS, EDID, legacy
/// modes) included.
pub const MAX_DEVICE_BLOBS: usize = 4096;
/// The most framebuffers one file may hold.
pub const MAX_FILE_FBS: usize = 256;
/// The most framebuffers a device holds, legacy cursor images included.
pub const MAX_DEVICE_FBS: usize = 4096;

/// What a connector's last probe found.
#[derive(Clone, Debug, Default)]
pub struct ConnInfo {
    /// Connection status.
    pub status: ConnectorStatus,
    /// Modes, validated and sorted.
    pub modes: Vec<DisplayMode>,
    /// The `EDID` blob.
    pub edid: Option<BlobRef>,
    /// Physical size.
    pub width_mm: u32,
    /// Physical size.
    pub height_mm: u32,
    /// The legacy DPMS state (`DRM_MODE_DPMS_*`).
    pub dpms: u32,
    /// The number of this connector among those of its type, from 1.
    pub type_id: u32,
}

/// The mode objects a file created.
#[derive(Debug, Default)]
pub struct FileKms {
    /// Its framebuffers, oldest first.
    pub fbs: Vec<u32>,
    /// Its blobs.
    pub blobs: Vec<u32>,
    /// The bytes of its blobs.
    pub blob_bytes: usize,
}

/// `GETRESOURCES`.
#[derive(Debug, PartialEq, Eq)]
pub struct Resources {
    /// The caller's framebuffers, newest first (Linux lists them so).
    pub fbs: Vec<u32>,
    /// CRTC IDs.
    pub crtcs: Vec<u32>,
    /// Connector IDs.
    pub connectors: Vec<u32>,
    /// Encoder IDs.
    pub encoders: Vec<u32>,
    /// Framebuffer size limits: min width, max width, min height, max height.
    pub limits: [u32; 4],
}

/// `GETCRTC`.
#[derive(Debug, PartialEq, Eq)]
pub struct CrtcInfo {
    /// The primary plane's framebuffer, or 0.
    pub fb_id: u32,
    /// The primary plane's source position, in pixels.
    pub x: u32,
    /// The primary plane's source position, in pixels.
    pub y: u32,
    /// The legacy gamma size.
    pub gamma_size: u32,
    /// The mode, if the CRTC has one.
    pub mode: Option<DisplayMode>,
}

/// `GETENCODER`.
#[derive(Debug, PartialEq, Eq)]
pub struct EncoderInfo {
    /// `DRM_MODE_ENCODER_*`.
    pub typ: u32,
    /// The CRTC driving it, or 0.
    pub crtc_id: u32,
    /// Mask of CRTC indexes.
    pub possible_crtcs: u32,
    /// Mask of encoder indexes.
    pub possible_clones: u32,
}

/// `GETPLANE`.
#[derive(Debug, PartialEq, Eq)]
pub struct PlaneInfo {
    /// Its CRTC, or 0.
    pub crtc_id: u32,
    /// Its framebuffer, or 0.
    pub fb_id: u32,
    /// Mask of CRTC indexes.
    pub possible_crtcs: u32,
    /// Its formats.
    pub formats: Vec<u32>,
}

/// `GETCONNECTOR`.
#[derive(Debug)]
pub struct ConnectorInfo {
    /// `DRM_MODE_CONNECTOR_*`.
    pub typ: u32,
    /// Its number among connectors of its type.
    pub type_id: u32,
    /// Connection status.
    pub status: u32,
    /// Physical size.
    pub width_mm: u32,
    /// Physical size.
    pub height_mm: u32,
    /// Its current encoder, or 0.
    pub encoder_id: u32,
    /// The encoders it can use.
    pub encoders: Vec<u32>,
    /// The modes, as the caller may see them.
    pub modes: Vec<uapi::drm_mode_modeinfo>,
    /// Property IDs and values.
    pub props: Vec<(u32, u64)>,
}

/// The mode-setting state of a device. See the module documentation.
pub struct KmsCore<B: FbBacking> {
    pub(super) layout: Layout,
    pub(super) ids: Ids,
    prop_ids: [u32; Prop::ALL.len()],
    pub(super) plane_ids: Vec<u32>,
    pub(super) crtc_ids: Vec<u32>,
    pub(super) encoder_ids: Vec<u32>,
    pub(super) connector_ids: Vec<u32>,
    pub(super) conns: Vec<ConnInfo>,
    in_formats: Vec<BlobRef>,
    pub(super) gamma: Vec<Vec<u16>>,
    pub(super) cursor_pos: Vec<(i32, i32)>,
    /// The current state of every object.
    pub cur: Current<B>,
    pub(super) blobs: BTreeMap<u32, BlobRef>,
    pub(super) fbs: BTreeMap<u32, FbRef<B>>,
    /// Blobs and framebuffers no file owns any more, freed once no state
    /// refers to them.
    pub(super) orphan_blobs: BTreeSet<u32>,
    pub(super) orphan_fbs: BTreeSet<u32>,
    pub(super) files: BTreeMap<u32, FileKms>,
    /// Vblank on/off changes not yet applied by the event layer.
    pub(super) transitions: Vec<super::atomic::Transition>,
}

impl<B: FbBacking> KmsCore<B> {
    /// Sets up the mode objects of a device from its driver: validates the
    /// layout (`EINVAL`), assigns IDs, builds the `IN_FORMATS` blobs, sets
    /// linear gamma ramps and probes every connector.
    pub fn new(drv: &dyn KmsDriver<B>, ctx: &Sleepable) -> Result<KmsCore<B>, Errno> {
        let layout = drv.layout();
        layout.validate().map_err(|_| Errno::EINVAL)?;
        let mut ids = Ids::new();
        let mut prop_ids = [0u32; Prop::ALL.len()];
        for (i, p) in Prop::ALL.iter().enumerate() {
            prop_ids[i] = ids.alloc(ObjKind::Property(*p as usize), ctx)?;
        }
        let mut alloc_all = |n: usize, k: fn(usize) -> ObjKind| -> Result<Vec<u32>, Errno> {
            (0..n).map(|i| ids.alloc(k(i), ctx)).collect()
        };
        let plane_ids = alloc_all(layout.planes.len(), ObjKind::Plane)?;
        let crtc_ids = alloc_all(layout.crtcs.len(), ObjKind::Crtc)?;
        let encoder_ids = alloc_all(layout.encoders.len(), ObjKind::Encoder)?;
        let connector_ids = alloc_all(layout.connectors.len(), ObjKind::Connector)?;
        let mut blobs = BTreeMap::new();
        let mut in_formats = Vec::new();
        for (i, p) in layout.planes.iter().enumerate() {
            let data = formats::in_formats_blob(&p.formats, layout.plane_modifiers(i), |f, m| {
                drv.format_mod_supported(i, f, m)
            });
            let id = ids.alloc(ObjKind::Blob, ctx)?;
            let b = Arc::new(Blob { id, data });
            blobs.insert(id, b.clone());
            in_formats.push(b);
        }
        let mut conns = Vec::new();
        for (i, c) in layout.connectors.iter().enumerate() {
            let type_id =
                layout.connectors.get(..i).map_or(0, |h| h.iter().filter(|o| o.typ == c.typ).count()) as u32 + 1;
            conns.push(ConnInfo { dpms: uapi::DRM_MODE_DPMS_OFF, type_id, ..ConnInfo::default() });
        }
        let gamma = layout
            .crtcs
            .iter()
            .map(|c| {
                let n = c.gamma_size as usize;
                // Linux's drm_mode_crtc_set_gamma_size fills a linear ramp.
                (0..3 * n).map(|i| (((i % n.max(1)) as u32 * 0xffff) / (n.max(2) as u32 - 1)) as u16).collect()
            })
            .collect();
        let cur = Current::new(&layout);
        let cursor_pos = layout.crtcs.iter().map(|_| (0, 0)).collect();
        let mut k = KmsCore {
            layout,
            ids,
            prop_ids,
            plane_ids,
            crtc_ids,
            encoder_ids,
            connector_ids,
            conns,
            in_formats,
            gamma,
            cursor_pos,
            cur,
            blobs,
            fbs: BTreeMap::new(),
            orphan_blobs: BTreeSet::new(),
            orphan_fbs: BTreeSet::new(),
            files: BTreeMap::new(),
            transitions: Vec::new(),
        };
        for i in 0..k.layout.connectors.len() {
            k.probe(i, drv, ctx)?;
        }
        Ok(k)
    }

    /// The layout.
    pub fn layout(&self) -> &Layout {
        &self.layout
    }

    /// The ID of a standard property.
    pub fn prop_id(&self, p: Prop) -> u32 {
        self.prop_ids.get(p as usize).copied().unwrap_or(0)
    }

    /// The CRTC IDs, by index.
    pub fn crtc_ids(&self) -> &[u32] {
        &self.crtc_ids
    }

    /// Looks up an object of a given kind. `kind` compares by variant.
    pub(super) fn find(&self, id: u32) -> Option<ObjKind> {
        self.ids.get(id)
    }

    pub(super) fn crtc_index(&self, id: u32) -> Option<usize> {
        match self.ids.get(id)? {
            ObjKind::Crtc(i) => Some(i),
            _ => None,
        }
    }

    pub(super) fn plane_index(&self, id: u32) -> Option<usize> {
        match self.ids.get(id)? {
            ObjKind::Plane(i) => Some(i),
            _ => None,
        }
    }

    pub(super) fn connector_index(&self, id: u32) -> Option<usize> {
        match self.ids.get(id)? {
            ObjKind::Connector(i) => Some(i),
            _ => None,
        }
    }

    /// Looks up a framebuffer by ID (any file's, as in Linux).
    pub fn fb(&self, id: u32) -> Option<&FbRef<B>> {
        self.fbs.get(&id)
    }

    /// Looks up a blob by ID.
    pub fn blob(&self, id: u32) -> Option<&BlobRef> {
        self.blobs.get(&id)
    }

    // --- files ---------------------------------------------------------------

    /// Registers an opened file (a clone ID).
    pub fn file_open(&mut self, file: u32, _ctx: &Sleepable) {
        self.files.insert(file, FileKms::default());
    }

    /// Forgets a closed file: its framebuffers are removed (planes that show
    /// one are turned off, see [`KmsCore::rm_fb`]) and its blobs are
    /// destroyed.
    pub fn file_close(&mut self, file: u32, drv: &dyn KmsDriver<B>, ctx: &Sleepable) -> Result<(), Errno> {
        let Some(f) = self.files.remove(&file) else { return Ok(()) };
        let fbs: Vec<FbRef<B>> = f.fbs.iter().filter_map(|id| self.fbs.get(id).cloned()).collect();
        for id in &f.fbs {
            self.orphan_fbs.insert(*id);
        }
        let r = self.disable_fbs(&fbs, drv, ctx);
        drop(fbs);
        for id in f.blobs {
            self.orphan_blobs.insert(id);
        }
        self.sweep(ctx);
        r
    }

    /// Frees blobs and framebuffers that no file owns and no state refers
    /// to, and their IDs.
    pub fn sweep(&mut self, ctx: &Sleepable) {
        let fbs: Vec<u32> = self.orphan_fbs.iter().copied().collect();
        for id in fbs {
            if self.fbs.get(&id).is_none_or(|f| Arc::strong_count(f) == 1) {
                self.orphan_fbs.remove(&id);
                if let Some(f) = self.fbs.remove(&id) {
                    release_fb(f, ctx);
                }
                self.ids.free(id);
            }
        }
        let blobs: Vec<u32> = self.orphan_blobs.iter().copied().collect();
        for id in blobs {
            if self.blobs.get(&id).is_none_or(|b| Arc::strong_count(b) == 1) {
                self.orphan_blobs.remove(&id);
                self.blobs.remove(&id);
                self.ids.free(id);
            }
        }
    }

    /// Releases every framebuffer and blob and the current state, when the
    /// device detaches. The driver must have dropped its own references.
    pub fn teardown(&mut self, ctx: &Sleepable) {
        for p in self.cur.planes.iter_mut() {
            if let Some(fb) = p.fb.take() {
                release_fb(fb, ctx);
            }
        }
        for c in self.cur.crtcs.iter_mut() {
            c.mode_blob = None;
        }
        self.files.clear();
        let fbs = core::mem::take(&mut self.fbs);
        for (id, fb) in fbs {
            release_fb(fb, ctx);
            self.ids.free(id);
        }
        self.orphan_fbs.clear();
    }

    /// The number of framebuffers and blobs alive, for statistics.
    pub fn counts(&self) -> (usize, usize) {
        (self.fbs.len(), self.blobs.len())
    }

    // --- probing -------------------------------------------------------------

    /// Probes connector `i` and updates its status, modes and `EDID` blob.
    pub fn probe(&mut self, i: usize, drv: &dyn KmsDriver<B>, ctx: &Sleepable) -> Result<(), Errno> {
        let desc = self.layout.connectors.get(i).ok_or(Errno::EINVAL)?.clone();
        let p = drv.probe(i, ctx);
        let mut modes = Vec::new();
        let (mut wmm, mut hmm) = (p.width_mm, p.height_mm);
        let parsed = p.edid.as_deref().and_then(|e| edid::parse(e).ok());
        if let Some(info) = &parsed {
            modes.extend(info.modes.iter().copied());
            if info.width_mm != 0 {
                (wmm, hmm) = (info.width_mm, info.height_mm);
            }
        }
        modes.extend(p.modes.iter().copied());
        let connected = p.status == ConnectorStatus::Connected;
        if connected && modes.is_empty() {
            modes = modes::noedid_modes(desc.limits.max_width.min(1024), desc.limits.max_height.min(768));
        }
        if !connected {
            modes.clear();
        }
        modes::finish_list(&mut modes, &desc.limits);
        // The EDID blob: the same blob while the EDID does not change.
        let new_edid = if connected && parsed.is_some() { p.edid } else { None };
        let old = self.conns.get(i).and_then(|c| c.edid.clone());
        let edid_blob = match (old, new_edid) {
            (Some(o), Some(n)) if o.data == n => Some(o),
            (old, new) => {
                if let Some(o) = old {
                    self.orphan_blobs.insert(o.id);
                }
                match new {
                    Some(data) => {
                        let id = self.ids.alloc(ObjKind::Blob, ctx)?;
                        let b = Arc::new(Blob { id, data });
                        self.blobs.insert(id, b.clone());
                        Some(b)
                    }
                    None => None,
                }
            }
        };
        let c = self.conns.get_mut(i).ok_or(Errno::EINVAL)?;
        c.status = p.status;
        c.modes = modes;
        c.edid = edid_blob;
        c.width_mm = wmm;
        c.height_mm = hmm;
        self.sweep(ctx);
        Ok(())
    }

    // --- getters -------------------------------------------------------------

    /// `GETRESOURCES`.
    pub fn resources(&self, file: u32) -> Resources {
        let fbs = self.files.get(&file).map(|f| f.fbs.iter().rev().copied().collect()).unwrap_or_default();
        Resources {
            fbs,
            crtcs: self.crtc_ids.clone(),
            connectors: self.connector_ids.clone(),
            encoders: self.encoder_ids.clone(),
            limits: [self.layout.min_width, self.layout.max_width, self.layout.min_height, self.layout.max_height],
        }
    }

    /// `GETPLANERESOURCES`: overlay planes, and primary and cursor planes
    /// for clients with `DRM_CLIENT_CAP_UNIVERSAL_PLANES`.
    pub fn plane_resources(&self, universal: bool) -> Vec<u32> {
        self.layout
            .planes
            .iter()
            .zip(&self.plane_ids)
            .filter(|(p, _)| universal || p.typ == super::objects::PlaneType::Overlay)
            .map(|(_, id)| *id)
            .collect()
    }

    /// `GETCRTC`.
    pub fn get_crtc(&self, id: u32) -> Result<CrtcInfo, Errno> {
        let i = self.crtc_index(id).ok_or(Errno::ENOENT)?;
        let desc = self.layout.crtcs.get(i).ok_or(Errno::ENOENT)?;
        let st = self.cur.crtcs.get(i).ok_or(Errno::ENOENT)?;
        let primary = self.cur.planes.get(desc.primary);
        Ok(CrtcInfo {
            fb_id: primary.and_then(|p| p.fb.as_ref()).map_or(0, |f| f.id),
            x: primary.map_or(0, |p| p.src_x >> 16),
            y: primary.map_or(0, |p| p.src_y >> 16),
            gamma_size: desc.gamma_size,
            mode: if st.enable() { st.mode } else { None },
        })
    }

    /// `GETENCODER`.
    pub fn get_encoder(&self, id: u32) -> Result<EncoderInfo, Errno> {
        let i = match self.ids.get(id) {
            Some(ObjKind::Encoder(i)) => i,
            _ => return Err(Errno::ENOENT),
        };
        let d = self.layout.encoders.get(i).ok_or(Errno::ENOENT)?;
        let crtc = self.cur.connectors.iter().find(|c| c.encoder == Some(i)).and_then(|c| c.crtc);
        Ok(EncoderInfo {
            typ: d.typ,
            crtc_id: crtc.and_then(|c| self.crtc_ids.get(c).copied()).unwrap_or(0),
            possible_crtcs: d.possible_crtcs,
            possible_clones: d.possible_clones,
        })
    }

    /// `GETPLANE`.
    pub fn get_plane(&self, id: u32) -> Result<PlaneInfo, Errno> {
        let i = self.plane_index(id).ok_or(Errno::ENOENT)?;
        let d = self.layout.planes.get(i).ok_or(Errno::ENOENT)?;
        let st = self.cur.planes.get(i).ok_or(Errno::ENOENT)?;
        Ok(PlaneInfo {
            crtc_id: st.crtc.and_then(|c| self.crtc_ids.get(c).copied()).unwrap_or(0),
            fb_id: st.fb.as_ref().map_or(0, |f| f.id),
            possible_crtcs: d.possible_crtcs,
            formats: d.formats.clone(),
        })
    }

    /// `GETCONNECTOR`. With `probe` (the caller is master and asked with
    /// `count_modes` 0), the connector is probed first. Modes carry their
    /// aspect ratio only for clients that asked for it; for the others,
    /// modes that then look the same are listed once (Linux's
    /// `drm_mode_expose_to_userspace`).
    pub fn get_connector(
        &mut self,
        id: u32,
        probe: bool,
        aspect: bool,
        atomic: bool,
        drv: &dyn KmsDriver<B>,
        ctx: &Sleepable,
    ) -> Result<ConnectorInfo, Errno> {
        let i = self.connector_index(id).ok_or(Errno::ENOENT)?;
        if probe {
            self.probe(i, drv, ctx)?;
        }
        let d = self.layout.connectors.get(i).ok_or(Errno::ENOENT)?;
        let c = self.conns.get(i).ok_or(Errno::ENOENT)?;
        let mut modes: Vec<uapi::drm_mode_modeinfo> = Vec::new();
        for m in &c.modes {
            let u = m.to_umode(aspect);
            let dup = !aspect
                && modes.iter().any(|o| {
                    let (mut a, mut b) = (*o, u);
                    a.type_ = 0;
                    b.type_ = 0;
                    a.name = [0; modes::NAME_LEN];
                    b.name = [0; modes::NAME_LEN];
                    a.vrefresh = 0;
                    b.vrefresh = 0;
                    umode_eq(&a, &b)
                });
            if !dup {
                modes.push(u);
            }
        }
        let encoders = (0..self.layout.encoders.len())
            .filter(|e| d.encoders & (1 << e) != 0)
            .filter_map(|e| self.encoder_ids.get(e).copied())
            .collect();
        let st = self.cur.connectors.get(i).ok_or(Errno::ENOENT)?;
        Ok(ConnectorInfo {
            typ: d.typ,
            type_id: c.type_id,
            status: c.status as u32,
            width_mm: c.width_mm,
            height_mm: c.height_mm,
            encoder_id: st.encoder.and_then(|e| self.encoder_ids.get(e).copied()).unwrap_or(0),
            encoders,
            modes,
            props: self.obj_props(id, 0, atomic)?,
        })
    }

    /// `GETPROPERTY`: the definition of a property.
    pub fn get_property(&self, id: u32) -> Result<&'static PropDef, Errno> {
        match self.ids.get(id) {
            Some(ObjKind::Property(i)) => Prop::ALL.get(i).map(|p| p.def()).ok_or(Errno::ENOENT),
            _ => Err(Errno::ENOENT),
        }
    }

    /// The properties of an object and their current values
    /// (`OBJ_GETPROPERTIES`): `ENOENT` for an unknown object or a type
    /// mismatch (`obj_type` 0 matches any), `EINVAL` for objects without
    /// properties. Atomic properties are listed for atomic clients only.
    pub fn obj_props(&self, id: u32, obj_type: u32, atomic: bool) -> Result<Vec<(u32, u64)>, Errno> {
        let kind = self.ids.get(id).ok_or(Errno::ENOENT)?;
        if obj_type != 0 && kind.code() != obj_type {
            return Err(Errno::ENOENT);
        }
        let list = props_of(kind).ok_or(Errno::EINVAL)?;
        Ok(list
            .iter()
            .filter(|p| atomic || !p.def().atomic)
            .map(|p| (self.prop_id(*p), self.prop_value(kind, *p)))
            .collect())
    }

    /// The current value of property `p` of an object.
    pub(super) fn prop_value(&self, kind: ObjKind, p: Prop) -> u64 {
        let crtc_id = |c: Option<usize>| c.and_then(|c| self.crtc_ids.get(c).copied()).unwrap_or(0) as u64;
        match kind {
            ObjKind::Connector(i) => {
                let st = self.cur.connectors.get(i);
                let info = self.conns.get(i);
                match p {
                    Prop::Edid => info.and_then(|c| c.edid.as_ref()).map_or(0, |b| u64::from(b.id)),
                    Prop::Dpms => info.map_or(0, |c| u64::from(c.dpms)),
                    Prop::LinkStatus => st.map_or(0, |s| u64::from(s.link_status)),
                    Prop::CrtcId => crtc_id(st.and_then(|s| s.crtc)),
                    _ => 0,
                }
            }
            ObjKind::Crtc(i) => {
                let st = self.cur.crtcs.get(i);
                match p {
                    Prop::Active => st.map_or(0, |s| u64::from(s.active)),
                    Prop::ModeId => st.and_then(|s| s.mode_blob.as_ref()).map_or(0, |b| u64::from(b.id)),
                    _ => 0,
                }
            }
            ObjKind::Plane(i) => {
                let Some(st) = self.cur.planes.get(i) else { return 0 };
                match p {
                    Prop::FbId => st.fb.as_ref().map_or(0, |f| u64::from(f.id)),
                    Prop::CrtcId => crtc_id(st.crtc),
                    Prop::CrtcX => st.crtc_x as i64 as u64,
                    Prop::CrtcY => st.crtc_y as i64 as u64,
                    Prop::CrtcW => u64::from(st.crtc_w),
                    Prop::CrtcH => u64::from(st.crtc_h),
                    Prop::SrcX => u64::from(st.src_x),
                    Prop::SrcY => u64::from(st.src_y),
                    Prop::SrcW => u64::from(st.src_w),
                    Prop::SrcH => u64::from(st.src_h),
                    Prop::Type => self.layout.planes.get(i).map_or(0, |d| d.typ as u64),
                    Prop::InFormats => self.in_formats.get(i).map_or(0, |b| u64::from(b.id)),
                    _ => 0,
                }
            }
            _ => 0,
        }
    }

    // --- blobs ---------------------------------------------------------------

    /// `CREATEPROPBLOB`: `EINVAL` for an empty blob, `ENOMEM` past one of
    /// the bounds above ([`MAX_BLOB`], [`MAX_FILE_BLOB_BYTES`],
    /// [`MAX_FILE_BLOBS`], [`MAX_DEVICE_BLOB_BYTES`], [`MAX_DEVICE_BLOBS`]).
    pub fn create_blob(&mut self, file: u32, data: Vec<u8>, ctx: &Sleepable) -> Result<u32, Errno> {
        if data.is_empty() {
            return Err(Errno::EINVAL);
        }
        let len = data.len();
        let f = self.files.get(&file).ok_or(Errno::ENXIO)?;
        let device_bytes: usize = self.files.values().map(|f| f.blob_bytes).sum();
        if len > MAX_BLOB
            || f.blob_bytes + len > MAX_FILE_BLOB_BYTES
            || f.blobs.len() >= MAX_FILE_BLOBS
            || device_bytes + len > MAX_DEVICE_BLOB_BYTES
            || self.blobs.len() >= MAX_DEVICE_BLOBS
        {
            return Err(Errno::ENOMEM);
        }
        let id = self.ids.alloc(ObjKind::Blob, ctx)?;
        self.blobs.insert(id, Arc::new(Blob { id, data }));
        let f = self.files.get_mut(&file).ok_or(Errno::ENXIO)?;
        f.blobs.push(id);
        f.blob_bytes += len;
        Ok(id)
    }

    /// `DESTROYPROPBLOB`: `ENOENT` for an unknown blob, `EPERM` for one the
    /// file did not create. A blob that a state still uses stays until it
    /// is no longer used.
    pub fn destroy_blob(&mut self, file: u32, id: u32, ctx: &Sleepable) -> Result<(), Errno> {
        let len = self.blobs.get(&id).ok_or(Errno::ENOENT)?.data.len();
        let f = self.files.get_mut(&file).ok_or(Errno::ENXIO)?;
        let pos = f.blobs.iter().position(|b| *b == id).ok_or(Errno::EPERM)?;
        f.blobs.remove(pos);
        f.blob_bytes -= len;
        self.orphan_blobs.insert(id);
        self.sweep(ctx);
        Ok(())
    }

    // --- framebuffers --------------------------------------------------------

    /// Whether any plane scans out `format` with `modifier`
    /// (`drm_any_plane_has_format`).
    pub fn any_plane_has_format(&self, drv: &dyn KmsDriver<B>, format: u32, modifier: u64) -> bool {
        (0..self.layout.planes.len()).any(|i| self.plane_has_format(drv, i, format, modifier))
    }

    /// Whether plane `i` scans out `format` with `modifier`.
    pub fn plane_has_format(&self, drv: &dyn KmsDriver<B>, i: usize, format: u32, modifier: u64) -> bool {
        self.layout.planes.get(i).is_some_and(|p| p.formats.contains(&format))
            && self.layout.plane_modifiers(i).contains(&modifier)
            && drv.format_mod_supported(i, format, modifier)
    }

    /// The checks of `ADDFB2` before the handles are looked up
    /// (`framebuffer_check`): see [`check_fb_request`]; then some plane must
    /// scan out the format with the modifier (`EINVAL`).
    pub fn check_fb(&self, r: &FbRequest, drv: &dyn KmsDriver<B>) -> Result<usize, Errno> {
        let n = check_fb_request(r, &self.layout)?;
        if !self.any_plane_has_format(drv, r.format, r.modifiers[0]) {
            return Err(Errno::EINVAL);
        }
        Ok(n)
    }

    /// Creates a framebuffer from a checked request and the objects its
    /// handles name: each object must be large enough for its plane
    /// (`EINVAL`). `file` is the owner, or `None` for an internal one (the
    /// legacy cursor). Returns the new framebuffer; the objects are
    /// released on failure.
    pub fn add_fb(
        &mut self,
        file: Option<u32>,
        r: &FbRequest,
        objs: [Option<B>; 4],
        hot: (i32, i32),
        ctx: &Sleepable,
    ) -> Result<FbRef<B>, Errno> {
        let release = |objs: [Option<B>; 4]| objs.into_iter().flatten().for_each(|o| o.release(ctx));
        for (i, o) in objs.iter().enumerate() {
            if let Some(o) = o {
                let need = super::objects::plane_min_size(r, i).unwrap_or(u64::MAX);
                if o.size() < need {
                    release(objs);
                    return Err(Errno::EINVAL);
                }
            }
        }
        let file_full = file.and_then(|f| self.files.get(&f)).is_some_and(|f| f.fbs.len() >= MAX_FILE_FBS);
        if file_full || self.fbs.len() >= MAX_DEVICE_FBS {
            release(objs);
            return Err(Errno::ENOMEM);
        }
        let id = match self.ids.alloc(ObjKind::Fb, ctx) {
            Ok(id) => id,
            Err(e) => {
                release(objs);
                return Err(e);
            }
        };
        let fb = Arc::new(Framebuffer {
            id,
            width: r.width,
            height: r.height,
            format: r.format,
            modifier: r.modifiers[0],
            flags: r.flags,
            pitches: r.pitches,
            offsets: r.offsets,
            hot_x: hot.0,
            hot_y: hot.1,
            objs,
        });
        self.fbs.insert(id, fb.clone());
        match file.and_then(|f| self.files.get_mut(&f)) {
            Some(f) => f.fbs.push(id),
            None => {
                self.orphan_fbs.insert(id);
            }
        }
        Ok(fb)
    }

    /// `RMFB`: `ENOENT` unless the framebuffer exists and belongs to the
    /// file. Planes that show it are turned off, and a CRTC whose primary
    /// plane that was is turned off with it (Linux's `atomic_remove_fb`).
    pub fn rm_fb(&mut self, file: u32, id: u32, drv: &dyn KmsDriver<B>, ctx: &Sleepable) -> Result<(), Errno> {
        let fb = self.fbs.get(&id).cloned().ok_or(Errno::ENOENT)?;
        let f = self.files.get_mut(&file).ok_or(Errno::ENOENT)?;
        let pos = f.fbs.iter().position(|x| *x == id).ok_or(Errno::ENOENT)?;
        f.fbs.remove(pos);
        self.orphan_fbs.insert(id);
        let r = self.disable_fbs(&[fb], drv, ctx);
        self.sweep(ctx);
        r
    }
}

/// Releases a framebuffer reference; if it was the last one, the buffer
/// references go with [`FbBacking::release`] in thread context.
pub fn release_fb<B: FbBacking>(fb: FbRef<B>, ctx: &Sleepable) {
    if let Some(inner) = Arc::into_inner(fb) {
        for o in inner.objs.into_iter().flatten() {
            o.release(ctx);
        }
    }
}

/// Releases the states an update replaced.
pub fn release_old<B: FbBacking>(old: OldState<B>, ctx: &Sleepable) {
    for p in old.planes {
        if let Some(fb) = p.fb {
            release_fb(fb, ctx);
        }
    }
}

/// The properties of an object kind, or `None` if it has none.
pub(super) fn props_of(kind: ObjKind) -> Option<&'static [Prop]> {
    match kind {
        ObjKind::Connector(_) => Some(CONNECTOR_PROPS),
        ObjKind::Crtc(_) => Some(CRTC_PROPS),
        ObjKind::Plane(_) => Some(PLANE_PROPS),
        _ => None,
    }
}

fn umode_eq(a: &uapi::drm_mode_modeinfo, b: &uapi::drm_mode_modeinfo) -> bool {
    (a.clock, a.hdisplay, a.hsync_start, a.hsync_end, a.htotal, a.hskew)
        == (b.clock, b.hdisplay, b.hsync_start, b.hsync_end, b.htotal, b.hskew)
        && (a.vdisplay, a.vsync_start, a.vsync_end, a.vtotal, a.vscan, a.flags)
            == (b.vdisplay, b.vsync_start, b.vsync_end, b.vtotal, b.vscan, b.flags)
}
