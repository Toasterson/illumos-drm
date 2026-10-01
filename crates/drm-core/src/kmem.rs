// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Raw kernel heap allocation for the containers of this crate.
//!
//! [`crate::kvec::KVec`] and [`crate::karc::KArc`] decide between
//! `KM_SLEEP` and `KM_NOSLEEP` per call, from the context token the caller
//! holds. In host unit tests the memory comes from the Rust global
//! allocator, so that the pure logic built on these containers can be
//! tested with `cargo test`.

/// The allocation flag of one allocation.
#[derive(Clone, Copy)]
pub(crate) enum Flag {
    /// `KM_SLEEP`: may block, needs a `Sleepable` token at the call site.
    Sleep,
    /// `KM_NOSLEEP`: never blocks, may fail.
    NoSleep,
}

#[cfg(not(test))]
mod imp {
    use super::Flag;
    use ddi::sys;

    pub fn alloc(size: usize, flag: Flag) -> *mut u8 {
        let f = match flag {
            Flag::Sleep => sys::KM_SLEEP,
            Flag::NoSleep => sys::KM_NOSLEEP,
        };
        // SAFETY: kmem_alloc(9F) may be called with KM_SLEEP from thread
        // context, which the Sleepable token of the caller proves, and with
        // KM_NOSLEEP from any context.
        unsafe { sys::kmem_alloc(size, f) as *mut u8 }
    }

    /// # Safety
    /// `p` must come from [`alloc()`] with the same `size`.
    pub unsafe fn free(p: *mut u8, size: usize) {
        // SAFETY: the caller's contract; kmem_free(9F) may be called from
        // any context, interrupt context included.
        unsafe { sys::kmem_free(p as *mut sys::c_void, size) }
    }
}

#[cfg(test)]
mod imp {
    extern crate std;
    use super::Flag;
    use std::alloc::Layout;

    pub fn alloc(size: usize, _flag: Flag) -> *mut u8 {
        match Layout::from_size_align(size, 8) {
            // SAFETY: size is non-zero; callers never allocate zero bytes.
            Ok(l) => unsafe { std::alloc::alloc(l) },
            Err(_) => core::ptr::null_mut(),
        }
    }

    /// # Safety
    /// `p` must come from [`alloc()`] with the same `size`.
    pub unsafe fn free(p: *mut u8, size: usize) {
        if let Ok(l) = Layout::from_size_align(size, 8) {
            // SAFETY: the caller's contract.
            unsafe { std::alloc::dealloc(p, l) }
        }
    }
}

/// Allocates `size` bytes, 8-byte aligned, or returns null. `size` must not
/// be zero.
pub(crate) fn alloc(size: usize, flag: Flag) -> *mut u8 {
    imp::alloc(size, flag)
}

/// Frees memory from [`alloc()`].
///
/// # Safety
/// `p` must come from [`alloc()`] with the same `size` and must not be used
/// afterwards.
pub(crate) unsafe fn free(p: *mut u8, size: usize) {
    // SAFETY: the caller's contract.
    unsafe { imp::free(p, size) }
}
