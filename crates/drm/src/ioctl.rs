// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The table of core ioctls and their decoding.
//!
//! The illumos `_IOWR` encoding keeps only the low eight bits of the
//! argument size, so the command number cannot tell the kernel how many
//! bytes to copy. The core therefore matches the complete 32-bit command
//! against the table: direction, size bits, group and number must all be
//! those of the `DRM_IOCTL_*` value in `drm-uapi`. Anything else, including
//! a known number with a different size or direction, fails with `ENOTTY`
//! rather than being guessed at.

use drm_uapi as uapi;
use drm_uapi::ioc;

use crate::state::Perm;

/// The core ioctls: those of milestone R0 and the GEM and dumb-buffer
/// ioctls of R1.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Op {
    /// `DRM_IOCTL_VERSION`.
    Version,
    /// `DRM_IOCTL_GET_UNIQUE`.
    GetUnique,
    /// `DRM_IOCTL_GET_MAGIC`.
    GetMagic,
    /// `DRM_IOCTL_GET_CLIENT` (index 0 only, as on current Linux).
    GetClient,
    /// `DRM_IOCTL_SET_VERSION`.
    SetVersion,
    /// `DRM_IOCTL_GET_CAP`.
    GetCap,
    /// `DRM_IOCTL_SET_CLIENT_CAP`.
    SetClientCap,
    /// `DRM_IOCTL_SET_UNIQUE`.
    SetUnique,
    /// `DRM_IOCTL_AUTH_MAGIC`.
    AuthMagic,
    /// `DRM_IOCTL_SET_MASTER`.
    SetMaster,
    /// `DRM_IOCTL_DROP_MASTER`.
    DropMaster,
    /// `DRM_IOCTL_GEM_CLOSE`.
    GemClose,
    /// `DRM_IOCTL_GEM_FLINK`.
    GemFlink,
    /// `DRM_IOCTL_GEM_OPEN`.
    GemOpen,
    /// `DRM_IOCTL_MODE_CREATE_DUMB`.
    CreateDumb,
    /// `DRM_IOCTL_MODE_MAP_DUMB`.
    MapDumb,
    /// `DRM_IOCTL_MODE_DESTROY_DUMB`.
    DestroyDumb,
}

const NONE: Perm = Perm { auth: false, master: false, root_only: false, render_allow: false };
const RENDER: Perm = Perm { render_allow: true, ..NONE };
const AUTH: Perm = Perm { auth: true, ..NONE };
const MASTER: Perm = Perm { master: true, ..NONE };
const AUTH_MASTER: Perm = Perm { auth: true, master: true, ..NONE };
const ROOT_AUTH_MASTER: Perm = Perm { auth: true, master: true, root_only: true, ..NONE };

/// One entry of the table: the full command number, the operation and its
/// permission flags. The flags are those of Linux's `drm_ioctls[]`;
/// `SET_UNIQUE` has the flags it had while Linux still implemented it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Entry {
    /// The command number.
    pub cmd: u32,
    /// What it does.
    pub op: Op,
    /// Who may do it.
    pub perm: Perm,
}

/// The ioctls the core implements.
pub const TABLE: &[Entry] = &[
    Entry { cmd: uapi::DRM_IOCTL_VERSION, op: Op::Version, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_GET_UNIQUE, op: Op::GetUnique, perm: NONE },
    Entry { cmd: uapi::DRM_IOCTL_GET_MAGIC, op: Op::GetMagic, perm: NONE },
    Entry { cmd: uapi::DRM_IOCTL_GET_CLIENT, op: Op::GetClient, perm: NONE },
    Entry { cmd: uapi::DRM_IOCTL_SET_VERSION, op: Op::SetVersion, perm: MASTER },
    Entry { cmd: uapi::DRM_IOCTL_GET_CAP, op: Op::GetCap, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_SET_CLIENT_CAP, op: Op::SetClientCap, perm: NONE },
    Entry { cmd: uapi::DRM_IOCTL_SET_UNIQUE, op: Op::SetUnique, perm: ROOT_AUTH_MASTER },
    Entry { cmd: uapi::DRM_IOCTL_AUTH_MAGIC, op: Op::AuthMagic, perm: AUTH_MASTER },
    Entry { cmd: uapi::DRM_IOCTL_SET_MASTER, op: Op::SetMaster, perm: NONE },
    Entry { cmd: uapi::DRM_IOCTL_DROP_MASTER, op: Op::DropMaster, perm: NONE },
    Entry { cmd: uapi::DRM_IOCTL_GEM_CLOSE, op: Op::GemClose, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_GEM_FLINK, op: Op::GemFlink, perm: AUTH },
    Entry { cmd: uapi::DRM_IOCTL_GEM_OPEN, op: Op::GemOpen, perm: AUTH },
    Entry { cmd: uapi::DRM_IOCTL_MODE_CREATE_DUMB, op: Op::CreateDumb, perm: NONE },
    Entry { cmd: uapi::DRM_IOCTL_MODE_MAP_DUMB, op: Op::MapDumb, perm: NONE },
    Entry { cmd: uapi::DRM_IOCTL_MODE_DESTROY_DUMB, op: Op::DestroyDumb, perm: NONE },
];

/// The first command number of driver-private ioctls (`DRM_COMMAND_BASE`).
pub const DRIVER_FIRST: u8 = 0x40;
/// The command number after the last driver-private ioctl
/// (`DRM_COMMAND_END`).
pub const DRIVER_END: u8 = 0xa0;

/// A driver-private ioctl: the complete command value, built with the
/// `drm_uapi::ioc` functions and a number from [`DRIVER_FIRST`] to below
/// [`DRIVER_END`], and its permission flags, as in Linux's
/// `DRM_IOCTL_DEF_DRV`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DriverIoctl {
    /// The command value.
    pub cmd: u32,
    /// Who may use it.
    pub perm: Perm,
}

impl DriverIoctl {
    /// Reports whether the command lies in the driver range of the DRM
    /// group; entries outside it are ignored by the core.
    pub const fn in_driver_range(&self) -> bool {
        let nr = ioc::ioc_nr(self.cmd);
        ioc::ioc_group(self.cmd) == ioc::DRM_IOCTL_BASE && nr >= DRIVER_FIRST && nr < DRIVER_END
    }
}

/// Finds a driver-private ioctl by its complete command value.
pub fn lookup_driver(table: &'static [DriverIoctl], cmd: u32) -> Option<&'static DriverIoctl> {
    table.iter().find(|e| e.cmd == cmd && e.in_driver_range())
}

/// Reports whether a command has the group and number of one of the
/// driver's ioctls, whatever its size and direction bits (for 32-bit
/// callers, as [`is_core_nr`]).
pub fn is_driver_nr(table: &[DriverIoctl], cmd: u32) -> bool {
    ioc::ioc_group(cmd) == ioc::DRM_IOCTL_BASE
        && table.iter().any(|e| e.in_driver_range() && ioc::ioc_nr(e.cmd) == ioc::ioc_nr(cmd))
}

/// Finds the table entry for a command, or `None` (`ENOTTY`).
pub fn lookup(cmd: u32) -> Option<&'static Entry> {
    TABLE.iter().find(|e| e.cmd == cmd)
}

/// Reports whether a command has the group and number of a core ioctl,
/// whatever its size and direction bits. Used to tell a 32-bit caller's
/// core command (`ENOTSUP`) from an unrelated command (`ENOTTY`).
pub fn is_core_nr(cmd: u32) -> bool {
    ioc::ioc_group(cmd) == ioc::DRM_IOCTL_BASE && TABLE.iter().any(|e| ioc::ioc_nr(e.cmd) == ioc::ioc_nr(cmd))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exact_match_only() {
        assert_eq!(lookup(uapi::DRM_IOCTL_VERSION).map(|e| e.op), Some(Op::Version));
        assert_eq!(lookup(uapi::DRM_IOCTL_DROP_MASTER).map(|e| e.op), Some(Op::DropMaster));
        // The 32-bit drm_version is 36 bytes: same number, other size.
        assert_eq!(lookup(ioc::drm_iowr::<[u8; 36]>(0x00)), None);
        // Same number and size, other direction.
        assert_eq!(lookup(ioc::drm_ior::<uapi::drm_version>(0x00)), None);
        // Another group letter.
        assert_eq!(lookup(uapi::DRM_IOCTL_VERSION ^ 0x0100), None);
        assert_eq!(lookup(uapi::DRM_IOCTL_GEM_CLOSE).map(|e| e.op), Some(Op::GemClose));
        assert_eq!(lookup(uapi::DRM_IOCTL_MODE_DESTROY_DUMB).map(|e| e.op), Some(Op::DestroyDumb));
        // Ioctls of later milestones are not in the table yet.
        assert_eq!(lookup(uapi::DRM_IOCTL_PRIME_HANDLE_TO_FD), None);
        assert_eq!(lookup(uapi::DRM_IOCTL_MODE_ATOMIC), None);
    }

    #[test]
    fn core_numbers_for_32_bit_callers() {
        // The ILP32 drm_version is 36 bytes: another command value, but a
        // core ioctl all the same.
        assert!(is_core_nr(ioc::drm_iowr::<[u8; 36]>(0x00)));
        assert!(is_core_nr(uapi::DRM_IOCTL_SET_MASTER));
        assert!(is_core_nr(uapi::DRM_IOCTL_GEM_CLOSE));
        assert!(!is_core_nr(uapi::DRM_IOCTL_PRIME_HANDLE_TO_FD));
        // TCGETA ('T' << 8 | 1), which isatty(3C) issues.
        assert!(!is_core_nr(0x5401));
    }

    #[test]
    fn table_is_consistent() {
        for (i, e) in TABLE.iter().enumerate() {
            assert_eq!(ioc::ioc_group(e.cmd), ioc::DRM_IOCTL_BASE);
            assert!(TABLE[i + 1..].iter().all(|o| o.cmd != e.cmd && o.op != e.op));
        }
        // Render nodes may query the version and capabilities and close GEM
        // handles, as in Linux's drm_ioctls[]; FLINK and OPEN need
        // authentication; the dumb-buffer ioctls are primary-node only.
        let render: [Op; 3] = [Op::Version, Op::GetCap, Op::GemClose];
        for e in TABLE {
            assert_eq!(e.perm.render_allow, render.contains(&e.op), "{:?}", e.op);
        }
        let auth: [Op; 4] = [Op::SetUnique, Op::AuthMagic, Op::GemFlink, Op::GemOpen];
        for e in TABLE {
            assert_eq!(e.perm.auth, auth.contains(&e.op), "{:?}", e.op);
        }
        for op in [Op::CreateDumb, Op::MapDumb, Op::DestroyDumb] {
            let e = TABLE.iter().find(|e| e.op == op).unwrap();
            assert_eq!(e.perm, NONE, "{op:?}");
        }
    }

    #[test]
    fn driver_range() {
        const T: &[DriverIoctl] = &[
            DriverIoctl { cmd: ioc::drm_iowr::<[u8; 8]>(0x40), perm: RENDER },
            DriverIoctl { cmd: ioc::drm_iowr::<[u8; 16]>(0x9f), perm: NONE },
            // Outside the driver range: ignored.
            DriverIoctl { cmd: ioc::drm_iowr::<[u8; 8]>(0x0c), perm: NONE },
            DriverIoctl { cmd: ioc::drm_iowr::<[u8; 8]>(0xa0), perm: NONE },
        ];
        assert_eq!(lookup_driver(T, ioc::drm_iowr::<[u8; 8]>(0x40)).map(|e| e.perm), Some(RENDER));
        assert!(lookup_driver(T, ioc::drm_iowr::<[u8; 16]>(0x9f)).is_some());
        assert!(lookup_driver(T, ioc::drm_iowr::<[u8; 12]>(0x40)).is_none(), "full value match");
        assert!(lookup_driver(T, ioc::drm_iowr::<[u8; 8]>(0x0c)).is_none());
        assert!(lookup_driver(T, ioc::drm_iowr::<[u8; 8]>(0xa0)).is_none());
        assert!(is_driver_nr(T, ioc::drm_iowr::<[u8; 4]>(0x40)));
        assert!(!is_driver_nr(T, ioc::drm_iowr::<[u8; 8]>(0x0c)));
        assert!(!is_driver_nr(T, ioc::drm_iowr::<[u8; 8]>(0x41)));
    }
}
