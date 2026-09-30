// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The table of core ioctls, their decoding, and the argument sizes.
//!
//! # Argument sizes
//!
//! Core and mode-setting commands are matched on their group, number and
//! direction, not on the size bits, and the caller's structure may be
//! shorter or longer than the kernel's, as with Linux's `drm_ioctl()`
//! (`drivers/gpu/drm/drm_ioctl.c`, read at v7.3-rc5). There, with `U` the
//! size in the caller's command, `K` the size in the kernel's definition,
//! `in = U` if both commands have `IOC_IN` (else 0) and `out = U` if both
//! have `IOC_OUT` (else 0), the kernel buffer is `max(in, out, K)` bytes,
//! `in` bytes are copied in, the rest of the buffer is zeroed, the handler
//! works on the first `K` bytes, and `out` bytes are copied out. So:
//!
//! * a shorter structure (`U < K`) is zero-extended, and only `U` bytes go
//!   back;
//! * a longer structure (`U > K`) has its extra bytes copied in and copied
//!   back unchanged if the command copies in, or zeroed on the way out if
//!   it only copies out; the handler never sees them, and an unreadable or
//!   unwritable tail is `EFAULT` like the rest of the structure.
//!
//! [`SizePlan`] is that computation. The core follows it byte for byte,
//! with two differences that come from its own design, not from the size
//! rule: a command whose direction differs from the table's is `ENOTTY`
//! (Linux uses the intersection of both directions), and a handler copies
//! its result out when it has one, as before, instead of the dispatcher
//! copying the buffer back after every call, including failed ones (the
//! handlers were written against Linux's errnos and undo work whose result
//! cannot be copied out). An unknown number stays `ENOTTY` (Linux answers
//! `EINVAL` for an unused number of the DRM group).
//!
//! The illumos `_IOWR` encoding keeps only the low eight bits of the size,
//! so a caller's size is known exactly only below 256 bytes; every core
//! and mode-setting structure is smaller than that (the tests check each
//! handler's structure against its command), and a caller's structure of
//! 256 bytes or more arrives with its size modulo 256, which is then what
//! is copied, as the C macros encoded it.
//!
//! Driver-private commands keep the exact match on the complete value
//! ([`lookup_driver`]); a driver that accepts several sizes of a structure
//! lists each (as `virtio_gpu` does for `EXECBUFFER`).

use drm_uapi as uapi;
use drm_uapi::ioc;

use crate::state::Perm;

/// The core ioctls: those of milestone R0, the GEM and dumb-buffer ioctls
/// of R1, and the PRIME and sync object ioctls of R2.
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
    // Milestone R2: PRIME and sync objects.
    /// `DRM_IOCTL_PRIME_HANDLE_TO_FD`.
    PrimeHandleToFd,
    /// `DRM_IOCTL_PRIME_FD_TO_HANDLE`.
    PrimeFdToHandle,
    /// `DRM_IOCTL_SYNCOBJ_CREATE`.
    SyncobjCreate,
    /// `DRM_IOCTL_SYNCOBJ_DESTROY`.
    SyncobjDestroy,
    /// `DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD`.
    SyncobjHandleToFd,
    /// `DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE`.
    SyncobjFdToHandle,
    /// `DRM_IOCTL_SYNCOBJ_WAIT`.
    SyncobjWait,
    /// `DRM_IOCTL_SYNCOBJ_RESET`.
    SyncobjReset,
    /// `DRM_IOCTL_SYNCOBJ_SIGNAL`.
    SyncobjSignal,
    /// `DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT`.
    SyncobjTimelineWait,
    /// `DRM_IOCTL_SYNCOBJ_QUERY`.
    SyncobjQuery,
    /// `DRM_IOCTL_SYNCOBJ_TRANSFER`.
    SyncobjTransfer,
    /// `DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL`.
    SyncobjTimelineSignal,
}

impl Op {
    /// Reports whether the operation is one of the sync object ioctls.
    pub const fn is_syncobj(self) -> bool {
        matches!(
            self,
            Op::SyncobjCreate
                | Op::SyncobjDestroy
                | Op::SyncobjHandleToFd
                | Op::SyncobjFdToHandle
                | Op::SyncobjWait
                | Op::SyncobjReset
                | Op::SyncobjSignal
                | Op::SyncobjTimelineWait
                | Op::SyncobjQuery
                | Op::SyncobjTransfer
                | Op::SyncobjTimelineSignal
        )
    }
    /// Reports whether the operation is one of the PRIME ioctls.
    pub const fn is_prime(self) -> bool {
        matches!(self, Op::PrimeHandleToFd | Op::PrimeFdToHandle)
    }
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
    // R2: render nodes may use all of them, without authentication, as in
    // current Linux (DRM_RENDER_ALLOW).
    Entry { cmd: uapi::DRM_IOCTL_PRIME_HANDLE_TO_FD, op: Op::PrimeHandleToFd, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_PRIME_FD_TO_HANDLE, op: Op::PrimeFdToHandle, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_SYNCOBJ_CREATE, op: Op::SyncobjCreate, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_SYNCOBJ_DESTROY, op: Op::SyncobjDestroy, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, op: Op::SyncobjHandleToFd, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, op: Op::SyncobjFdToHandle, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_SYNCOBJ_WAIT, op: Op::SyncobjWait, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_SYNCOBJ_RESET, op: Op::SyncobjReset, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_SYNCOBJ_SIGNAL, op: Op::SyncobjSignal, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, op: Op::SyncobjTimelineWait, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_SYNCOBJ_QUERY, op: Op::SyncobjQuery, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_SYNCOBJ_TRANSFER, op: Op::SyncobjTransfer, perm: RENDER },
    Entry { cmd: uapi::DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, op: Op::SyncobjTimelineSignal, perm: RENDER },
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

/// Reports whether `cmd` is the command `table_cmd` with any argument
/// size: the same group, number and direction.
pub const fn same_command(table_cmd: u32, cmd: u32) -> bool {
    table_cmd & !(ioc::IOCPARM_MASK << 16) == cmd & !(ioc::IOCPARM_MASK << 16)
}

/// Finds the table entry for a command, whatever its size bits, or `None`
/// (`ENOTTY`).
pub fn lookup(cmd: u32) -> Option<&'static Entry> {
    TABLE.iter().find(|e| same_command(e.cmd, cmd))
}

/// How the argument of one call is copied, from the caller's command and
/// the table's (see the module documentation). All sizes are in bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SizePlan {
    /// The caller's size `U`, from its command.
    pub user: usize,
    /// The kernel's size `K`, from the table's command.
    pub kernel: usize,
    /// Whether the argument is copied in (`IOC_IN` in both commands).
    pub input: bool,
    /// Whether the argument is copied out (`IOC_OUT` in both commands).
    pub output: bool,
}

impl SizePlan {
    /// The plan for a caller's command `cmd` that matched the table command
    /// `table_cmd`.
    pub const fn new(table_cmd: u32, cmd: u32) -> SizePlan {
        SizePlan {
            user: ioc::ioc_size(cmd),
            kernel: ioc::ioc_size(table_cmd),
            input: cmd & table_cmd & ioc::IOC_IN != 0,
            output: cmd & table_cmd & ioc::IOC_OUT != 0,
        }
    }
    /// The plan of a caller whose structure is the kernel's own.
    pub const fn exact(table_cmd: u32) -> SizePlan {
        SizePlan::new(table_cmd, table_cmd)
    }
    /// The bytes of the kernel structure filled from the caller; the rest
    /// of it is zero.
    pub const fn copy_in(&self) -> usize {
        if self.input { min(self.user, self.kernel) } else { 0 }
    }
    /// The bytes after the kernel structure that are copied in, kept, and
    /// copied back out unchanged (`U - K` when the caller's structure is
    /// longer and the command copies in).
    pub const fn tail_in(&self) -> usize {
        if self.input { self.user.saturating_sub(self.kernel) } else { 0 }
    }
    /// The bytes of the kernel structure copied out.
    pub const fn copy_out(&self) -> usize {
        if self.output { min(self.user, self.kernel) } else { 0 }
    }
    /// The bytes after the kernel structure that are copied out: the tail
    /// that was copied in, or zeros if the command does not copy in.
    pub const fn tail_out(&self) -> usize {
        if self.output { self.user.saturating_sub(self.kernel) } else { 0 }
    }
    /// Reports whether the caller's structure is exactly the kernel's.
    pub const fn is_exact(&self) -> bool {
        self.user == self.kernel
    }
}

const fn min(a: usize, b: usize) -> usize {
    if a < b { a } else { b }
}

/// Builds the bytes the caller receives, `U` of them, from the kernel
/// structure after the handler (`kernel`, `K` bytes) and the tail copied
/// in (`tail`, `plan.tail_in()` bytes), as Linux's `drm_ioctl()` copies
/// its buffer out. Returns the number of bytes written to `out`, which must
/// hold at least `plan.copy_out() + plan.tail_out()` bytes; `None` if a
/// length does not fit.
pub fn assemble_out(plan: &SizePlan, kernel: &[u8], tail: &[u8], out: &mut [u8]) -> Option<usize> {
    let head = plan.copy_out();
    let rest = plan.tail_out();
    out.get_mut(..head)?.copy_from_slice(kernel.get(..head)?);
    let t = out.get_mut(head..head + rest)?;
    if plan.tail_in() == rest {
        t.copy_from_slice(tail.get(..rest)?);
    } else {
        t.fill(0);
    }
    Some(head + rest)
}

/// Reports whether a command has the group and number of a core ioctl,
/// whatever its size and direction bits. Used to tell a 32-bit caller's
/// core command (`ENOTSUP`) from an unrelated command (`ENOTTY`).
pub fn is_core_nr(cmd: u32) -> bool {
    ioc::ioc_group(cmd) == ioc::DRM_IOCTL_BASE && TABLE.iter().any(|e| ioc::ioc_nr(e.cmd) == ioc::ioc_nr(cmd))
}

#[cfg(test)]
mod tests {
    extern crate std;

    use super::*;
    use std::vec;
    use std::vec::Vec;

    #[test]
    fn match_ignores_size_but_not_direction() {
        assert_eq!(lookup(uapi::DRM_IOCTL_VERSION).map(|e| e.op), Some(Op::Version));
        assert_eq!(lookup(uapi::DRM_IOCTL_DROP_MASTER).map(|e| e.op), Some(Op::DropMaster));
        // Another size of the same command is the same command (Linux's
        // drm_ioctl() zero-extends or truncates): the 36-byte ILP32
        // drm_version, a zero size, the largest size the encoding carries.
        for size in [0usize, 1, 36, 63, 65, 255] {
            let cmd = ioc::with_size(uapi::DRM_IOCTL_VERSION, size);
            assert_eq!(lookup(cmd).map(|e| e.op), Some(Op::Version), "size {size}");
        }
        // Same number, other direction: not the command.
        assert_eq!(lookup(ioc::drm_ior::<uapi::drm_version>(0x00)), None);
        assert_eq!(lookup(ioc::drm_iow::<uapi::drm_version>(0x00)), None);
        assert_eq!(lookup(ioc::drm_iowr::<[u8; 0]>(0x1e)), None, "SET_MASTER is _IO");
        // Another group letter.
        assert_eq!(lookup(uapi::DRM_IOCTL_VERSION ^ 0x0100), None);
        assert_eq!(lookup(uapi::DRM_IOCTL_GEM_CLOSE).map(|e| e.op), Some(Op::GemClose));
        assert_eq!(lookup(uapi::DRM_IOCTL_MODE_DESTROY_DUMB).map(|e| e.op), Some(Op::DestroyDumb));
        assert_eq!(lookup(uapi::DRM_IOCTL_PRIME_HANDLE_TO_FD).map(|e| e.op), Some(Op::PrimeHandleToFd));
        assert_eq!(lookup(uapi::DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL).map(|e| e.op), Some(Op::SyncobjTimelineSignal));
        // Mode setting has its own table (crate::kms::ioctl).
        assert_eq!(lookup(uapi::DRM_IOCTL_MODE_ATOMIC), None);
        // Unused numbers of the group.
        assert_eq!(lookup(ioc::drm_iowr::<[u8; 8]>(0x3f)), None);
    }

    /// The structures that grew between the libdrm 2.4.109 headers, which
    /// the kernel's uAPI comes from, and libdrm 2.4.134 (gfx-drm
    /// `README.illumos`): the command, the 2.4.109 size and the 2.4.134
    /// size.
    const GROWN: [(u32, usize, usize); 4] = [
        (uapi::DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, 16, 24),
        (uapi::DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, 16, 24),
        (uapi::DRM_IOCTL_SYNCOBJ_WAIT, 32, 40),
        (uapi::DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, 40, 48),
    ];

    #[test]
    fn grown_syncobj_commands() {
        for (cmd, old, new) in GROWN {
            assert_eq!(ioc::ioc_size(cmd), old, "the kernel's structure is the 2.4.109 one");
            let e = lookup(cmd).unwrap();
            let grown = ioc::with_size(cmd, new);
            assert_ne!(grown, cmd);
            assert_eq!(lookup(grown), Some(e), "libdrm 2.4.134's command reaches the same entry");
            let p = SizePlan::new(e.cmd, grown);
            assert_eq!((p.user, p.kernel, p.copy_in(), p.tail_in()), (new, old, old, new - old));
            assert_eq!((p.copy_out(), p.tail_out()), (old, new - old));
            assert!(SizePlan::new(e.cmd, cmd).is_exact());
        }
    }

    /// Linux's drm_ioctl() (v7.3-rc5), restated: the bytes the handler
    /// sees and the bytes the caller finds after the call, for a caller
    /// buffer `user_buf` (whose length is at least the caller's size) and
    /// a handler that sets every byte of its K-byte structure to `fill`.
    fn linux_model(table_cmd: u32, cmd: u32, user_buf: &[u8], fill: u8) -> (Vec<u8>, Vec<u8>) {
        let drv_size = ioc::ioc_size(table_cmd);
        let mut in_size = ioc::ioc_size(cmd);
        let mut out_size = in_size;
        if cmd & table_cmd & ioc::IOC_IN == 0 {
            in_size = 0;
        }
        if cmd & table_cmd & ioc::IOC_OUT == 0 {
            out_size = 0;
        }
        let ksize = in_size.max(out_size).max(drv_size);
        let mut kdata = vec![0u8; ksize];
        kdata[..in_size].copy_from_slice(&user_buf[..in_size]);
        let seen = kdata[..drv_size].to_vec();
        kdata[..drv_size].fill(fill);
        let mut after = user_buf.to_vec();
        after[..out_size].copy_from_slice(&kdata[..out_size]);
        (seen, after)
    }

    /// The same through SizePlan, as uaccess::CoreArg applies it.
    fn plan_model(table_cmd: u32, cmd: u32, user_buf: &[u8], fill: u8) -> (Vec<u8>, Vec<u8>) {
        let p = SizePlan::new(table_cmd, cmd);
        let mut k = vec![0u8; p.kernel];
        k[..p.copy_in()].copy_from_slice(&user_buf[..p.copy_in()]);
        let tail = user_buf[p.kernel.min(p.user)..][..p.tail_in()].to_vec();
        let seen = k.clone();
        k.fill(fill);
        let mut out = [0u8; 256];
        let n = assemble_out(&p, &k, &tail, &mut out).unwrap();
        let mut after = user_buf.to_vec();
        after[..n].copy_from_slice(&out[..n]);
        (seen, after)
    }

    #[test]
    fn sizes_follow_linux_drm_ioctl() {
        // Every entry of the table, every size the encoding can carry and
        // every direction, against the restated Linux code.
        let user_buf: Vec<u8> = (0..256u32).map(|i| (i as u8).wrapping_mul(7).wrapping_add(1)).collect();
        let dirs = [ioc::IOC_VOID, ioc::IOC_IN, ioc::IOC_OUT, ioc::IOC_INOUT];
        let mut n = 0;
        for e in TABLE {
            for dir in dirs {
                for size in 0..=255usize {
                    let cmd = ioc::with_size((e.cmd & !ioc::IOC_DIRMASK) | dir, size);
                    let a = linux_model(e.cmd, cmd, &user_buf, 0xa5);
                    let b = plan_model(e.cmd, cmd, &user_buf, 0xa5);
                    assert_eq!(a, b, "{:?} dir {dir:#x} size {size}", e.op);
                    n += 1;
                }
            }
        }
        assert_eq!(n, TABLE.len() * 4 * 256);
    }

    #[test]
    fn plan_cases() {
        let wr = |n: u8, size: usize| ioc::with_size(ioc::drm_iowr::<[u8; 0]>(n), size);
        let t = wr(0x10, 16);
        // Shorter: zero-extended in, truncated out.
        let p = SizePlan::new(t, wr(0x10, 8));
        assert_eq!((p.copy_in(), p.tail_in(), p.copy_out(), p.tail_out()), (8, 0, 8, 0));
        // Longer: the tail is copied in and back.
        let p = SizePlan::new(t, wr(0x10, 24));
        assert_eq!((p.copy_in(), p.tail_in(), p.copy_out(), p.tail_out()), (16, 8, 16, 8));
        // Longer, read-only command: the tail comes back as zeros.
        let r = ioc::with_size(ioc::drm_ior::<[u8; 0]>(0x02), 4);
        let p = SizePlan::new(r, ioc::with_size(r, 8));
        assert_eq!((p.copy_in(), p.tail_in(), p.copy_out(), p.tail_out()), (0, 0, 4, 4));
        let (k, tail, mut out) = ([1u8; 4], [9u8; 4], [0xffu8; 8]);
        assert_eq!(assemble_out(&p, &k, &tail, &mut out), Some(8));
        assert_eq!(out, [1, 1, 1, 1, 0, 0, 0, 0]);
        // Write-only command: nothing goes back.
        let w = ioc::with_size(ioc::drm_iow::<[u8; 0]>(0x11), 4);
        let p = SizePlan::new(w, ioc::with_size(w, 12));
        assert_eq!((p.copy_in(), p.tail_in(), p.copy_out(), p.tail_out()), (4, 8, 0, 0));
        // An output buffer too small is refused, not overrun.
        let p = SizePlan::new(t, wr(0x10, 24));
        assert_eq!(assemble_out(&p, &[0u8; 16], &[0u8; 8], &mut [0u8; 23]), None);
    }

    /// The size of the structure each handler reads and writes
    /// (`UserArg::<T>` in `device.rs`, `prime.rs`, `syncobj_ioctl.rs`), 0
    /// for the `_IO` commands. The match is exhaustive, so a new operation
    /// must name its structure here.
    fn handler_size(op: Op) -> usize {
        use core::mem::size_of;
        match op {
            Op::Version => size_of::<uapi::drm_version>(),
            Op::GetUnique | Op::SetUnique => size_of::<uapi::drm_unique>(),
            Op::GetMagic | Op::AuthMagic => size_of::<uapi::drm_auth>(),
            Op::GetClient => size_of::<uapi::drm_client>(),
            Op::SetVersion => size_of::<uapi::drm_set_version>(),
            Op::GetCap => size_of::<uapi::drm_get_cap>(),
            Op::SetClientCap => size_of::<uapi::drm_set_client_cap>(),
            Op::SetMaster | Op::DropMaster => 0,
            Op::GemClose => size_of::<uapi::drm_gem_close>(),
            Op::GemFlink => size_of::<uapi::drm_gem_flink>(),
            Op::GemOpen => size_of::<uapi::drm_gem_open>(),
            Op::CreateDumb => size_of::<uapi::drm_mode_create_dumb>(),
            Op::MapDumb => size_of::<uapi::drm_mode_map_dumb>(),
            Op::DestroyDumb => size_of::<uapi::drm_mode_destroy_dumb>(),
            Op::PrimeHandleToFd | Op::PrimeFdToHandle => size_of::<uapi::drm_prime_handle>(),
            Op::SyncobjCreate => size_of::<uapi::drm_syncobj_create>(),
            Op::SyncobjDestroy => size_of::<uapi::drm_syncobj_destroy>(),
            Op::SyncobjHandleToFd | Op::SyncobjFdToHandle => size_of::<uapi::drm_syncobj_handle>(),
            Op::SyncobjWait => size_of::<uapi::drm_syncobj_wait>(),
            Op::SyncobjReset | Op::SyncobjSignal => size_of::<uapi::drm_syncobj_array>(),
            Op::SyncobjTimelineWait => size_of::<uapi::drm_syncobj_timeline_wait>(),
            Op::SyncobjQuery | Op::SyncobjTimelineSignal => size_of::<uapi::drm_syncobj_timeline_array>(),
            Op::SyncobjTransfer => size_of::<uapi::drm_syncobj_transfer>(),
        }
    }

    #[test]
    fn handler_structures_match_their_commands() {
        // The size bits of a command are the size of its structure modulo
        // 256. Every handler's structure must be below 256 bytes and have
        // exactly the size its table command carries, or uaccess::CoreArg
        // would refuse every call to it (EINVAL) and a caller's size could
        // not be told from the command.
        for e in TABLE {
            let size = handler_size(e.op);
            assert!(size < 256, "{:?}: {size} bytes", e.op);
            assert_eq!(ioc::ioc_size(e.cmd), size, "{:?}", e.op);
        }
    }

    #[test]
    fn core_numbers_for_32_bit_callers() {
        // The ILP32 drm_version is 36 bytes: another command value, but a
        // core ioctl all the same.
        assert!(is_core_nr(ioc::drm_iowr::<[u8; 36]>(0x00)));
        assert!(is_core_nr(uapi::DRM_IOCTL_SET_MASTER));
        assert!(is_core_nr(uapi::DRM_IOCTL_GEM_CLOSE));
        assert!(is_core_nr(uapi::DRM_IOCTL_PRIME_HANDLE_TO_FD));
        assert!(!is_core_nr(uapi::DRM_IOCTL_MODE_ATOMIC));
        // TCGETA ('T' << 8 | 1), which isatty(3C) issues.
        assert!(!is_core_nr(0x5401));
    }

    #[test]
    fn table_is_consistent() {
        for (i, e) in TABLE.iter().enumerate() {
            assert_eq!(ioc::ioc_group(e.cmd), ioc::DRM_IOCTL_BASE);
            // One entry per number, whatever the size and direction.
            assert!(TABLE[i + 1..].iter().all(|o| ioc::ioc_nr(o.cmd) != ioc::ioc_nr(e.cmd) && o.op != e.op));
        }
        // Render nodes may query the version and capabilities and close GEM
        // handles, as in Linux's drm_ioctls[]; FLINK and OPEN need
        // authentication; the dumb-buffer ioctls are primary-node only.
        let render: [Op; 3] = [Op::Version, Op::GetCap, Op::GemClose];
        for e in TABLE {
            let r2 = e.op.is_prime() || e.op.is_syncobj();
            assert_eq!(e.perm.render_allow, render.contains(&e.op) || r2, "{:?}", e.op);
            if r2 {
                assert_eq!(e.perm, RENDER, "{:?}", e.op);
            }
        }
        assert_eq!(TABLE.iter().filter(|e| e.op.is_syncobj()).count(), 11);
        assert_eq!(TABLE.iter().filter(|e| e.op.is_prime()).count(), 2);
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
