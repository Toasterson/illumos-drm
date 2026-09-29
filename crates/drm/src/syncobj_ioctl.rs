// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The sync object ioctls: `SYNCOBJ_CREATE`, `DESTROY`, `HANDLE_TO_FD`,
//! `FD_TO_HANDLE`, `WAIT`, `RESET`, `SIGNAL`, `TIMELINE_WAIT`, `QUERY`,
//! `TRANSFER` and `TIMELINE_SIGNAL`, with the argument checks and error
//! numbers of Linux's `drm_syncobj.c`:
//!
//! * without [`Features::SYNCOBJ`] every one fails with `EOPNOTSUPP`, and
//!   the timeline ones (`TIMELINE_WAIT`, `QUERY`, `TRANSFER`,
//!   `TIMELINE_SIGNAL`) also without [`Features::SYNCOBJ_TIMELINE`];
//! * unknown flags are `EINVAL`, and so is a count of zero handles;
//!   non-zero `pad` fields are `EINVAL` where Linux checks them
//!   (`DESTROY`, `HANDLE_TO_FD`, `FD_TO_HANDLE`, `RESET`, `SIGNAL`,
//!   `TRANSFER`) and ignored where it does not (`WAIT`, `TIMELINE_WAIT`,
//!   whose `pad` follows the output `first_signaled`); an unknown handle
//!   is `ENOENT` in the array ioctls, `TRANSFER`, `EXPORT_SYNC_FILE` and
//!   `IMPORT_SYNC_FILE`, `EINVAL` in `DESTROY` and plain `HANDLE_TO_FD`;
//! * `FD_TO_HANDLE` accepts sync object files and sync files of any
//!   device of this driver (they share this code), other files are
//!   `EINVAL`;
//! * `TRANSFER` passes its flags to the fence lookup; Linux does not check
//!   them, here any flag but `WAIT_FOR_SUBMIT` is `EINVAL` (nothing else
//!   has a meaning there, and userland passes 0);
//! * sync object files and sync files are close-on-exec descriptors, as
//!   Linux makes them; `HANDLE_TO_FD` without a flag exports the object
//!   itself, with `EXPORT_SYNC_FILE` its current fence (`EINVAL` if it has
//!   none), and `FD_TO_HANDLE` reverses both;
//! * wait timeouts are absolute `CLOCK_MONOTONIC` nanoseconds (the clock
//!   of `gethrtime(9F)`), 0 or a time in the past polls, and an expired
//!   wait is `ETIME`, an interrupted one `EINTR`.
//!
//! Arrays of handles and points are copied in with at most
//! [`MAX_ARRAY`](crate::uaccess::MAX_ARRAY) entries; more is `EINVAL`.

use ddi::prelude::*;
use ddi::private::fd::FdFlags;
use drm_uapi as uapi;

use crate::anon_fd::AnonObj;
use crate::caps::EOPNOTSUPP;
use crate::defer::Ctx;
use crate::driver::Features;
use crate::fence::Fence;
use crate::fence_dev::SyncCore;
use crate::gem::GemObject;
use crate::ioctl::Op;
use crate::kvec::KVec;
use crate::minor::CloneId;
use crate::syncobj::{self, Syncobj, WAIT_ALL, WAIT_AVAILABLE, WAIT_FOR_SUBMIT};
use crate::uaccess::{Plain, UserArg, copy_in_array, copy_out_array};

// SAFETY: integer fields only; see the bindings in drm-uapi.
unsafe impl Plain for uapi::drm_syncobj_create {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_syncobj_destroy {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_syncobj_handle {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_syncobj_transfer {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_syncobj_wait {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_syncobj_timeline_wait {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_syncobj_array {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_syncobj_timeline_array {}

const CREATE_SIGNALED: u32 = uapi::DRM_SYNCOBJ_CREATE_SIGNALED;
const EXPORT_SYNC_FILE: u32 = uapi::DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
const IMPORT_SYNC_FILE: u32 = uapi::DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
const LAST_SUBMITTED: u32 = uapi::DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED;

/// The arguments `WAIT` and `TIMELINE_WAIT` share.
struct WaitArgs {
    handles: u64,
    points: Option<u64>,
    count: u32,
    flags: u32,
    timeout_nsec: i64,
}

impl<O: GemObject> SyncCore<O> {
    /// Serves one of the sync object ioctls for file `c`.
    pub(crate) fn syncobj_ioctl(
        &self,
        op: Op,
        c: CloneId,
        features: Features,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        if !features.contains(Features::SYNCOBJ) {
            return Err(EOPNOTSUPP);
        }
        let timeline =
            matches!(op, Op::SyncobjTimelineWait | Op::SyncobjQuery | Op::SyncobjTransfer | Op::SyncobjTimelineSignal);
        if timeline && !features.contains(Features::SYNCOBJ_TIMELINE) {
            return Err(EOPNOTSUPP);
        }
        self.housekeeping(ctx);
        let r = match op {
            Op::SyncobjCreate => self.so_create(c, arg, ctx),
            Op::SyncobjDestroy => self.so_destroy(c, arg),
            Op::SyncobjHandleToFd => self.so_handle_to_fd(c, arg, ctx),
            Op::SyncobjFdToHandle => self.so_fd_to_handle(c, arg, ctx),
            Op::SyncobjWait => self.so_wait(c, arg, ctx),
            Op::SyncobjTimelineWait => self.so_timeline_wait(c, arg, ctx),
            Op::SyncobjReset => self.so_array(c, arg, ctx, |s, ctx| {
                s.replace_fence(None, ctx);
                Ok(())
            }),
            Op::SyncobjSignal => self.so_array(c, arg, ctx, |s, ctx| s.signal(0, ctx)),
            Op::SyncobjTimelineSignal => self.so_timeline_signal(c, arg, ctx),
            Op::SyncobjQuery => self.so_query(c, arg, ctx),
            Op::SyncobjTransfer => self.so_transfer(c, arg, ctx),
            _ => Err(Errno::ENOTTY),
        };
        self.publish();
        r
    }

    /// Looks up a handle of file `c`; `None` if there is none.
    fn so_find(&self, c: CloneId, handle: u32) -> Result<Option<Syncobj>, Errno> {
        let f = self.file(c)?;
        let t = f.objs.lock();
        Ok(t.get(handle).cloned())
    }

    /// Looks up an array of handles (Linux's `drm_syncobj_array_find`):
    /// `ENOENT` for an unknown one.
    fn so_find_all(&self, c: CloneId, handles: &[u32], ctx: &Sleepable) -> Result<KVec<Syncobj>, Errno> {
        let f = self.file(c)?;
        let mut out = KVec::new();
        let t = f.objs.lock();
        for h in handles {
            out.push(t.get(*h).cloned().ok_or(Errno::ENOENT)?, ctx)?;
        }
        Ok(out)
    }

    /// Creates a handle for `s` in file `c`.
    fn so_handle_create(&self, c: CloneId, s: Syncobj, ctx: &Sleepable) -> Result<u32, Errno> {
        let f = self.file(c)?;
        let r = f.objs.lock().insert(s, ctx);
        r.map_err(|(s, e)| {
            drop(s);
            e
        })
    }

    fn so_create(&self, c: CloneId, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let mut a = UserArg::<uapi::drm_syncobj_create>::read(arg)?;
        let flags = a.get().flags;
        if flags & !CREATE_SIGNALED != 0 {
            return Err(Errno::EINVAL);
        }
        let s = syncobj::create(&self.domain, flags & CREATE_SIGNALED != 0, ctx)?;
        let h = self.so_handle_create(c, s, ctx)?;
        // SAFETY: a field assignment.
        unsafe { a.get_mut() }.handle = h;
        if let Err(e) = a.write(arg) {
            // The caller cannot learn the handle; do not leave it behind.
            let f = self.file(c)?;
            let s = f.objs.lock().remove(h);
            drop(s);
            return Err(e);
        }
        Ok(())
    }

    fn so_destroy(&self, c: CloneId, arg: &IoctlArg<'_>) -> Result<(), Errno> {
        let a = UserArg::<uapi::drm_syncobj_destroy>::read(arg)?;
        if a.get().pad != 0 {
            return Err(Errno::EINVAL);
        }
        let f = self.file(c)?;
        let s = f.objs.lock().remove(a.get().handle).ok_or(Errno::EINVAL)?;
        drop(s);
        Ok(())
    }

    fn so_handle_to_fd(&self, c: CloneId, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let mut a = UserArg::<uapi::drm_syncobj_handle>::read(arg)?;
        let (flags, handle) = (a.get().flags, a.get().handle);
        if a.get().pad != 0 || (flags != 0 && flags != EXPORT_SYNC_FILE) {
            return Err(Errno::EINVAL);
        }
        // Linux: an unknown handle is ENOENT for EXPORT_SYNC_FILE
        // (drm_syncobj_find_fence) and EINVAL for a sync object file.
        let unknown = if flags == EXPORT_SYNC_FILE { Errno::ENOENT } else { Errno::EINVAL };
        let s = self.so_find(c, handle)?.ok_or(unknown)?;
        let write = |fd: i32| {
            // SAFETY: a field assignment.
            unsafe { a.get_mut() }.fd = fd;
            a.write(arg)
        };
        if flags == EXPORT_SYNC_FILE {
            let f = s.find_fence(0, false, ctx);
            drop(s);
            return self.export_sync_file(f?, b"", arg, write, ctx);
        }
        let flags = FdFlags { write: false, cloexec: true };
        self.anon.export(&self.dev, AnonObj::Syncobj(s), flags, arg, write, &self.domain.stats, ctx)
    }

    fn so_fd_to_handle(&self, c: CloneId, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let mut a = UserArg::<uapi::drm_syncobj_handle>::read(arg)?;
        let (flags, fd) = (a.get().flags, a.get().fd);
        if a.get().pad != 0 || (flags != 0 && flags != IMPORT_SYNC_FILE) {
            return Err(Errno::EINVAL);
        }
        if flags == IMPORT_SYNC_FILE {
            let fence =
                self.sync_file_fence(fd, arg, ctx).map_err(|e| if e == Errno::EBADF { Errno::EINVAL } else { e })?;
            let Some(s) = self.so_find(c, a.get().handle)? else {
                fence.put(Ctx::Sleep(ctx));
                return Err(Errno::ENOENT);
            };
            s.replace_fence(Some(fence), ctx);
            return Ok(());
        }
        let s = match self.lookup(fd, arg, false, ctx).map_err(|e| if e == Errno::EBADF { Errno::EINVAL } else { e })? {
            AnonObj::Syncobj(s) => s,
            o => {
                o.put(ctx);
                return Err(Errno::EINVAL);
            }
        };
        let h = self.so_handle_create(c, s, ctx)?;
        // SAFETY: a field assignment.
        unsafe { a.get_mut() }.handle = h;
        if let Err(e) = a.write(arg) {
            let f = self.file(c)?;
            let s = f.objs.lock().remove(h);
            drop(s);
            return Err(e);
        }
        Ok(())
    }

    /// The common part of `WAIT` and `TIMELINE_WAIT`.
    fn so_wait_common(&self, c: CloneId, arg: &IoctlArg<'_>, w: WaitArgs, ctx: &Sleepable) -> Result<u32, Errno> {
        let WaitArgs { handles: handles_addr, points: points_addr, count, flags, timeout_nsec } = w;
        if count == 0 {
            return Err(Errno::EINVAL);
        }
        let n = count as usize;
        let handles: KVec<u32> = copy_in_array(arg, handles_addr as usize, n, ctx)?;
        let points: KVec<u64> = match points_addr {
            Some(p) => copy_in_array(arg, p as usize, n, ctx)?,
            None => {
                let mut v = KVec::new();
                for _ in 0..n {
                    v.push(0u64, ctx)?;
                }
                v
            }
        };
        let objs = self.so_find_all(c, &handles, ctx)?;
        let mut pairs: KVec<(&syncobj::SyncobjInner, u64)> = KVec::new();
        for (s, p) in objs.iter().zip(points.iter()) {
            pairs.push((&**s, *p), ctx)?;
        }
        syncobj::wait_many(&pairs, flags, timeout_nsec, &self.domain, ctx)
    }

    fn so_wait(&self, c: CloneId, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let mut a = UserArg::<uapi::drm_syncobj_wait>::read(arg)?;
        let v = *a.get();
        if v.flags & !(WAIT_ALL | WAIT_FOR_SUBMIT) != 0 {
            return Err(Errno::EINVAL);
        }
        let w = WaitArgs {
            handles: v.handles,
            points: None,
            count: v.count_handles,
            flags: v.flags,
            timeout_nsec: v.timeout_nsec,
        };
        let first = self.so_wait_common(c, arg, w, ctx)?;
        // SAFETY: a field assignment.
        unsafe { a.get_mut() }.first_signaled = first;
        a.write(arg)
    }

    fn so_timeline_wait(&self, c: CloneId, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let mut a = UserArg::<uapi::drm_syncobj_timeline_wait>::read(arg)?;
        let v = *a.get();
        if v.flags & !(WAIT_ALL | WAIT_FOR_SUBMIT | WAIT_AVAILABLE) != 0 {
            return Err(Errno::EINVAL);
        }
        let w = WaitArgs {
            handles: v.handles,
            points: Some(v.points),
            count: v.count_handles,
            flags: v.flags,
            timeout_nsec: v.timeout_nsec,
        };
        let first = self.so_wait_common(c, arg, w, ctx)?;
        // SAFETY: a field assignment.
        unsafe { a.get_mut() }.first_signaled = first;
        a.write(arg)
    }

    /// `RESET` and `SIGNAL`: `f` for every object of the array.
    fn so_array(
        &self,
        c: CloneId,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
        f: impl Fn(&syncobj::SyncobjInner, &Sleepable) -> Result<(), Errno>,
    ) -> Result<(), Errno> {
        let a = UserArg::<uapi::drm_syncobj_array>::read(arg)?;
        let v = *a.get();
        if v.count_handles == 0 || v.pad != 0 {
            return Err(Errno::EINVAL);
        }
        let handles: KVec<u32> = copy_in_array(arg, v.handles as usize, v.count_handles as usize, ctx)?;
        let objs = self.so_find_all(c, &handles, ctx)?;
        for s in objs.iter() {
            f(s, ctx)?;
        }
        Ok(())
    }

    fn so_timeline_signal(&self, c: CloneId, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let a = UserArg::<uapi::drm_syncobj_timeline_array>::read(arg)?;
        let v = *a.get();
        if v.flags != 0 || v.count_handles == 0 {
            return Err(Errno::EINVAL);
        }
        let n = v.count_handles as usize;
        let handles: KVec<u32> = copy_in_array(arg, v.handles as usize, n, ctx)?;
        let points: KVec<u64> = copy_in_array(arg, v.points as usize, n, ctx)?;
        let objs = self.so_find_all(c, &handles, ctx)?;
        for (s, p) in objs.iter().zip(points.iter()) {
            s.signal(*p, ctx)?;
        }
        Ok(())
    }

    fn so_query(&self, c: CloneId, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let a = UserArg::<uapi::drm_syncobj_timeline_array>::read(arg)?;
        let v = *a.get();
        if v.flags & !LAST_SUBMITTED != 0 || v.count_handles == 0 {
            return Err(Errno::EINVAL);
        }
        let n = v.count_handles as usize;
        let handles: KVec<u32> = copy_in_array(arg, v.handles as usize, n, ctx)?;
        let objs = self.so_find_all(c, &handles, ctx)?;
        let mut points: KVec<u64> = KVec::new();
        for s in objs.iter() {
            points.push(s.query(v.flags & LAST_SUBMITTED != 0, ctx), ctx)?;
        }
        copy_out_array(arg, v.points as usize, &points)
    }

    fn so_transfer(&self, c: CloneId, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let a = UserArg::<uapi::drm_syncobj_transfer>::read(arg)?;
        let v = *a.get();
        if v.pad != 0 || v.flags & !WAIT_FOR_SUBMIT != 0 {
            return Err(Errno::EINVAL);
        }
        let src = self.so_find(c, v.src_handle)?.ok_or(Errno::ENOENT)?;
        let dst = self.so_find(c, v.dst_handle)?.ok_or(Errno::ENOENT)?;
        let fence: Fence = src.find_fence(v.src_point, v.flags & WAIT_FOR_SUBMIT != 0, ctx)?;
        drop(src);
        if v.dst_point != 0 {
            dst.add_point(fence, v.dst_point, ctx)
        } else {
            dst.replace_fence(Some(fence), ctx);
            Ok(())
        }
    }
}
