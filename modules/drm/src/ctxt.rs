// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The driver ioctls in progress, by thread.
//!
//! A facade ioctl handler (`drm_ioctl_t`) receives a kernel copy of its
//! argument, as on Linux. The facade functions that need the ioctl itself
//! (`drm_copy_from_user` and `drm_copy_to_user` need its data model and
//! `FKIOCTL` flag; `dma_buf_fd`, `dma_buf_get`, `drm_sync_file_install` and
//! `sync_file_get_fence` need the `IoctlArg` that carries its device number,
//! illumos-ddi's rule for making file descriptors) find it here: the facade
//! driver records a frame for the calling thread around the handler.
//! Ioctls of one file may run on several threads at once, so the table is
//! keyed by the thread (`ddi_get_kt_did(9F)`).

use core::cell::UnsafeCell;
use core::ptr::NonNull;

use ddi::sys;
use ddi::{Errno, IoctlArg, Sleepable};
use drm_core::ksync::KMutex;
use drm_core::kvec::KVec;

use crate::devcore::DevCore;

/// One ioctl in progress.
#[derive(Clone, Copy)]
pub(crate) struct Frame {
    thread: sys::kt_did_t,
    /// The argument of the ioctl, alive while the frame is recorded.
    arg: NonNull<IoctlArg<'static>>,
    /// The device the ioctl is on.
    pub(crate) core: NonNull<DevCore>,
    /// The matched table entry's command and the caller's.
    pub(crate) entry_cmd: u32,
    pub(crate) user_cmd: u32,
}

// SAFETY: the pointers are only followed by the recording thread, while
// the frame is recorded (see `Frame::arg`).
unsafe impl Send for Frame {}

struct Table(UnsafeCell<Option<KMutex<KVec<Frame>>>>);
// SAFETY: set in _init before installation and cleared in _fini after
// removal; read only in between.
unsafe impl Sync for Table {}

static FRAMES: Table = Table(UnsafeCell::new(None));

pub(crate) fn init(_ctx: &Sleepable) -> Result<(), Errno> {
    // SAFETY: called from _init only, before anything else runs.
    unsafe { *FRAMES.0.get() = Some(KMutex::new(KVec::new())) };
    Ok(())
}

pub(crate) fn fini() {
    // SAFETY: called from _init's failure path and _fini after removal.
    unsafe { *FRAMES.0.get() = None };
}

fn table() -> &'static KMutex<KVec<Frame>> {
    // SAFETY: see Table.
    match unsafe { &*FRAMES.0.get() } {
        Some(t) => t,
        // SAFETY: a constant message; unreachable while installed.
        None => unsafe { sys::panic(c"drm: ioctl table used while not installed".as_ptr()) },
    }
}

fn current() -> sys::kt_did_t {
    // SAFETY: ddi_get_kt_did(9F) may be called from any context.
    unsafe { sys::ddi_get_kt_did() }
}

/// Runs `f` with a frame for the calling thread recorded.
pub(crate) fn with_frame<R>(
    core: NonNull<DevCore>,
    cmds: (u32, u32),
    arg: &IoctlArg<'_>,
    ctx: &Sleepable,
    f: impl FnOnce() -> R,
) -> Result<R, Errno> {
    let me = current();
    // SAFETY: the lifetime is erased for the table; the frame is removed
    // before this function returns, while `arg` is still borrowed.
    let arg = unsafe { NonNull::new_unchecked(arg as *const IoctlArg<'_> as *mut IoctlArg<'static>) };
    table().lock().push(Frame { thread: me, arg, core, entry_cmd: cmds.0, user_cmd: cmds.1 }, ctx)?;
    let r = f();
    let mut t = table().lock();
    // The newest frame of this thread is this one (a handler does not
    // start another driver ioctl on the same thread).
    if let Some(i) = t.iter().rposition(|fr| fr.thread == me) {
        t.swap_remove(i);
    }
    Ok(r)
}

/// Runs `f` with the calling thread's ioctl in progress, or returns `None`
/// outside a handler.
pub(crate) fn with_current<R>(f: impl FnOnce(&Frame, &IoctlArg<'_>) -> R) -> Option<R> {
    let me = current();
    let fr = table().lock().iter().rev().find(|fr| fr.thread == me).copied()?;
    // SAFETY: the frame is this thread's and stays recorded until its
    // handler returns, which cannot happen during this call.
    let arg = unsafe { fr.arg.as_ref() };
    Some(f(&fr, arg))
}
