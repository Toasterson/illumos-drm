// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Sync files: a fence as a file descriptor (Linux's `sync_file`).
//!
//! `SYNCOBJ_HANDLE_TO_FD` with `EXPORT_SYNC_FILE` wraps the fence of a sync
//! object in a sync file, `SYNCOBJ_FD_TO_HANDLE` with `IMPORT_SYNC_FILE`
//! puts the fence of a sync file into a sync object, and a sync file can be
//! passed between processes like any descriptor ([`crate::anon_fd`]).
//! `poll(2)` reports `POLLIN` once the fence has signalled: the sync file
//! registers a fence callback when it is created, which calls
//! `pollwakeup(9F)` from the signaller's context, interrupt context
//! included.
//!
//! The descriptor also answers the two ioctls of `linux/sync_file.h` that
//! libsync (in libdrm) uses: `SYNC_IOC_MERGE` makes a new sync file whose
//! fence signals when the fences of both have ([`merge`]: members of the
//! same context are reduced to the later one, signalled ones are left out,
//! as Linux's `dma_fence_unwrap_merge` does), and `SYNC_IOC_FILE_INFO`
//! reports the status and the fences. The argument structures and command
//! numbers are those of the Linux header in the illumos `_IOWR` encoding,
//! as libsync compiled on illumos produces them.

use ddi::{Errno, Sleepable};

use crate::defer::Ctx;
use crate::fence::{Fence, FenceDomain};
use crate::karc::KArc;
use crate::kvec::KVec;

/// `struct sync_merge_data` of `linux/sync_file.h`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SyncMergeData {
    /// The name of the new sync file.
    pub name: [u8; 32],
    /// The other sync file.
    pub fd2: i32,
    /// The new sync file (out).
    pub fence: i32,
    /// Must be 0.
    pub flags: u32,
    /// Must be 0.
    pub pad: u32,
}

/// `struct sync_fence_info` of `linux/sync_file.h`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SyncFenceInfo {
    /// The timeline's name.
    pub obj_name: [u8; 32],
    /// The driver's name.
    pub driver_name: [u8; 32],
    /// 1 signalled, 0 active, negative errno for an error.
    pub status: i32,
    /// Unused, 0.
    pub flags: u32,
    /// The monotonic time of the signal, 0 while active.
    pub timestamp_ns: u64,
}

/// `struct sync_file_info` of `linux/sync_file.h`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SyncFileInfo {
    /// The sync file's name.
    pub name: [u8; 32],
    /// The status of the whole file, as for a fence.
    pub status: i32,
    /// Must be 0.
    pub flags: u32,
    /// In: room in the array (0 asks for the count); out: the count.
    pub num_fences: u32,
    /// Must be 0.
    pub pad: u32,
    /// The address of an array of [`SyncFenceInfo`].
    pub sync_fence_info: u64,
}

const _: () = assert!(size_of::<SyncMergeData>() == 48);
const _: () = assert!(size_of::<SyncFenceInfo>() == 80);
const _: () = assert!(size_of::<SyncFileInfo>() == 56);
const _: () = assert!(core::mem::offset_of!(SyncFileInfo, sync_fence_info) == 48);

/// `struct dma_buf_sync` of `linux/dma-buf.h`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct DmaBufSync {
    /// `DMA_BUF_SYNC_*`.
    pub flags: u64,
}

const fn iowr(group: u8, nr: u8, size: usize) -> u32 {
    drm_uapi::ioc::IOC_INOUT | (((size as u32) & drm_uapi::ioc::IOCPARM_MASK) << 16) | ((group as u32) << 8) | nr as u32
}
const fn iow(group: u8, nr: u8, size: usize) -> u32 {
    drm_uapi::ioc::IOC_IN | (((size as u32) & drm_uapi::ioc::IOCPARM_MASK) << 16) | ((group as u32) << 8) | nr as u32
}

/// `SYNC_IOC_MERGE`, `_IOWR('>', 3, struct sync_merge_data)`.
pub const SYNC_IOC_MERGE: u32 = iowr(b'>', 3, size_of::<SyncMergeData>());
/// `SYNC_IOC_FILE_INFO`, `_IOWR('>', 4, struct sync_file_info)`.
pub const SYNC_IOC_FILE_INFO: u32 = iowr(b'>', 4, size_of::<SyncFileInfo>());
/// `DMA_BUF_IOCTL_SYNC`, `_IOW('b', 0, struct dma_buf_sync)`.
pub const DMA_BUF_IOCTL_SYNC: u32 = iow(b'b', 0, size_of::<DmaBufSync>());
/// The valid flags of `DMA_BUF_IOCTL_SYNC`: read, write, end.
pub const DMA_BUF_SYNC_VALID: u64 = 0x7;
/// `DMA_BUF_SYNC_RW`.
pub const DMA_BUF_SYNC_RW: u64 = 0x3;

/// Returns the status of `f` as sync files report it: 1 signalled, 0
/// active, a negative errno for an error (Linux's `dma_fence_get_status`).
pub fn status(f: &Fence) -> i32 {
    if !f.is_signaled() {
        return 0;
    }
    f.error().map_or(1, |e| -e.get())
}

/// Returns the fences a sync file of `f` consists of: an array's members,
/// or `f` itself.
pub fn members(f: &Fence) -> &[Fence] {
    f.array_members().unwrap_or(core::slice::from_ref(f))
}

/// Merges the fences of two sync files (Linux's `sync_file_merge`): the
/// members of both, one per context (the later), unsignalled ones only.
/// The result is a signalled fence if nothing is left, the one fence if one
/// is, and an array on a new context otherwise.
pub fn merge(domain: &KArc<FenceDomain>, a: &Fence, b: &Fence, ctx: &Sleepable) -> Result<Fence, Errno> {
    let mut out: KVec<Fence> = KVec::new();
    for f in members(a).iter().chain(members(b)) {
        if f.is_signaled() {
            continue;
        }
        match out.iter_mut().find(|o| o.context() == f.context()) {
            Some(o) => {
                if f.is_later(o) {
                    let old = core::mem::replace(o, f.clone());
                    old.put(Ctx::Sleep(ctx));
                }
            }
            None => out.push(f.clone(), ctx)?,
        }
    }
    match out.len() {
        0 => Fence::signaled_stub(domain, ctx),
        1 => out.swap_remove(0).ok_or(Errno::EINVAL),
        _ => Fence::array(domain, out, domain.alloc_contexts(1), 1, false, ctx),
    }
}

/// Copies a name into a 32-byte field, NUL-terminated.
pub fn name_field(src: &[u8]) -> [u8; 32] {
    let mut out = [0u8; 32];
    let n = src.iter().position(|b| *b == 0).unwrap_or(src.len()).min(31);
    if let (Some(d), Some(s)) = (out.get_mut(..n), src.get(..n)) {
        d.copy_from_slice(s);
    }
    out
}

#[cfg(not(test))]
pub use imp::SyncFile;

#[cfg(not(test))]
mod imp {
    use core::sync::atomic::{AtomicPtr, Ordering};

    use ddi::{Errno, PollEvents, PollHead, PollRegistration, Sleepable};

    use crate::defer::Ctx;
    use crate::fence::{Fence, FenceCallback, FenceCb, SyncStats};
    use crate::karc::KArc;

    /// The fence callback of a sync file: wakes its pollers. Holds a raw
    /// pointer to the file's head, which is valid because the file removes
    /// the callback (waiting for a running one) before the head goes.
    struct PollWake(AtomicPtr<PollHead>);

    impl FenceCallback for PollWake {
        fn signaled(&self, _f: &Fence, _ctx: Ctx<'_>) -> Option<Fence> {
            let p = self.0.load(Ordering::Acquire);
            if !p.is_null() {
                // SAFETY: see PollWake.
                unsafe { &*p }.wakeup(PollEvents::IN.union(PollEvents::RDNORM));
            }
            None
        }
    }

    /// A sync file. See the module documentation.
    pub struct SyncFile {
        fence: Fence,
        name: [u8; 32],
        head: PollHead,
        cb: FenceCb<PollWake>,
    }

    impl SyncFile {
        /// Makes a sync file of `fence` named `name`.
        pub fn new(fence: Fence, name: &[u8], ctx: &Sleepable) -> Result<KArc<SyncFile>, Errno> {
            let cb = FenceCb::new(PollWake(AtomicPtr::new(core::ptr::null_mut())), ctx)?;
            let head = PollHead::new(ctx);
            let sf = KArc::new(SyncFile { fence, name: super::name_field(name), head, cb }, ctx)?;
            // The head does not move any more (it is in the KArc, and its
            // pollhead in a box of its own).
            sf.cb.get().0.store(&sf.head as *const PollHead as *mut PollHead, Ordering::Release);
            // An already signalled fence needs no callback: polls see it.
            let _ = sf.fence.add_callback(&sf.cb);
            SyncStats::inc(&sf.fence.domain().stats.sync_files);
            Ok(sf)
        }

        /// Returns the fence.
        pub fn fence(&self) -> &Fence {
            &self.fence
        }

        /// Returns the name.
        pub fn name(&self) -> &[u8; 32] {
            &self.name
        }

        /// `chpoll(9E)`: `POLLIN` once the fence has signalled.
        pub fn chpoll(&self, reg: &mut PollRegistration<'_>) -> PollEvents {
            reg.register(&self.head);
            if self.fence.is_signaled() { PollEvents::IN.union(PollEvents::RDNORM) } else { PollEvents::NONE }
        }

        /// Drops a reference in thread context.
        pub fn put(this: KArc<SyncFile>, ctx: &Sleepable) {
            if let Some(last) = KArc::release(this) {
                last.fence.remove_callback(&last.cb, ctx);
                drop(last);
            }
        }
    }

    impl Drop for SyncFile {
        /// Thread context (sync files are only dropped by ioctls and
        /// `close(9E)`): the callback goes before the head it points to.
        fn drop(&mut self) {
            self.fence.remove_callback_spin(&self.cb);
            SyncStats::dec(&self.fence.domain().stats.sync_files);
        }
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;

    fn sleep() -> Sleepable {
        // SAFETY: host test threads may block.
        unsafe { Sleepable::assume() }
    }

    #[test]
    fn merge_dedups_contexts_and_skips_signaled() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        let (c1, c2) = (d.alloc_contexts(1), d.alloc_contexts(1));
        let a1 = Fence::new(&d, c1, 1, &ctx).unwrap();
        let a5 = Fence::new(&d, c1, 5, &ctx).unwrap();
        let b = Fence::new(&d, c2, 1, &ctx).unwrap();
        let done = Fence::signaled_stub(&d, &ctx).unwrap();
        let m = merge(&d, &a1, &a5, &ctx).unwrap();
        assert!(Fence::ptr_eq(&m, &a5), "one context: the later fence");
        let m2 = merge(&d, &m, &b, &ctx).unwrap();
        assert_eq!(members(&m2).len(), 2);
        let m3 = merge(&d, &m2, &done, &ctx).unwrap();
        assert_eq!(members(&m3).len(), 2, "signalled fences are left out, arrays are flattened");
        assert_eq!(status(&m3), 0);
        a5.signal_with_error(Errno::EIO, Ctx::Sleep(&ctx));
        b.signal(Ctx::Sleep(&ctx));
        assert_eq!(status(&m3), -Errno::EIO.get());
        assert_eq!(status(&b), 1);
        let none = merge(&d, &a5, &done, &ctx).unwrap();
        assert!(none.is_signaled(), "nothing left: a signalled fence");
        assert_eq!(&name_field(b"merged\0junk")[..7], b"merged\0");
        assert_eq!(name_field(&[b'x'; 40])[31], 0, "always terminated");
        drop((a1, a5, b, done, m, m2, m3, none));
        d.drain(&ctx);
        assert_eq!(d.stats.live(), [0; 8]);
    }

    #[test]
    fn ioctl_numbers() {
        // _IOWR('>', 3, 48 bytes) and _IOWR('>', 4, 56 bytes) in the
        // illumos encoding; _IOW('b', 0, 8 bytes).
        assert_eq!(SYNC_IOC_MERGE, 0xc030_3e03);
        assert_eq!(SYNC_IOC_FILE_INFO, 0xc038_3e04);
        assert_eq!(DMA_BUF_IOCTL_SYNC, 0x8008_6200);
    }
}
