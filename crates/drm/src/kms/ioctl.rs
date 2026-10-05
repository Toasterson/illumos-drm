// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The mode-setting ioctls and their permission flags.
//!
//! The flags are those of Linux's `drm_ioctls[]`: the ioctls that change
//! what is on screen need the master (`DRM_MASTER`), the others need
//! nothing beyond a primary node; none is allowed on render nodes. As for
//! the core table ([`crate::ioctl`]), commands are matched on their group,
//! number and direction, and the caller's structure may be shorter or
//! longer than the kernel's (Linux's `drm_ioctl()` size rule, see
//! [`crate::ioctl`]).

use drm_uapi as uapi;
use drm_uapi::ioc;

use crate::state::Perm;

/// The mode-setting ioctls.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[allow(missing_docs, reason = "each variant is the DRM_IOCTL_* of the same name")]
pub enum KmsOp {
    ModesetCtl,
    WaitVblank,
    CrtcGetSequence,
    CrtcQueueSequence,
    GetResources,
    GetCrtc,
    SetCrtc,
    Cursor,
    GetGamma,
    SetGamma,
    GetEncoder,
    GetConnector,
    AttachMode,
    DetachMode,
    GetProperty,
    SetProperty,
    GetPropBlob,
    GetFb,
    AddFb,
    RmFb,
    PageFlip,
    DirtyFb,
    GetPlaneResources,
    GetPlane,
    SetPlane,
    AddFb2,
    ObjGetProperties,
    ObjSetProperty,
    Cursor2,
    Atomic,
    CreatePropBlob,
    DestroyPropBlob,
    GetFb2,
}

const ANY: Perm = Perm { auth: false, master: false, root_only: false, render_allow: false };
const MASTER: Perm = Perm { master: true, ..ANY };

/// One entry: the command value, the operation, the permission.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct KmsEntry {
    /// The command value.
    pub cmd: u32,
    /// The operation.
    pub op: KmsOp,
    /// Who may use it.
    pub perm: Perm,
}

const fn e(cmd: u32, op: KmsOp, perm: Perm) -> KmsEntry {
    KmsEntry { cmd, op, perm }
}

/// The mode-setting ioctls the core implements.
pub const TABLE: &[KmsEntry] = &[
    e(uapi::DRM_IOCTL_MODESET_CTL, KmsOp::ModesetCtl, ANY),
    e(uapi::DRM_IOCTL_WAIT_VBLANK, KmsOp::WaitVblank, ANY),
    e(uapi::DRM_IOCTL_CRTC_GET_SEQUENCE, KmsOp::CrtcGetSequence, ANY),
    e(uapi::DRM_IOCTL_CRTC_QUEUE_SEQUENCE, KmsOp::CrtcQueueSequence, ANY),
    e(uapi::DRM_IOCTL_MODE_GETRESOURCES, KmsOp::GetResources, ANY),
    e(uapi::DRM_IOCTL_MODE_GETCRTC, KmsOp::GetCrtc, ANY),
    e(uapi::DRM_IOCTL_MODE_SETCRTC, KmsOp::SetCrtc, MASTER),
    e(uapi::DRM_IOCTL_MODE_CURSOR, KmsOp::Cursor, MASTER),
    e(uapi::DRM_IOCTL_MODE_GETGAMMA, KmsOp::GetGamma, ANY),
    e(uapi::DRM_IOCTL_MODE_SETGAMMA, KmsOp::SetGamma, MASTER),
    e(uapi::DRM_IOCTL_MODE_GETENCODER, KmsOp::GetEncoder, ANY),
    e(uapi::DRM_IOCTL_MODE_GETCONNECTOR, KmsOp::GetConnector, ANY),
    e(uapi::DRM_IOCTL_MODE_ATTACHMODE, KmsOp::AttachMode, MASTER),
    e(uapi::DRM_IOCTL_MODE_DETACHMODE, KmsOp::DetachMode, MASTER),
    e(uapi::DRM_IOCTL_MODE_GETPROPERTY, KmsOp::GetProperty, ANY),
    e(uapi::DRM_IOCTL_MODE_SETPROPERTY, KmsOp::SetProperty, MASTER),
    e(uapi::DRM_IOCTL_MODE_GETPROPBLOB, KmsOp::GetPropBlob, ANY),
    e(uapi::DRM_IOCTL_MODE_GETFB, KmsOp::GetFb, ANY),
    e(uapi::DRM_IOCTL_MODE_ADDFB, KmsOp::AddFb, ANY),
    e(uapi::DRM_IOCTL_MODE_RMFB, KmsOp::RmFb, ANY),
    e(uapi::DRM_IOCTL_MODE_PAGE_FLIP, KmsOp::PageFlip, MASTER),
    e(uapi::DRM_IOCTL_MODE_DIRTYFB, KmsOp::DirtyFb, MASTER),
    e(uapi::DRM_IOCTL_MODE_GETPLANERESOURCES, KmsOp::GetPlaneResources, ANY),
    e(uapi::DRM_IOCTL_MODE_GETPLANE, KmsOp::GetPlane, ANY),
    e(uapi::DRM_IOCTL_MODE_SETPLANE, KmsOp::SetPlane, MASTER),
    e(uapi::DRM_IOCTL_MODE_ADDFB2, KmsOp::AddFb2, ANY),
    e(uapi::DRM_IOCTL_MODE_OBJ_GETPROPERTIES, KmsOp::ObjGetProperties, ANY),
    e(uapi::DRM_IOCTL_MODE_OBJ_SETPROPERTY, KmsOp::ObjSetProperty, MASTER),
    e(uapi::DRM_IOCTL_MODE_CURSOR2, KmsOp::Cursor2, MASTER),
    e(uapi::DRM_IOCTL_MODE_ATOMIC, KmsOp::Atomic, MASTER),
    e(uapi::DRM_IOCTL_MODE_CREATEPROPBLOB, KmsOp::CreatePropBlob, ANY),
    e(uapi::DRM_IOCTL_MODE_DESTROYPROPBLOB, KmsOp::DestroyPropBlob, ANY),
    e(uapi::DRM_IOCTL_MODE_GETFB2, KmsOp::GetFb2, ANY),
];

/// Finds a mode-setting ioctl by its group, number and direction, whatever
/// its size bits.
pub fn lookup(cmd: u32) -> Option<&'static KmsEntry> {
    TABLE.iter().find(|x| crate::ioctl::same_command(x.cmd, cmd))
}

/// Whether a command has the group and number of a mode-setting ioctl,
/// whatever its size (32-bit callers get `ENOTSUP` for these).
pub fn is_kms_nr(cmd: u32) -> bool {
    ioc::ioc_group(cmd) == ioc::DRM_IOCTL_BASE && TABLE.iter().any(|x| ioc::ioc_nr(x.cmd) == ioc::ioc_nr(cmd))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The size of the structure each handler of `kms::dev` reads and
    /// writes (the no-op `MODESET_CTL`, `ATTACHMODE` and `DETACHMODE` read
    /// nothing; their commands' structures are named). Exhaustive, so a new
    /// operation must name its structure here.
    fn handler_size(op: KmsOp) -> usize {
        use core::mem::size_of;
        match op {
            KmsOp::ModesetCtl => size_of::<uapi::drm_modeset_ctl>(),
            KmsOp::AttachMode | KmsOp::DetachMode => size_of::<uapi::drm_mode_mode_cmd>(),
            KmsOp::WaitVblank => size_of::<uapi::drm_wait_vblank>(),
            KmsOp::CrtcGetSequence => size_of::<uapi::drm_crtc_get_sequence>(),
            KmsOp::CrtcQueueSequence => size_of::<uapi::drm_crtc_queue_sequence>(),
            KmsOp::GetResources => size_of::<uapi::drm_mode_card_res>(),
            KmsOp::GetCrtc | KmsOp::SetCrtc => size_of::<uapi::drm_mode_crtc>(),
            KmsOp::Cursor => size_of::<uapi::drm_mode_cursor>(),
            KmsOp::Cursor2 => size_of::<uapi::drm_mode_cursor2>(),
            KmsOp::GetGamma | KmsOp::SetGamma => size_of::<uapi::drm_mode_crtc_lut>(),
            KmsOp::GetEncoder => size_of::<uapi::drm_mode_get_encoder>(),
            KmsOp::GetConnector => size_of::<uapi::drm_mode_get_connector>(),
            KmsOp::GetProperty => size_of::<uapi::drm_mode_get_property>(),
            KmsOp::SetProperty => size_of::<uapi::drm_mode_connector_set_property>(),
            KmsOp::GetPropBlob => size_of::<uapi::drm_mode_get_blob>(),
            KmsOp::GetFb | KmsOp::AddFb => size_of::<uapi::drm_mode_fb_cmd>(),
            KmsOp::GetFb2 | KmsOp::AddFb2 => size_of::<uapi::drm_mode_fb_cmd2>(),
            KmsOp::RmFb => size_of::<u32>(),
            KmsOp::PageFlip => size_of::<uapi::drm_mode_crtc_page_flip>(),
            KmsOp::DirtyFb => size_of::<uapi::drm_mode_fb_dirty_cmd>(),
            KmsOp::GetPlaneResources => size_of::<uapi::drm_mode_get_plane_res>(),
            KmsOp::GetPlane => size_of::<uapi::drm_mode_get_plane>(),
            KmsOp::SetPlane => size_of::<uapi::drm_mode_set_plane>(),
            KmsOp::ObjGetProperties => size_of::<uapi::drm_mode_obj_get_properties>(),
            KmsOp::ObjSetProperty => size_of::<uapi::drm_mode_obj_set_property>(),
            KmsOp::Atomic => size_of::<uapi::drm_mode_atomic>(),
            KmsOp::CreatePropBlob => size_of::<uapi::drm_mode_create_blob>(),
            KmsOp::DestroyPropBlob => size_of::<uapi::drm_mode_destroy_blob>(),
        }
    }

    #[test]
    fn table() {
        for (i, x) in TABLE.iter().enumerate() {
            assert!(
                TABLE[i + 1..].iter().all(|o| ioc::ioc_nr(o.cmd) != ioc::ioc_nr(x.cmd) && o.op != x.op),
                "{:?}",
                x.op
            );
            assert!(
                crate::ioctl::TABLE.iter().all(|c| ioc::ioc_nr(c.cmd) != ioc::ioc_nr(x.cmd)),
                "{:?} has the number of a core ioctl",
                x.op
            );
            assert!(!x.perm.render_allow && !x.perm.auth && !x.perm.root_only);
            // The handler's structure is below 256 bytes and is the size
            // the command carries (see crate::ioctl).
            let size = handler_size(x.op);
            assert!(size < 256, "{:?}: {size} bytes", x.op);
            assert_eq!(ioc::ioc_size(x.cmd), size, "{:?}", x.op);
            // Any size of the command finds it; another direction does not.
            for size in [0, 4, 255] {
                assert_eq!(lookup(ioc::with_size(x.cmd, size)).map(|e| e.op), Some(x.op));
            }
            let other = if ioc::ioc_dir(x.cmd) == ioc::IOC_INOUT { ioc::IOC_OUT } else { ioc::IOC_INOUT };
            assert!(lookup((x.cmd & !ioc::IOC_DIRMASK) | other).is_none(), "{:?}", x.op);
        }
        let master = [
            KmsOp::SetCrtc,
            KmsOp::Cursor,
            KmsOp::SetGamma,
            KmsOp::AttachMode,
            KmsOp::DetachMode,
            KmsOp::SetProperty,
            KmsOp::PageFlip,
            KmsOp::DirtyFb,
            KmsOp::SetPlane,
            KmsOp::ObjSetProperty,
            KmsOp::Cursor2,
            KmsOp::Atomic,
        ];
        for x in TABLE {
            assert_eq!(x.perm.master, master.contains(&x.op), "{:?}", x.op);
        }
        assert_eq!(lookup(uapi::DRM_IOCTL_MODE_ATOMIC).map(|x| x.op), Some(KmsOp::Atomic));
        assert!(lookup(uapi::DRM_IOCTL_MODE_CREATE_LEASE).is_none(), "leases are not implemented");
        assert!(is_kms_nr(ioc::drm_iowr::<[u8; 20]>(0xbc)));
        assert!(!is_kms_nr(uapi::DRM_IOCTL_VERSION));
    }
}
