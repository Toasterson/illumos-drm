// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Fences: counted references to the facade's `struct dma_fence`.

use core::ptr::NonNull;

use ddi::Errno;
use drm_facade_sys as sys;

use crate::check_ptr;
use crate::device::{Dev, Driver};

/// A reference to a fence. Cloning takes a reference
/// (`dma_fence_get`), dropping gives it back (`dma_fence_put`), in any
/// context.
pub struct Fence {
    p: NonNull<sys::dma_fence>,
}

// SAFETY: the facade's fence functions are thread-safe and may be called
// from any context up to low-level interrupts.
unsafe impl Send for Fence {}
// SAFETY: as above.
unsafe impl Sync for Fence {}

impl Clone for Fence {
    fn clone(&self) -> Fence {
        // SAFETY: a live fence.
        unsafe { sys::dma_fence_get(self.p.as_ptr()) };
        Fence { p: self.p }
    }
}

impl Drop for Fence {
    fn drop(&mut self) {
        // SAFETY: this reference's count.
        unsafe { sys::dma_fence_put(self.p.as_ptr()) };
    }
}

impl core::fmt::Debug for Fence {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Fence").field("context", &self.context()).field("seqno", &self.seqno()).finish()
    }
}

impl Fence {
    /// A new, unsignalled fence of device `dev`'s fence domain
    /// (`drm_illumos_fence_create`); any context.
    pub fn new<D: Driver>(dev: &Dev<D>, context: u64, seqno: u64) -> Result<Fence, Errno> {
        // SAFETY: a registered device.
        let p = unsafe { sys::drm_illumos_fence_create(dev.raw(), context, seqno) };
        Ok(Fence { p: check_ptr(p)? })
    }

    /// `n` consecutive fence contexts of the device.
    pub fn alloc_contexts<D: Driver>(dev: &Dev<D>, n: u32) -> u64 {
        // SAFETY: a registered device.
        unsafe { sys::drm_illumos_fence_context_alloc(dev.raw(), n) }
    }

    /// Takes over a reference the facade returned.
    ///
    /// # Safety
    /// `p` is a fence with one reference that becomes this value's.
    pub(crate) unsafe fn from_owned(p: *mut sys::dma_fence) -> Option<Fence> {
        NonNull::new(p).map(|p| Fence { p })
    }

    /// The C fence.
    pub fn raw(&self) -> *mut sys::dma_fence {
        self.p.as_ptr()
    }

    /// The fence's context.
    pub fn context(&self) -> u64 {
        // SAFETY: a live fence; the field is fixed.
        unsafe { (*self.p.as_ptr()).context }
    }

    /// The fence's number on its context.
    pub fn seqno(&self) -> u64 {
        // SAFETY: as above.
        unsafe { (*self.p.as_ptr()).seqno }
    }

    /// Signals the fence; any context up to low-level interrupts.
    pub fn signal(&self) {
        // SAFETY: a live fence.
        unsafe { sys::dma_fence_signal(self.p.as_ptr()) };
    }

    /// Records an error, then signals.
    pub fn signal_with_error(&self, e: Errno) {
        self.set_error(e);
        self.signal();
    }

    /// Records an error for a fence that has not signalled.
    pub fn set_error(&self, e: Errno) {
        // SAFETY: a live fence.
        unsafe { sys::dma_fence_set_error(self.p.as_ptr(), -e.get()) };
    }

    /// Whether the fence has signalled.
    pub fn is_signaled(&self) -> bool {
        // SAFETY: a live fence.
        unsafe { sys::dma_fence_is_signaled(self.p.as_ptr()) }
    }

    /// Waits until the fence signals or `timeout_ns` passed (negative: no
    /// limit): `ETIME`, or `EINTR` with `intr`. Thread context.
    pub fn wait(&self, intr: bool, timeout_ns: i64) -> Result<(), Errno> {
        // SAFETY: a live fence.
        let r = unsafe { sys::dma_fence_wait_timeout(self.p.as_ptr(), intr, ns_to_ticks(timeout_ns)) };
        wait_errno(r)
    }

    /// Whether two references name the same C fence.
    pub fn ptr_eq(a: &Fence, b: &Fence) -> bool {
        a.p == b.p
    }
}

/// Clock ticks for a timeout in nanoseconds, rounded up; negative is
/// `MAX_SCHEDULE_TIMEOUT` (no limit).
pub(crate) fn ns_to_ticks(ns: i64) -> i64 {
    if ns < 0 {
        return i64::MAX;
    }
    let us = (ns as u64).div_ceil(1000);
    // SAFETY: drv_usectohz(9F) may be called from any context.
    unsafe { ddi::sys::drv_usectohz(us as ddi::sys::clock_t) as i64 }
}

/// The errno of a Linux-style wait result: remaining time, 0 for a
/// timeout, a negative errno.
pub(crate) fn wait_errno(r: i64) -> Result<(), Errno> {
    if r > 0 {
        Ok(())
    } else if r == 0 {
        Err(Errno::new(ddi::sys::errno::ETIME).unwrap_or(Errno::EINVAL))
    } else {
        Err(Errno::new((-r) as i32).unwrap_or(Errno::EINVAL))
    }
}
