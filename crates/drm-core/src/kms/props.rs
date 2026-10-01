// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Mode-object properties: their definitions as `GETPROPERTY` reports them
//! and the rules for values (`drm_property.c`).
//!
//! Every property of this core is one of the standard ones Linux defines,
//! with the same name, type, flags and values, so that clients that look
//! properties up by name (libdrm's `modetest`, Weston, Mutter, the Xorg
//! modesetting driver) find them. The values themselves live in the
//! object state ([`super::state`]); this module only describes them.

use drm_uapi as uapi;

/// The type of a property and the values it admits.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PropKind {
    /// An unsigned range, inclusive.
    Range(u64, u64),
    /// A signed range, inclusive.
    SignedRange(i64, i64),
    /// One of the listed values.
    Enum(&'static [(u64, &'static str)]),
    /// A set of the listed bits (values are bit numbers).
    Bitmask(&'static [(u64, &'static str)]),
    /// The ID of a property blob, or 0.
    Blob,
    /// The ID of a mode object of the given `DRM_MODE_OBJECT_*` type, or 0.
    Object(u32),
}

/// A property definition.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PropDef {
    /// The name, at most 31 bytes.
    pub name: &'static str,
    /// Type and values.
    pub kind: PropKind,
    /// Userland may not change it (`DRM_MODE_PROP_IMMUTABLE`).
    pub immutable: bool,
    /// Only atomic clients see it (`DRM_MODE_PROP_ATOMIC`).
    pub atomic: bool,
}

impl PropDef {
    /// The `flags` of `GETPROPERTY`: the type bits and the modifiers.
    pub fn flags(&self) -> u32 {
        let t = match self.kind {
            PropKind::Range(..) => uapi::DRM_MODE_PROP_RANGE,
            PropKind::SignedRange(..) => 2 << 6,
            PropKind::Enum(_) => uapi::DRM_MODE_PROP_ENUM,
            PropKind::Bitmask(_) => uapi::DRM_MODE_PROP_BITMASK,
            PropKind::Blob => uapi::DRM_MODE_PROP_BLOB,
            PropKind::Object(_) => 1 << 6,
        };
        let mut f = t;
        if self.immutable {
            f |= uapi::DRM_MODE_PROP_IMMUTABLE;
        }
        if self.atomic {
            f |= uapi::DRM_MODE_PROP_ATOMIC;
        }
        f
    }

    /// The `values` array of `GETPROPERTY`: bounds for ranges, the values
    /// of enums, the bit numbers of bitmasks, the object type of object
    /// properties, nothing for blobs.
    pub fn values(&self) -> impl Iterator<Item = u64> + '_ {
        let (two, list, one): Values = match self.kind {
            PropKind::Range(a, b) => (Some([a, b]), &[], None),
            PropKind::SignedRange(a, b) => (Some([a as u64, b as u64]), &[], None),
            PropKind::Enum(l) | PropKind::Bitmask(l) => (None, l, None),
            PropKind::Blob => (None, &[], None),
            PropKind::Object(t) => (None, &[], Some(u64::from(t))),
        };
        two.into_iter().flatten().chain(list.iter().map(|e| e.0)).chain(one)
    }

    /// The enum entries of `GETPROPERTY` (enums and bitmasks only).
    pub fn enums(&self) -> &'static [(u64, &'static str)] {
        match self.kind {
            PropKind::Enum(l) | PropKind::Bitmask(l) => l,
            _ => &[],
        }
    }

    /// Checks a value for this property as `drm_property_change_valid_get`
    /// does, apart from the lookup of blobs and objects, which the caller
    /// does: in range, a listed enum value, or only listed bits.
    pub fn value_ok(&self, v: u64) -> bool {
        match self.kind {
            PropKind::Range(a, b) => (a..=b).contains(&v),
            PropKind::SignedRange(a, b) => (a..=b).contains(&(v as i64)),
            PropKind::Enum(l) => l.iter().any(|e| e.0 == v),
            PropKind::Bitmask(l) => {
                let all = l.iter().fold(0u64, |m, e| m | 1u64.checked_shl(e.0 as u32).unwrap_or(0));
                v & !all == 0
            }
            PropKind::Blob | PropKind::Object(_) => true,
        }
    }
}

/// The parts of a `values` array: two bounds, enum values, an object type.
type Values = (Option<[u64; 2]>, &'static [(u64, &'static str)], Option<u64>);

/// DPMS states (`DRM_MODE_DPMS_*`).
pub const DPMS_ENUM: &[(u64, &str)] = &[
    (uapi::DRM_MODE_DPMS_ON as u64, "On"),
    (uapi::DRM_MODE_DPMS_STANDBY as u64, "Standby"),
    (uapi::DRM_MODE_DPMS_SUSPEND as u64, "Suspend"),
    (uapi::DRM_MODE_DPMS_OFF as u64, "Off"),
];

/// Link states (`DRM_MODE_LINK_STATUS_*`).
pub const LINK_STATUS_ENUM: &[(u64, &str)] =
    &[(uapi::DRM_MODE_LINK_STATUS_GOOD as u64, "Good"), (uapi::DRM_MODE_LINK_STATUS_BAD as u64, "Bad")];

/// Plane types (`DRM_PLANE_TYPE_*`).
pub const PLANE_TYPE_ENUM: &[(u64, &str)] = &[(0, "Overlay"), (1, "Primary"), (2, "Cursor")];

const U32_MAX: u64 = u32::MAX as u64;
const I32_MAX: u64 = i32::MAX as u64;

const fn def(name: &'static str, kind: PropKind, immutable: bool, atomic: bool) -> PropDef {
    PropDef { name, kind, immutable, atomic }
}

/// The standard properties, in the order their IDs are assigned.
pub const STANDARD: &[PropDef] = &[
    def("EDID", PropKind::Blob, true, false),
    def("DPMS", PropKind::Enum(DPMS_ENUM), false, false),
    def("link-status", PropKind::Enum(LINK_STATUS_ENUM), false, false),
    def("non-desktop", PropKind::Range(0, 1), true, false),
    def("CRTC_ID", PropKind::Object(uapi::DRM_MODE_OBJECT_CRTC), false, true),
    def("ACTIVE", PropKind::Range(0, 1), false, true),
    def("MODE_ID", PropKind::Blob, false, true),
    def("OUT_FENCE_PTR", PropKind::Range(0, u64::MAX), false, true),
    def("FB_ID", PropKind::Object(uapi::DRM_MODE_OBJECT_FB), false, true),
    def("IN_FENCE_FD", PropKind::SignedRange(-1, i32::MAX as i64), false, true),
    def("SRC_X", PropKind::Range(0, U32_MAX), false, true),
    def("SRC_Y", PropKind::Range(0, U32_MAX), false, true),
    def("SRC_W", PropKind::Range(0, U32_MAX), false, true),
    def("SRC_H", PropKind::Range(0, U32_MAX), false, true),
    def("CRTC_X", PropKind::SignedRange(i32::MIN as i64, i32::MAX as i64), false, true),
    def("CRTC_Y", PropKind::SignedRange(i32::MIN as i64, i32::MAX as i64), false, true),
    def("CRTC_W", PropKind::Range(0, I32_MAX), false, true),
    def("CRTC_H", PropKind::Range(0, I32_MAX), false, true),
    def("type", PropKind::Enum(PLANE_TYPE_ENUM), true, false),
    def("IN_FORMATS", PropKind::Blob, true, false),
];

/// The standard properties by name; the discriminant is the index into
/// [`STANDARD`]. The core assigns their object IDs at initialisation and
/// keeps them in a table.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
#[allow(missing_docs, reason = "each variant is the property of the same name")]
pub enum Prop {
    Edid,
    Dpms,
    LinkStatus,
    NonDesktop,
    CrtcId,
    Active,
    ModeId,
    OutFencePtr,
    FbId,
    InFenceFd,
    SrcX,
    SrcY,
    SrcW,
    SrcH,
    CrtcX,
    CrtcY,
    CrtcW,
    CrtcH,
    Type,
    InFormats,
}

impl Prop {
    /// Every property, in [`STANDARD`] order.
    pub const ALL: [Prop; 20] = [
        Prop::Edid,
        Prop::Dpms,
        Prop::LinkStatus,
        Prop::NonDesktop,
        Prop::CrtcId,
        Prop::Active,
        Prop::ModeId,
        Prop::OutFencePtr,
        Prop::FbId,
        Prop::InFenceFd,
        Prop::SrcX,
        Prop::SrcY,
        Prop::SrcW,
        Prop::SrcH,
        Prop::CrtcX,
        Prop::CrtcY,
        Prop::CrtcW,
        Prop::CrtcH,
        Prop::Type,
        Prop::InFormats,
    ];

    /// The definition.
    pub fn def(self) -> &'static PropDef {
        // The enum and STANDARD have the same order; the test checks it.
        match STANDARD.get(self as usize) {
            Some(d) => d,
            None => &STANDARD[0],
        }
    }

    /// Fence properties exist so that their IDs can be recognised and
    /// refused with `EINVAL`, but no object carries them until fences exist
    /// (milestone R2).
    pub fn is_fence(self) -> bool {
        matches!(self, Prop::OutFencePtr | Prop::InFenceFd)
    }
}

/// The properties of a connector, in the order `OBJ_GETPROPERTIES` lists
/// them (Linux's `drm_connector_init` order).
pub const CONNECTOR_PROPS: &[Prop] = &[Prop::Edid, Prop::Dpms, Prop::LinkStatus, Prop::NonDesktop, Prop::CrtcId];
/// The properties of a CRTC.
pub const CRTC_PROPS: &[Prop] = &[Prop::Active, Prop::ModeId];
/// The properties of a plane (Linux's `drm_universal_plane_init` order).
pub const PLANE_PROPS: &[Prop] = &[
    Prop::FbId,
    Prop::CrtcId,
    Prop::CrtcX,
    Prop::CrtcY,
    Prop::CrtcW,
    Prop::CrtcH,
    Prop::SrcX,
    Prop::SrcY,
    Prop::SrcW,
    Prop::SrcH,
    Prop::Type,
    Prop::InFormats,
];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn table_order_and_flags() {
        for (i, p) in Prop::ALL.iter().enumerate() {
            assert_eq!(*p as usize, i);
            assert!(p.def().name.len() < uapi::DRM_PROP_NAME_LEN as usize);
        }
        assert_eq!(Prop::CrtcId.def().name, "CRTC_ID");
        assert_eq!(Prop::InFormats.def().name, "IN_FORMATS");
        assert_eq!(Prop::Edid.def().flags(), uapi::DRM_MODE_PROP_BLOB | uapi::DRM_MODE_PROP_IMMUTABLE);
        assert_eq!(Prop::CrtcX.def().flags(), (2 << 6) | uapi::DRM_MODE_PROP_ATOMIC);
        assert_eq!(Prop::FbId.def().flags(), (1 << 6) | uapi::DRM_MODE_PROP_ATOMIC);
        let v: [u64; 1] = [uapi::DRM_MODE_OBJECT_FB as u64];
        assert!(Prop::FbId.def().values().eq(v));
        assert!(Prop::CrtcX.def().values().eq([i32::MIN as i64 as u64, i32::MAX as u64]));
        assert!(Prop::Dpms.def().values().eq([0, 1, 2, 3]));
        assert_eq!(Prop::ModeId.def().values().count(), 0);
    }

    #[test]
    fn values() {
        assert!(Prop::Active.def().value_ok(1));
        assert!(!Prop::Active.def().value_ok(2));
        assert!(Prop::CrtcX.def().value_ok(-5i64 as u64));
        assert!(!Prop::CrtcX.def().value_ok(1 << 40));
        assert!(!Prop::CrtcW.def().value_ok(1 << 31));
        assert!(Prop::Dpms.def().value_ok(3));
        assert!(!Prop::Dpms.def().value_ok(4));
        const BITS: &[(u64, &str)] = &[(0, "a"), (2, "c")];
        let bm = def("bm", PropKind::Bitmask(BITS), false, false);
        assert!(bm.value_ok(0b101));
        assert!(!bm.value_ok(0b010));
        assert!(Prop::OutFencePtr.is_fence() && Prop::InFenceFd.is_fence() && !Prop::FbId.is_fence());
    }
}
