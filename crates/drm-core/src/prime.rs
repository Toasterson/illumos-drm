// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! PRIME: GEM objects as dma-buf file descriptors (`PRIME_HANDLE_TO_FD`,
//! `PRIME_FD_TO_HANDLE`), after Linux's `drm_prime.c`.
//!
//! `PRIME_HANDLE_TO_FD` gives the object behind a handle a dma-buf file
//! ([`crate::anon_fd`]): readable, writable too with `DRM_RDWR`,
//! close-on-exec with `DRM_CLOEXEC`; other flags are `EINVAL`, an unknown
//! handle `ENOENT`. While the object's dma-buf file is open, exporting it
//! again opens the same dma-buf (Linux keeps one `dma_buf` per object).
//! The dma-buf keeps the object alive until its last descriptor is closed,
//! and `mmap(2)` on it maps the object from the given offset
//! (`MAP_SHARED` only; `PROT_WRITE` needs `DRM_RDWR`, which the kernel's
//! `mmap(2)` checks against the file's mode); `DMA_BUF_IOCTL_SYNC` is
//! accepted and does nothing, as the memory is coherent.
//!
//! `PRIME_FD_TO_HANDLE` imports a dma-buf of the same device: it returns
//! the handle the calling file already has for the object, or creates one
//! (Linux's same-device fast path). A dma-buf of another device of the
//! same driver is `ENOTSUP`, because importing across devices needs the
//! attach and scatter/gather mapping of a real import, which no driver has
//! yet; a descriptor that is not a dma-buf is `EINVAL`, a closed one
//! `EBADF`. The `flags` field must be 0 (Linux ignores it; nothing else
//! has a meaning).
//!
//! Both ioctls are allowed on render nodes and need no authentication, as
//! in current Linux; a driver without [`Features::GEM`] answers `ENOSYS`,
//! as Linux does without PRIME callbacks.

use ddi::IoctlArg;
use ddi::prelude::*;
use ddi::private::fd::FdFlags;
use drm_uapi as uapi;

use crate::Device;
use crate::anon_fd::AnonObj;
use crate::driver::{Driver, Features};
use crate::fence_dev::SyncCore;
use crate::gem::{GemCore, GemObject, GemRef};
use crate::ioctl::Op;
use crate::karc::KArc;
use crate::minor::CloneId;
use crate::objects::ObjRef;
use crate::uaccess::{ArgIo, CoreArg, Plain, UserArg};

// SAFETY: integer fields only; see the bindings in drm-uapi.
unsafe impl Plain for uapi::drm_prime_handle {}

/// `DRM_CLOEXEC`, `O_CLOEXEC` on illumos (`FCLOEXEC`, 0x800000).
pub const DRM_CLOEXEC: u32 = 0x80_0000;
/// `DRM_RDWR`, `O_RDWR`.
pub const DRM_RDWR: u32 = 2;

/// `ENOSYS`, for a driver without GEM.
const ENOSYS: Errno = match Errno::new(ddi::sys::errno::ENOSYS) {
    Some(e) => e,
    None => Errno::EINVAL,
};

impl<O: GemObject> SyncCore<O> {
    /// Serves `PRIME_HANDLE_TO_FD` and `PRIME_FD_TO_HANDLE` for file `c`.
    pub(crate) fn prime_ioctl<D: Driver<Object = O>>(
        &self,
        op: Op,
        c: CloneId,
        features: Features,
        dev: &Device<D>,
        arg: &CoreArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let gem = match dev.gem() {
            Some(g) if features.contains(Features::GEM) => g,
            _ => return Err(ENOSYS),
        };
        self.housekeeping(ctx);
        let r = match op {
            Op::PrimeHandleToFd => self.handle_to_fd(c, gem, arg, ctx),
            Op::PrimeFdToHandle => self.fd_to_handle(c, dev, gem, arg, ctx),
            _ => Err(Errno::ENOTTY),
        };
        self.publish();
        r
    }

    fn handle_to_fd(
        &self,
        c: CloneId,
        gem: &KArc<GemCore<O>>,
        arg: &CoreArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let mut a = UserArg::<uapi::drm_prime_handle>::read(arg)?;
        let flags = a.get().flags;
        if flags & !(DRM_CLOEXEC | DRM_RDWR) != 0 {
            return Err(Errno::EINVAL);
        }
        let obj = gem.lookup(c, a.get().handle)?;
        let fdf = FdFlags { write: flags & DRM_RDWR != 0, cloexec: flags & DRM_CLOEXEC != 0 };
        self.anon.export(
            &self.dev,
            AnonObj::DmaBuf(obj),
            fdf,
            arg.io(),
            |fd| {
                // SAFETY: a field assignment.
                unsafe { a.get_mut() }.fd = fd;
                a.write(arg)
            },
            &self.domain.stats,
            ctx,
        )
    }

    fn fd_to_handle<D: Driver<Object = O>>(
        &self,
        c: CloneId,
        dev: &Device<D>,
        gem: &KArc<GemCore<O>>,
        arg: &CoreArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let mut a = UserArg::<uapi::drm_prime_handle>::read(arg)?;
        if a.get().flags != 0 {
            return Err(Errno::EINVAL);
        }
        let obj = match self.lookup(a.get().fd, arg.io(), false, ctx)? {
            AnonObj::DmaBuf(g) => g,
            o => {
                o.put(ctx);
                return Err(Errno::EINVAL);
            }
        };
        let obj = if KArc::ptr_eq(obj.core(), gem) {
            obj
        } else {
            // Another device's buffer (R5a: every device is served by the
            // one core module). As Linux's drm_gem_prime_fd_to_handle, a
            // file that imported it before gets its handle again;
            // otherwise the driver makes an object of this device for it
            // (gem_prime_import). Two threads of one file importing the same
            // buffer at once may each make one; Linux serialises that with
            // the file's PRIME lock.
            let mine = gem.handle_find(c, |r| r.object().imported_from().is_some_and(|e| e.same(&obj)));
            if let Some(h) = mine {
                obj.put(ctx);
                // SAFETY: a field assignment.
                unsafe { a.get_mut() }.handle = h;
                return a.write(arg);
            }
            let imported = dev.driver().prime_import(dev, &obj, ctx);
            obj.put(ctx);
            imported?
        };
        let handle = gem.handle_find_or_create(c, obj, ctx)?;
        // SAFETY: a field assignment.
        unsafe { a.get_mut() }.handle = handle;
        a.write(arg)
    }

    /// Hands `obj`'s dma-buf to the calling process as a new descriptor,
    /// from inside an ioctl of this device (the facade's `dma_buf_fd`):
    /// the same dma-buf as `PRIME_HANDLE_TO_FD` makes, `write` for writable
    /// mappings. `write_fd` receives the number before it becomes visible.
    pub fn export_dmabuf(
        &self,
        obj: GemRef<O>,
        write: bool,
        cloexec: bool,
        arg: &IoctlArg<'_>,
        write_fd: impl FnOnce(i32) -> Result<(), Errno>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        self.housekeeping(ctx);
        let fdf = FdFlags { write, cloexec };
        let r = self.anon.export(&self.dev, AnonObj::DmaBuf(obj), fdf, arg, write_fd, &self.domain.stats, ctx);
        self.publish();
        r
    }

    /// The GEM object of dma-buf descriptor `fd` of the calling process, of
    /// any device, from inside an ioctl (the facade's `dma_buf_get`):
    /// `EINVAL` for another kind of file, `EBADF` for a closed descriptor.
    pub fn dmabuf_object(&self, fd: i32, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<GemRef<O>, Errno> {
        match self.lookup(fd, arg, false, ctx)? {
            AnonObj::DmaBuf(g) => Ok(g),
            o => {
                o.put(ctx);
                Err(Errno::EINVAL)
            }
        }
    }
}
