// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The minor number layout of DRM devices.
//!
//! Device `N` has two minor nodes: the primary node `cardN` with node
//! minor `N`, and the render node `renderD(128+N)` with node minor
//! `128 + N`, the numbering Linux uses and Mesa's loader expects. Node
//! minors occupy the low nine bits, as in the gfx-drm driver
//! (`DRM_MINOR_NBITS`).
//!
//! `N`, the device number, is not the driver's instance number: every
//! device of every driver takes the lowest free number from the one table
//! of the system ([`crate::slots`], in the core module `misc/drm_core`) when it
//! registers and gives it back when it unregisters, so that no two devices
//! are ever both named `card0`. The core records which driver instance
//! holds which number ([`bind`]), so that [`instance_of_minor`] can map a
//! minor number back to the instance for the driver's `getinfo(9E)`
//! (`drm_minor_instance` of the facade).
//!
//! Every open is a clone open: the driver gives the new file its own minor
//! number, with a clone id from 1 to 511 in bits 9 to 17 above the node
//! minor, so that each file gets its own `close(9E)` and its own state. The
//! illumos patch to libdrm strips exactly these bits (`d &= ~0x3fe00` in
//! `drmGetDeviceNameFromFd`) to find the `/dev/dri` node of an open file,
//! which is why the clone id is limited to nine bits and a device can have
//! at most [`MAX_FILES`] open files at a time. Clone id 0 is never used, so
//! a file's device number never equals that of the node itself.

use core::sync::atomic::{AtomicI32, Ordering};

use ddi::{Instance, Minor};

/// The number of low bits that hold the node minor.
pub const NODE_BITS: u32 = 9;
/// The number of bits above the node minor that hold the clone id.
pub const CLONE_BITS: u32 = 9;
/// The mask of the clone id bits in a minor number, `0x3fe00`.
pub const CLONE_MASK: u32 = ((1 << CLONE_BITS) - 1) << NODE_BITS;
/// The largest clone id, and so the number of files a device can have open.
pub const MAX_FILES: u32 = (1 << CLONE_BITS) - 1;
/// The first node minor of render nodes.
pub const RENDER_BASE: u32 = 128;
/// The number of DRM devices on the system, of all drivers together
/// (device numbers 0 to 63).
pub const MAX_DEVICES: u32 = 64;

/// The kind of a minor node.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NodeKind {
    /// The primary node `cardN`: authentication, master, and (from R3) KMS.
    Primary,
    /// The render node `renderD(128+N)`: rendering only, no authentication.
    Render,
}

/// A clone id, between 1 and [`MAX_FILES`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct CloneId(u32);

impl CloneId {
    /// Wraps a clone id, or returns `None` if it is out of range.
    pub const fn new(id: u32) -> Option<CloneId> {
        if id >= 1 && id <= MAX_FILES { Some(CloneId(id)) } else { None }
    }
    /// Returns the numeric id.
    pub const fn get(self) -> u32 {
        self.0
    }
    /// Returns the id as an index into a per-device table.
    pub const fn index(self) -> usize {
        self.0 as usize
    }
}

/// A minor number taken apart.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Decoded {
    /// Which node the minor number belongs to.
    pub kind: NodeKind,
    /// The device number (not the driver instance number; see the module
    /// documentation).
    pub index: u32,
    /// The clone id of an open file, or `None` for the node itself.
    pub clone: Option<CloneId>,
}

/// Returns the node minor of a device's primary or render node, or `None`
/// if `index` is not below [`MAX_DEVICES`].
pub const fn node_minor(kind: NodeKind, index: u32) -> Option<u32> {
    if index >= MAX_DEVICES {
        return None;
    }
    Some(match kind {
        NodeKind::Primary => index,
        NodeKind::Render => RENDER_BASE + index,
    })
}

/// Returns the minor number of an open file.
pub const fn file_minor(kind: NodeKind, index: u32, clone: CloneId) -> Option<u32> {
    match node_minor(kind, index) {
        Some(m) => Some(m | (clone.0 << NODE_BITS)),
        None => None,
    }
}

/// Takes a minor number apart. Returns `None` for numbers that no DRM
/// device uses: bits above the clone id, or a node minor outside the
/// primary (0 to 63) and render (128 to 191) ranges.
pub const fn decode(minor: u32) -> Option<Decoded> {
    if minor >> (NODE_BITS + CLONE_BITS) != 0 {
        return None;
    }
    let node = minor & ((1 << NODE_BITS) - 1);
    let clone_raw = (minor & CLONE_MASK) >> NODE_BITS;
    let clone = if clone_raw == 0 { None } else { Some(CloneId(clone_raw)) };
    if node < MAX_DEVICES {
        Some(Decoded { kind: NodeKind::Primary, index: node, clone })
    } else if node >= RENDER_BASE && node < RENDER_BASE + MAX_DEVICES {
        Some(Decoded { kind: NodeKind::Render, index: node - RENDER_BASE, clone })
    } else {
        None
    }
}

/// The instance of this driver module that holds each device number, or
/// -1. Each driver module has its own copy (the core is linked into each),
/// and a number is held by at most one device on the system.
static INSTANCES: [AtomicI32; MAX_DEVICES as usize] = [const { AtomicI32::new(-1) }; MAX_DEVICES as usize];

/// Records that device number `index` belongs to instance `inst` of this
/// driver. `false`, and no change, if `index` is out of range, `inst` is
/// negative or the number is already recorded.
pub fn bind(index: u32, inst: Instance) -> bool {
    let Some(slot) = INSTANCES.get(index as usize) else { return false };
    inst.0 >= 0 && slot.compare_exchange(-1, inst.0, Ordering::AcqRel, Ordering::Relaxed).is_ok()
}

/// Forgets that device number `index` belongs to instance `inst`; `false`,
/// and no change, if it does not.
pub fn unbind(index: u32, inst: Instance) -> bool {
    let Some(slot) = INSTANCES.get(index as usize) else { return false };
    slot.compare_exchange(inst.0, -1, Ordering::AcqRel, Ordering::Relaxed).is_ok()
}

/// Maps a minor number to the driver instance that owns it. Drivers built
/// on the core use this as their `ddi::Driver::instance_of_minor`, so that
/// `getinfo(9E)` and the entry-point glue find the device for every node,
/// every clone and every anonymous file ([`crate::anon_fd`]). `None` for a
/// device number no instance of this driver holds (a device that has
/// detached, or one of another driver).
pub fn instance_of_minor(minor: Minor) -> Option<Instance> {
    let anon = || crate::anon_fd::decode(minor.0).map(|(index, _)| index);
    let index = decode(minor.0).map(|d| d.index).or_else(anon)?;
    let inst = INSTANCES.get(index as usize)?.load(Ordering::Acquire);
    if inst < 0 { None } else { Some(Instance(inst)) }
}

/// A NUL-terminated minor node name in a fixed buffer.
type Name = [u8; 12];

// `name` and `names` index arrays, but they only run during constant
// evaluation of the two statics below, where an out-of-bounds index would
// be a compile error; no code of theirs exists at run time.

const fn name(prefix: &[u8], n: u32) -> Name {
    let mut out = [0u8; 12];
    let mut i = 0;
    while i < prefix.len() {
        out[i] = prefix[i];
        i += 1;
    }
    if n >= 100 {
        out[i] = b'0' + (n / 100) as u8;
        i += 1;
    }
    if n >= 10 {
        out[i] = b'0' + (n / 10 % 10) as u8;
        i += 1;
    }
    out[i] = b'0' + (n % 10) as u8;
    out
}

const fn names(prefix: &[u8], base: u32) -> [Name; MAX_DEVICES as usize] {
    let mut out = [[0u8; 12]; MAX_DEVICES as usize];
    let mut i = 0;
    while i < MAX_DEVICES as usize {
        out[i] = name(prefix, base + i as u32);
        i += 1;
    }
    out
}

/// `card0` to `card63`. `ddi_create_minor_node(9F)` keeps a copy of the
/// name, but `ddi::MinorNode` wants a `'static` name to remove the node by,
/// hence the static tables.
static CARD_NAMES: [Name; MAX_DEVICES as usize] = names(b"card", 0);
/// `renderD128` to `renderD191`.
static RENDER_NAMES: [Name; MAX_DEVICES as usize] = names(b"renderD", RENDER_BASE);

/// Returns the minor node name of a device's primary or render node.
pub fn node_name(kind: NodeKind, index: u32) -> Option<&'static core::ffi::CStr> {
    let table = match kind {
        NodeKind::Primary => &CARD_NAMES,
        NodeKind::Render => &RENDER_NAMES,
    };
    let bytes = table.get(index as usize)?;
    core::ffi::CStr::from_bytes_until_nul(bytes).ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layout_matches_libdrm_mask() {
        // libdrm's illumos patch: d &= ~0x3fe00.
        assert_eq!(CLONE_MASK, 0x3fe00);
        let c = CloneId::new(MAX_FILES).unwrap();
        let m = file_minor(NodeKind::Render, 5, c).unwrap();
        assert_eq!(m & !CLONE_MASK, 133);
        assert_eq!(decode(m), Some(Decoded { kind: NodeKind::Render, index: 5, clone: Some(c) }));
        assert_eq!(decode(0), Some(Decoded { kind: NodeKind::Primary, index: 0, clone: None }));
        assert_eq!(decode(128 | (1 << 9)).map(|d| d.clone), Some(CloneId::new(1)));
    }

    #[test]
    fn rejects_foreign_minors() {
        assert_eq!(decode(64), None);
        assert_eq!(decode(127), None);
        assert_eq!(decode(192), None);
        assert_eq!(decode(1 << 18), None);
        assert_eq!(node_minor(NodeKind::Primary, 64), None);
        assert_eq!(CloneId::new(0), None);
        assert_eq!(CloneId::new(512), None);
    }

    #[test]
    fn instance_mapping() {
        // Device number 3 held by instance 0 of this driver, 4 by
        // instance 1; 5 by nobody here (another driver's device).
        assert!(bind(3, Instance(0)));
        assert!(bind(4, Instance(1)));
        assert_eq!(instance_of_minor(Minor(3)), Some(Instance(0)));
        assert_eq!(instance_of_minor(Minor(131 | (7 << 9))), Some(Instance(0)));
        assert_eq!(instance_of_minor(Minor(4)), Some(Instance(1)));
        assert_eq!(instance_of_minor(Minor(132)), Some(Instance(1)));
        let anon = crate::anon_fd::minor(4, 9).unwrap();
        assert_eq!(instance_of_minor(Minor(anon)), Some(Instance(1)));
        assert_eq!(instance_of_minor(Minor(5)), None);
        assert_eq!(instance_of_minor(Minor(133)), None);
        assert_eq!(instance_of_minor(Minor(200)), None);
        // One holder per number; only the holder can let go.
        assert!(!bind(3, Instance(2)));
        assert!(!unbind(3, Instance(1)));
        assert!(!bind(64, Instance(0)));
        assert!(!bind(6, Instance(-1)));
        assert!(unbind(3, Instance(0)));
        assert_eq!(instance_of_minor(Minor(3)), None);
        assert!(!unbind(3, Instance(0)), "a second unbind");
        // Reused after the detach, by another instance.
        assert!(bind(3, Instance(2)));
        assert_eq!(instance_of_minor(Minor(3)), Some(Instance(2)));
        assert!(unbind(3, Instance(2)) && unbind(4, Instance(1)));
    }

    #[test]
    fn node_names() {
        assert_eq!(node_name(NodeKind::Primary, 0).unwrap(), c"card0");
        assert_eq!(node_name(NodeKind::Primary, 63).unwrap(), c"card63");
        assert_eq!(node_name(NodeKind::Render, 0).unwrap(), c"renderD128");
        assert_eq!(node_name(NodeKind::Render, 63).unwrap(), c"renderD191");
        assert_eq!(node_name(NodeKind::Render, 64), None);
    }
}
