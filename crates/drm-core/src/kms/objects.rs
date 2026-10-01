// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Mode objects: the ID space, the static description of a device's
//! display pipeline, property blobs and framebuffers.
//!
//! Every mode object (CRTC, connector, encoder, plane, property,
//! framebuffer, blob) has an ID from one space, as in Linux's
//! `mode_config.object_idr`: the lowest free number from 1. The static
//! objects get theirs when the device attaches, in the order properties,
//! planes, CRTCs, encoders, connectors, the `IN_FORMATS` blobs; blobs and
//! framebuffers come and go at run time.
//!
//! Blobs and framebuffers are reference counted (`alloc::sync::Arc`): the
//! registry holds one reference, the object state of planes and CRTCs
//! (and states being checked) hold others. An object that userland
//! removed (`RMFB`, `DESTROYPROPBLOB`, the close of its file) leaves the
//! registry, and its ID becomes free, once no state refers to it any
//! more; until then it can still be looked up, as in Linux, where the ID
//! is unregistered by the last reference.

use alloc::collections::BTreeMap;
use alloc::sync::Arc;
use alloc::vec::Vec;

use ddi::Errno;
use drm_uapi as uapi;

use super::modes::{DisplayMode, ModeLimits};

/// The kinds of mode objects and their `DRM_MODE_OBJECT_*` codes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ObjKind {
    /// A CRTC, by index.
    Crtc(usize),
    /// A connector, by index.
    Connector(usize),
    /// An encoder, by index.
    Encoder(usize),
    /// A plane, by index.
    Plane(usize),
    /// A property, by [`super::props::Prop`] index.
    Property(usize),
    /// A framebuffer.
    Fb,
    /// A property blob.
    Blob,
}

impl ObjKind {
    /// The `DRM_MODE_OBJECT_*` code.
    pub fn code(self) -> u32 {
        match self {
            ObjKind::Crtc(_) => uapi::DRM_MODE_OBJECT_CRTC,
            ObjKind::Connector(_) => uapi::DRM_MODE_OBJECT_CONNECTOR,
            ObjKind::Encoder(_) => uapi::DRM_MODE_OBJECT_ENCODER,
            ObjKind::Plane(_) => uapi::DRM_MODE_OBJECT_PLANE,
            ObjKind::Property(_) => uapi::DRM_MODE_OBJECT_PROPERTY,
            ObjKind::Fb => uapi::DRM_MODE_OBJECT_FB,
            ObjKind::Blob => uapi::DRM_MODE_OBJECT_BLOB,
        }
    }
}

/// The ID space of one device.
#[derive(Default)]
pub struct Ids {
    map: BTreeMap<u32, ObjKind>,
    /// No ID below this is free.
    hint: u32,
}

impl Ids {
    /// Creates an empty space.
    pub const fn new() -> Ids {
        Ids { map: BTreeMap::new(), hint: 1 }
    }

    /// Allocates the lowest free ID (from 1) for an object. The search
    /// starts at the lowest ID that may be free, so allocating in a row is
    /// cheap. The caller must be in a context that may block: the map
    /// allocates with `KM_SLEEP`.
    pub fn alloc(&mut self, kind: ObjKind, _ctx: &ddi::Sleepable) -> Result<u32, Errno> {
        let mut want = self.hint.max(1);
        for (&k, _) in self.map.range(want..) {
            if k != want {
                break;
            }
            want = want.checked_add(1).ok_or(Errno::ENOMEM)?;
        }
        if want > i32::MAX as u32 {
            return Err(Errno::ENOMEM);
        }
        self.map.insert(want, kind);
        self.hint = want + 1;
        Ok(want)
    }

    /// Frees an ID.
    pub fn free(&mut self, id: u32) {
        if self.map.remove(&id).is_some() {
            self.hint = self.hint.min(id);
        }
    }

    /// Looks an ID up.
    pub fn get(&self, id: u32) -> Option<ObjKind> {
        self.map.get(&id).copied()
    }

    /// The number of IDs in use.
    pub fn len(&self) -> usize {
        self.map.len()
    }

    /// Reports whether no ID is in use.
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }
}

// --- the static description --------------------------------------------------

/// The type of a plane (`DRM_PLANE_TYPE_*`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PlaneType {
    /// An overlay plane.
    Overlay = 0,
    /// The primary plane of a CRTC.
    Primary = 1,
    /// The cursor plane of a CRTC.
    Cursor = 2,
}

/// A plane of the device.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PlaneDesc {
    /// Its type.
    pub typ: PlaneType,
    /// The CRTCs it can be shown on, as a mask of CRTC indexes.
    pub possible_crtcs: u32,
    /// The formats it scans out, at most 64.
    pub formats: Vec<u32>,
    /// The modifiers it scans out; empty means linear only.
    pub modifiers: Vec<u64>,
}

/// A CRTC of the device.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CrtcDesc {
    /// The index of its primary plane.
    pub primary: usize,
    /// The index of its cursor plane, if it has one.
    pub cursor: Option<usize>,
    /// The size of its legacy gamma table (`GETGAMMA`/`SETGAMMA`); 0 for
    /// none.
    pub gamma_size: u32,
}

/// An encoder of the device.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EncoderDesc {
    /// `DRM_MODE_ENCODER_*`.
    pub typ: u32,
    /// The CRTCs it can be driven by, as a mask of CRTC indexes.
    pub possible_crtcs: u32,
    /// The encoders it can be cloned with, as a mask of encoder indexes.
    pub possible_clones: u32,
}

/// A connector of the device.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConnectorDesc {
    /// `DRM_MODE_CONNECTOR_*`.
    pub typ: u32,
    /// The encoders it can use, as a mask of encoder indexes.
    pub encoders: u32,
    /// The limits applied to its modes.
    pub limits: ModeLimits,
}

/// The display pipeline of a device, which the driver describes once when
/// the device attaches.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Layout {
    /// Smallest framebuffer width.
    pub min_width: u32,
    /// Largest framebuffer width.
    pub max_width: u32,
    /// Smallest framebuffer height.
    pub min_height: u32,
    /// Largest framebuffer height.
    pub max_height: u32,
    /// The planes, primary and cursor planes included.
    pub planes: Vec<PlaneDesc>,
    /// The CRTCs, at most 32.
    pub crtcs: Vec<CrtcDesc>,
    /// The encoders, at most 32.
    pub encoders: Vec<EncoderDesc>,
    /// The connectors.
    pub connectors: Vec<ConnectorDesc>,
}

impl Layout {
    /// Checks the description for consistency: at most 32 CRTCs and
    /// encoders, every CRTC with a primary plane of type primary (and a
    /// cursor plane of type cursor) that can be shown on it, masks that name
    /// existing objects, at most 64 formats per plane, known formats, and
    /// sane size limits.
    pub fn validate(&self) -> Result<(), &'static str> {
        let nc = self.crtcs.len();
        if nc == 0 || nc > 32 || self.encoders.len() > 32 {
            return Err("between 1 and 32 CRTCs and at most 32 encoders");
        }
        if self.min_width == 0
            || self.min_height == 0
            || self.max_width < self.min_width
            || self.max_height < self.min_height
        {
            return Err("bad framebuffer size limits");
        }
        if self.max_width > 16384 || self.max_height > 16384 {
            return Err("framebuffers larger than 16384 are not supported");
        }
        let crtc_mask = if nc == 32 { u32::MAX } else { (1u32 << nc) - 1 };
        let enc_mask = if self.encoders.len() == 32 { u32::MAX } else { (1u32 << self.encoders.len()) - 1 };
        for p in &self.planes {
            if p.possible_crtcs == 0 || p.possible_crtcs & !crtc_mask != 0 {
                return Err("plane with bad possible_crtcs");
            }
            if p.formats.is_empty() || p.formats.len() > 64 {
                return Err("a plane needs between 1 and 64 formats");
            }
            if p.formats.iter().any(|f| super::formats::lookup(*f).is_none()) {
                return Err("plane with an unknown format");
            }
        }
        for (i, c) in self.crtcs.iter().enumerate() {
            let ok = |idx: usize, t: PlaneType| {
                self.planes.get(idx).is_some_and(|p| p.typ == t && p.possible_crtcs & (1 << i) != 0)
            };
            if !ok(c.primary, PlaneType::Primary) || c.cursor.is_some_and(|k| !ok(k, PlaneType::Cursor)) {
                return Err("CRTC with a bad primary or cursor plane");
            }
            if c.gamma_size > 4096 {
                return Err("gamma table larger than 4096 entries");
            }
        }
        for e in &self.encoders {
            if e.possible_crtcs == 0 || e.possible_crtcs & !crtc_mask != 0 || e.possible_clones & !enc_mask != 0 {
                return Err("encoder with bad masks");
            }
        }
        for c in &self.connectors {
            if c.encoders == 0 || c.encoders & !enc_mask != 0 {
                return Err("connector with bad encoder mask");
            }
        }
        Ok(())
    }

    /// The modifiers of plane `i`, linear if the driver listed none.
    pub fn plane_modifiers(&self, i: usize) -> &[u64] {
        match self.planes.get(i) {
            Some(p) if !p.modifiers.is_empty() => &p.modifiers,
            _ => &[super::formats::MOD_LINEAR],
        }
    }
}

// --- blobs -------------------------------------------------------------------

/// A property blob.
#[derive(Debug)]
pub struct Blob {
    /// Its ID.
    pub id: u32,
    /// Its contents.
    pub data: Vec<u8>,
}

/// A counted reference to a blob.
pub type BlobRef = Arc<Blob>;

impl Blob {
    /// Decodes a `MODE_ID` blob: exactly one `drm_mode_modeinfo`, converted
    /// and validated as `drm_mode_convert_umode` does.
    pub fn mode(&self) -> Result<DisplayMode, Errno> {
        if self.data.len() != core::mem::size_of::<uapi::drm_mode_modeinfo>() {
            return Err(Errno::EINVAL);
        }
        // SAFETY: the length matches, the structure consists of integers
        // only (every bit pattern is valid), and read_unaligned copes with
        // the Vec's alignment.
        let u = unsafe { core::ptr::read_unaligned(self.data.as_ptr() as *const uapi::drm_mode_modeinfo) };
        DisplayMode::from_umode(&u)
    }
}

/// Encodes a mode as the contents of a `MODE_ID` blob.
pub fn mode_blob_data(m: &DisplayMode) -> Vec<u8> {
    let u = m.to_umode(true);
    let size = core::mem::size_of::<uapi::drm_mode_modeinfo>();
    let mut v = Vec::with_capacity(size);
    // SAFETY: drm_mode_modeinfo has no padding (4 + 10 * 2 + 3 * 4 + 32 =
    // 68 bytes, asserted by the bindings), so every byte is initialised.
    let bytes = unsafe { core::slice::from_raw_parts(&u as *const _ as *const u8, size) };
    v.extend_from_slice(bytes);
    v
}

const _: () = assert!(core::mem::size_of::<uapi::drm_mode_modeinfo>() == 68);

// --- framebuffers ------------------------------------------------------------

/// A framebuffer: an image of a given size and format in one to four
/// buffer objects. `B` is the driver's buffer reference (`GemRef` in the
/// kernel).
#[derive(Debug)]
pub struct Framebuffer<B> {
    /// Its ID.
    pub id: u32,
    /// Width in pixels.
    pub width: u32,
    /// Height in pixels.
    pub height: u32,
    /// The fourcc format.
    pub format: u32,
    /// The modifier of every plane.
    pub modifier: u64,
    /// The `DRM_MODE_FB_*` flags it was created with (`GETFB2` reports
    /// them back).
    pub flags: u32,
    /// The pitch of each plane in bytes.
    pub pitches: [u32; 4],
    /// The offset of each plane in its object.
    pub offsets: [u32; 4],
    /// The cursor hotspot of a legacy cursor framebuffer (`CURSOR2`).
    pub hot_x: i32,
    /// The cursor hotspot of a legacy cursor framebuffer (`CURSOR2`).
    pub hot_y: i32,
    /// The buffer object of each plane.
    pub objs: [Option<B>; 4],
    /// For an internal legacy cursor framebuffer, the file whose `CURSOR`
    /// call made it: planes that show it go off when that file closes.
    pub cursor_of: Option<u32>,
}

/// Who a new framebuffer belongs to ([`super::core::KmsCore::add_fb`]).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FbOwner {
    /// `ADDFB`/`ADDFB2` of a file: the file owns it (`RMFB`, `GETRESOURCES`).
    File(u32),
    /// A legacy `CURSOR` call of a file: owned by no file, as in Linux,
    /// but shown only until that file closes.
    Cursor(u32),
}

/// A counted reference to a framebuffer.
pub type FbRef<B> = Arc<Framebuffer<B>>;

impl<B> Framebuffer<B> {
    /// The format description.
    pub fn info(&self) -> Option<&'static super::formats::FormatInfo> {
        super::formats::lookup(self.format)
    }
}

/// An `ADDFB2` request after decoding (handles still unresolved).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FbRequest {
    /// Width.
    pub width: u32,
    /// Height.
    pub height: u32,
    /// The fourcc format.
    pub format: u32,
    /// `DRM_MODE_FB_*` flags.
    pub flags: u32,
    /// GEM handles.
    pub handles: [u32; 4],
    /// Pitches.
    pub pitches: [u32; 4],
    /// Offsets.
    pub offsets: [u32; 4],
    /// Modifiers.
    pub modifiers: [u64; 4],
}

/// Checks an `ADDFB2` request as Linux's `framebuffer_check` does, before
/// the handles are looked up: known flags (`EINVAL`), a known format
/// (`EINVAL`), a size within the device's limits (`EINVAL`), for every
/// plane of the format a handle, a pitch that holds a line and a sane
/// offset (`EINVAL`, `ERANGE` for an image past 4 GiB), equal modifiers on all planes with
/// `DRM_MODE_FB_MODIFIERS` and zero modifiers without it (`EINVAL`), and
/// zeros in every unused plane slot (`EINVAL`). Returns the number of
/// planes.
pub fn check_fb_request(r: &FbRequest, layout: &Layout) -> Result<usize, Errno> {
    if r.flags & !(uapi::DRM_MODE_FB_INTERLACED | uapi::DRM_MODE_FB_MODIFIERS) != 0 {
        return Err(Errno::EINVAL);
    }
    let info = super::formats::lookup(r.format).ok_or(Errno::EINVAL)?;
    if r.width < layout.min_width
        || r.width > layout.max_width
        || r.height < layout.min_height
        || r.height > layout.max_height
    {
        return Err(Errno::EINVAL);
    }
    let with_mods = r.flags & uapi::DRM_MODE_FB_MODIFIERS != 0;
    let n = info.planes();
    for i in 0..4 {
        let (h, p, o, m) = (r.handles[i], r.pitches[i], r.offsets[i], r.modifiers[i]);
        if i >= n {
            if m != 0 {
                return Err(Errno::EINVAL);
            }
            // Userland from before DRM_MODE_FB_MODIFIERS did not clear the
            // unused slots, so Linux looks at them only with the flag.
            if with_mods && (h != 0 || p != 0 || o != 0) {
                return Err(Errno::EINVAL);
            }
            continue;
        }
        if h == 0 {
            return Err(Errno::EINVAL);
        }
        let min_pitch = u64::from(info.plane_width(r.width, i)) * u64::from(info.cpp[i]);
        if u64::from(p) < min_pitch {
            return Err(Errno::EINVAL);
        }
        let hgt = u64::from(info.plane_height(r.height, i));
        if u64::from(o) + u64::from(p) * hgt > u64::from(u32::MAX) {
            return Err(super::modes::ERANGE);
        }
        if (with_mods && m != r.modifiers[0]) || (!with_mods && m != 0) {
            return Err(Errno::EINVAL);
        }
    }
    Ok(n)
}

/// The smallest object size plane `i` of the request needs:
/// `offset + pitch * (height - 1) + width * cpp` (Linux's
/// `drm_gem_fb_init_with_funcs`).
pub fn plane_min_size(r: &FbRequest, i: usize) -> Option<u64> {
    let info = super::formats::lookup(r.format)?;
    let w = u64::from(info.plane_width(r.width, i));
    let h = u64::from(info.plane_height(r.height, i));
    Some(u64::from(r.offsets[i]) + u64::from(r.pitches[i]) * (h - 1) + w * u64::from(info.cpp[i]))
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use crate::kms::formats::*;

    fn sleep() -> ddi::Sleepable {
        // SAFETY: host tests run in an ordinary thread.
        unsafe { ddi::Sleepable::assume() }
    }

    pub(crate) fn layout() -> Layout {
        let limits = ModeLimits { max_width: 2048, max_height: 2048, max_clock: 0, interlace: false, dblscan: false };
        Layout {
            min_width: 1,
            max_width: 2048,
            min_height: 1,
            max_height: 2048,
            planes: std::vec![
                PlaneDesc {
                    typ: PlaneType::Primary,
                    possible_crtcs: 1,
                    formats: std::vec![XRGB8888, ARGB8888, RGB565],
                    modifiers: std::vec![MOD_LINEAR]
                },
                PlaneDesc {
                    typ: PlaneType::Cursor,
                    possible_crtcs: 1,
                    formats: std::vec![ARGB8888],
                    modifiers: std::vec![]
                },
            ],
            crtcs: std::vec![CrtcDesc { primary: 0, cursor: Some(1), gamma_size: 256 }],
            encoders: std::vec![EncoderDesc {
                typ: uapi::DRM_MODE_ENCODER_VIRTUAL,
                possible_crtcs: 1,
                possible_clones: 0
            }],
            connectors: std::vec![ConnectorDesc { typ: uapi::DRM_MODE_CONNECTOR_VIRTUAL, encoders: 1, limits }],
        }
    }

    #[test]
    fn ids_lowest_free() {
        let ctx = sleep();
        let mut ids = Ids::new();
        assert_eq!(ids.alloc(ObjKind::Fb, &ctx), Ok(1));
        assert_eq!(ids.alloc(ObjKind::Blob, &ctx), Ok(2));
        assert_eq!(ids.alloc(ObjKind::Crtc(0), &ctx), Ok(3));
        ids.free(2);
        assert_eq!(ids.alloc(ObjKind::Fb, &ctx), Ok(2));
        assert_eq!(ids.get(3), Some(ObjKind::Crtc(0)));
        assert_eq!(ids.get(4), None);
        assert_eq!(ids.len(), 3);
    }

    #[test]
    fn layout_validation() {
        let l = layout();
        assert_eq!(l.validate(), Ok(()));
        let mut bad = l.clone();
        bad.crtcs[0].primary = 1;
        assert!(bad.validate().is_err());
        let mut bad = l.clone();
        bad.planes[0].formats.push(fourcc(b'Y', b'U', b'Y', b'V'));
        assert!(bad.validate().is_err());
        let mut bad = l.clone();
        bad.encoders[0].possible_crtcs = 2;
        assert!(bad.validate().is_err());
        assert_eq!(l.plane_modifiers(1), &[MOD_LINEAR]);
    }

    #[test]
    fn fb_request_checks() {
        let l = layout();
        let ok = FbRequest {
            width: 64,
            height: 32,
            format: XRGB8888,
            flags: 0,
            handles: [1, 0, 0, 0],
            pitches: [256, 0, 0, 0],
            offsets: [0; 4],
            modifiers: [0; 4],
        };
        assert_eq!(check_fb_request(&ok, &l), Ok(1));
        assert_eq!(plane_min_size(&ok, 0), Some(256 * 31 + 256));
        let with = |f: &dyn Fn(&mut FbRequest)| {
            let mut r = ok;
            f(&mut r);
            check_fb_request(&r, &l)
        };
        assert_eq!(with(&|r| r.format = 0), Err(Errno::EINVAL));
        assert_eq!(with(&|r| r.flags = 4), Err(Errno::EINVAL));
        assert_eq!(with(&|r| r.width = 0), Err(Errno::EINVAL));
        assert_eq!(with(&|r| r.height = 4096), Err(Errno::EINVAL));
        assert_eq!(with(&|r| r.handles[0] = 0), Err(Errno::EINVAL));
        assert_eq!(with(&|r| r.pitches[0] = 255), Err(Errno::EINVAL));
        assert_eq!(with(&|r| r.handles[1] = 1), Ok(1), "old userland leaves garbage there");
        assert_eq!(
            with(&|r| {
                r.flags = uapi::DRM_MODE_FB_MODIFIERS;
                r.handles[1] = 1;
            }),
            Err(Errno::EINVAL)
        );
        assert_eq!(with(&|r| r.modifiers[1] = 1), Err(Errno::EINVAL));
        assert_eq!(with(&|r| r.modifiers[0] = 1), Err(Errno::EINVAL), "modifier without the flag");
        assert_eq!(
            with(&|r| {
                r.flags = uapi::DRM_MODE_FB_MODIFIERS;
                r.modifiers[0] = MOD_I915_X_TILED;
            }),
            Ok(1),
            "any modifier passes here; the planes decide"
        );
        assert_eq!(with(&|r| r.offsets[0] = u32::MAX), Err(crate::kms::modes::ERANGE));
        let nv12 =
            FbRequest { format: NV12, handles: [1, 1, 0, 0], pitches: [64, 64, 0, 0], offsets: [0, 2048, 0, 0], ..ok };
        assert_eq!(check_fb_request(&nv12, &l), Ok(2));
        assert_eq!(plane_min_size(&nv12, 1), Some(2048 + 64 * 15 + 64));
    }

    #[test]
    fn mode_blob_round_trip() {
        let m = crate::kms::modes::find_dmt(1024, 768, 60).unwrap();
        let b = Blob { id: 1, data: mode_blob_data(&m) };
        assert_eq!(b.mode(), Ok(m));
        let short = Blob { id: 2, data: std::vec![0; 10] };
        assert_eq!(short.mode(), Err(Errno::EINVAL));
    }
}
