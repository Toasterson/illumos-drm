// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `dma_buf.h`: the C handle of a dma-buf. In the core a dma-buf is its GEM
//! object (exported through an anonymous file); a `struct dma_buf` here is
//! a counted handle that holds a reference to that object.

use alloc::boxed::Box;
use core::ptr;
use core::sync::atomic::{AtomicU32, Ordering};

use ddi::Errno;
use drm_core::gem::{GemObject, GemRef};
use drm_facade_sys as f;

use crate::devcore::{DevCore, neg};
use crate::fobj::{FObj, gem_ref};

/// The structure behind a `struct dma_buf *`.
#[repr(C)]
pub(crate) struct DmaBuf {
    buf: f::dma_buf,
    obj: GemRef<FObj>,
    refs: AtomicU32,
}

fn err_ptr<T>(e: Errno) -> *mut T {
    neg(e) as isize as *mut T
}

impl DmaBuf {
    /// A new handle for `obj` with one reference.
    pub(crate) fn make(obj: GemRef<FObj>) -> *mut f::dma_buf {
        let name = obj.object().core().info().name.as_ptr();
        // SAFETY: plain data; zero is valid.
        let mut buf: f::dma_buf = unsafe { core::mem::zeroed() };
        buf.size = obj.object().size();
        buf.priv_ = obj.object().base().cast();
        // SAFETY: the object's C structure is live (obj holds it).
        buf.resv = unsafe { (*obj.object().base()).resv };
        buf.exp_name = name;
        let b = Box::into_raw(Box::new(DmaBuf { buf, obj, refs: AtomicU32::new(1) }));
        // SAFETY: just made.
        unsafe {
            let l = ptr::addr_of_mut!((*b).buf.attachments);
            (*l).next = l;
            (*l).prev = l;
            (*b).buf.__core[0] = b as u64;
            (*b).buf.__core[1] = (b as u64) ^ crate::MAGIC_DMABUF;
        }
        b.cast()
    }
}

/// The handle behind a `struct dma_buf *` the facade made.
///
/// # Safety
/// `buf` is NULL or a handle with a reference the caller holds.
unsafe fn handle<'a>(buf: *const f::dma_buf) -> Option<&'a DmaBuf> {
    if buf.is_null() {
        return None;
    }
    // SAFETY: the caller's contract.
    let (p, check) = unsafe { ((*buf).__core[0], (*buf).__core[1]) };
    if p != buf as u64 || p ^ crate::MAGIC_DMABUF != check {
        return None;
    }
    // SAFETY: the structure is the first field of a live DmaBuf.
    Some(unsafe { &*(buf as *const DmaBuf) })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_gem_prime_export(obj: *mut f::drm_gem_object, _flags: i32) -> *mut f::dma_buf {
    // SAFETY: the caller holds the object.
    let Some(r) = (unsafe { gem_ref(obj) }) else { return err_ptr(Errno::EINVAL) };
    DmaBuf::make(r)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_gem_prime_import(
    dev: *mut f::drm_device,
    dma_buf: *mut f::dma_buf,
) -> *mut f::drm_gem_object {
    // SAFETY: documented for thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller's device and dma-buf.
    let (Some(core), Some(h)) = (unsafe { DevCore::of(dev) }, unsafe { handle(dma_buf) }) else {
        return err_ptr(Errno::EINVAL);
    };
    let Some(gem) = core.device().and_then(|d| d.gem().cloned()) else { return err_ptr(Errno::ENODEV) };
    let r = if drm_core::karc::KArc::ptr_eq(h.obj.core(), &gem) {
        // This device's own buffer: the object itself.
        h.obj.clone()
    } else {
        match crate::fobj::import_object(core, &h.obj, &ctx) {
            Ok(r) => r,
            Err(e) => return err_ptr(e),
        }
    };
    let base = r.object().base();
    // The caller's reference.
    let _ = r.into_raw();
    base
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_buf_get(fd: i32) -> *mut f::dma_buf {
    // SAFETY: documented for ioctl handlers, thread context.
    let ctx = unsafe { super::sleepable() };
    let r = crate::ctxt::with_current(|frame, arg| {
        // SAFETY: the ioctl's device, registered while it runs.
        let core = unsafe { frame.core.as_ref() };
        let d = core.device().ok_or(Errno::ENODEV)?;
        d.sync().dmabuf_object(fd, arg, &ctx)
    });
    match r {
        Some(Ok(obj)) => DmaBuf::make(obj),
        Some(Err(e)) => err_ptr(e),
        None => err_ptr(Errno::EINVAL),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_buf_fd(dmabuf: *mut f::dma_buf, flags: i32) -> i32 {
    // SAFETY: documented for ioctl handlers, thread context.
    let ctx = unsafe { super::sleepable() };
    // SAFETY: the caller holds the handle.
    let Some(h) = (unsafe { handle(dmabuf) }) else { return neg(Errno::EINVAL) };
    let flags = flags as u32;
    let write = flags & drm_core::prime::DRM_RDWR != 0;
    let cloexec = flags & drm_core::prime::DRM_CLOEXEC != 0;
    let r = crate::ctxt::with_current(|_frame, arg| {
        // The dma-buf file belongs to the exporting object's device.
        let core = h.obj.object().core();
        let d = core.device().ok_or(Errno::ENODEV)?;
        let mut out = -1;
        d.sync().export_dmabuf(
            h.obj.clone(),
            write,
            cloexec,
            arg,
            |fd| {
                out = fd;
                Ok(())
            },
            &ctx,
        )?;
        Ok(out)
    });
    match r {
        Some(Ok(fd)) => {
            // As on Linux, the descriptor takes over the caller's
            // reference (its file holds its own object reference).
            // SAFETY: the caller's reference, given up on success.
            unsafe { dma_buf_put(dmabuf) };
            fd
        }
        Some(Err(e)) => neg(e),
        None => neg(Errno::EINVAL),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn get_dma_buf(dmabuf: *mut f::dma_buf) {
    // SAFETY: the caller holds the handle.
    if let Some(h) = unsafe { handle(dmabuf) } {
        h.refs.fetch_add(1, Ordering::Relaxed);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dma_buf_put(dmabuf: *mut f::dma_buf) {
    // SAFETY: the caller gives up its reference.
    let Some(h) = (unsafe { handle(dmabuf) }) else { return };
    if h.refs.fetch_sub(1, Ordering::AcqRel) != 1 {
        return;
    }
    // SAFETY: the last reference: the handle and its object reference go
    // (in the caller's context: the object defers its release if needed).
    let b = unsafe {
        (*dmabuf).__core[0] = 0;
        (*dmabuf).__core[1] = 0;
        Box::from_raw(dmabuf as *mut DmaBuf)
    };
    let DmaBuf { obj, .. } = *b;
    // SAFETY: dma_buf.h: an interrupt handler or a thread that may block.
    unsafe { super::with_ctx(|c| obj.put_in(c)) };
}
