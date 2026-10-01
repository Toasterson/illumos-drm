// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Sync objects and sync files.

use core::ptr::NonNull;

use ddi::Errno;
use drm_facade_sys as sys;

use crate::check;
use crate::device::File;
use crate::fence::Fence;

/// A sync object of a file (`drm_syncobj_find`), dropped with
/// `drm_syncobj_put`. Thread context.
pub struct Syncobj {
    p: NonNull<sys::drm_syncobj>,
}

// SAFETY: the facade's sync-object functions are thread-safe.
unsafe impl Send for Syncobj {}
// SAFETY: as above.
unsafe impl Sync for Syncobj {}

impl Drop for Syncobj {
    fn drop(&mut self) {
        // SAFETY: this handle.
        unsafe { sys::drm_syncobj_put(self.p.as_ptr()) };
    }
}

impl Syncobj {
    /// The sync object of `handle` in `file` (`ENOENT`).
    pub fn find(file: &File, handle: u32) -> Result<Syncobj, Errno> {
        // SAFETY: a live file.
        let p = unsafe { sys::drm_syncobj_find(file.raw(), handle) };
        NonNull::new(p).map(|p| Syncobj { p }).ok_or(Errno::ENOENT)
    }

    /// Replaces the fence (`None` resets).
    pub fn replace_fence(&self, f: Option<&Fence>) {
        let fp = f.map_or(core::ptr::null_mut(), Fence::raw);
        // SAFETY: a live handle and fence.
        unsafe { sys::drm_syncobj_replace_fence(self.p.as_ptr(), fp) };
    }

    /// Adds `f` as timeline point `point` (0: replaces the fence).
    pub fn add_point(&self, f: &Fence, point: u64) {
        // SAFETY: a live handle and fence; the chain token is consumed.
        unsafe {
            let chain = sys::dma_fence_chain_alloc();
            sys::drm_syncobj_add_point(self.p.as_ptr(), chain, f.raw(), point);
        }
    }

    /// The fence of `handle` in `file` at `point` (0: the current one);
    /// `ENOENT` for none or a point not submitted yet (with
    /// `wait_for_submit`, it waits for the submission).
    pub fn find_fence(file: &File, handle: u32, point: u64, wait_for_submit: bool) -> Result<Fence, Errno> {
        let flags = if wait_for_submit { u64::from(drm_uapi::DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) } else { 0 };
        let mut out = core::ptr::null_mut();
        // SAFETY: a live file.
        check(unsafe { sys::drm_syncobj_find_fence(file.raw(), handle, point, flags, &mut out) })?;
        // SAFETY: the fence returned with a reference for us.
        unsafe { Fence::from_owned(out) }.ok_or(Errno::ENOENT)
    }
}

/// Makes a sync file of `f` and returns a new descriptor of the calling
/// process for it (close-on-exec with `cloexec`); inside an ioctl handler.
pub fn sync_file_export(f: &Fence, cloexec: bool) -> Result<i32, Errno> {
    let mut fd = -1;
    // SAFETY: a live fence, inside a handler.
    check(unsafe { sys::drm_sync_file_install(f.raw(), cloexec, &mut fd) })?;
    Ok(fd)
}

/// The fence of sync file `fd` of the calling process: `EBADF` for no
/// such descriptor, `EINVAL` for another kind of file, `ENOTSUP` for a
/// sync file of a device with another fence priority; inside an ioctl
/// handler.
pub fn sync_file_fence(fd: i32) -> Result<Fence, Errno> {
    let mut p = core::ptr::null_mut();
    // SAFETY: inside a handler.
    check(unsafe { sys::drm_illumos_sync_file_get_fence(fd, &mut p) })?;
    // SAFETY: returned with a reference for us.
    unsafe { Fence::from_owned(p) }.ok_or(Errno::EINVAL)
}
