// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The facade's `spinlock_t`, a `kmutex_t` on illumos (`struct spinlock {
//! kmutex_t rlock; }`), for the locks the core keeps inside public
//! structures: a fence's lock and a device's `managed.lock`. In the
//! kernel these are adaptive mutexes, which low-level interrupt threads
//! may take; on the host the mutex word holds the owner's thread id, so
//! the tests can check who holds a lock.
//!
//! # Safety
//! Every function takes a pointer to a lock in memory the caller keeps
//! valid; `init` before any other use and `destroy` after the last, in
//! thread context.

#[cfg(not(test))]
mod imp {
    use core::ptr;
    use ddi::sys;
    use drm_facade_sys as f;

    fn m(l: *mut f::spinlock_t) -> *mut sys::kmutex_t {
        // SAFETY: only computes the field's address.
        unsafe { ptr::addr_of_mut!((*l).rlock) as *mut sys::kmutex_t }
    }
    /// An adaptive mutex: interrupt threads of low-level interrupts may
    /// take it.
    pub(crate) unsafe fn init(l: *mut f::spinlock_t) {
        // SAFETY: the caller's lock, not yet in use.
        unsafe { sys::mutex_init(m(l), ptr::null(), sys::kmutex_type_t::MUTEX_DRIVER, ptr::null_mut()) }
    }
    pub(crate) unsafe fn destroy(l: *mut f::spinlock_t) {
        // SAFETY: the caller's lock, no longer in use; thread context.
        unsafe { sys::mutex_destroy(m(l)) }
    }
    pub(crate) unsafe fn enter(l: *mut f::spinlock_t) {
        // SAFETY: an initialised lock.
        unsafe { sys::mutex_enter(m(l)) }
    }
    pub(crate) unsafe fn exit(l: *mut f::spinlock_t) {
        // SAFETY: held by the caller.
        unsafe { sys::mutex_exit(m(l)) }
    }
    pub(crate) unsafe fn owned(l: *mut f::spinlock_t) -> bool {
        // SAFETY: an initialised lock.
        unsafe { sys::mutex_owned(m(l)) != 0 }
    }
}

/// Host stand-in: the `kmutex_t` word holds the owner's thread id.
#[cfg(test)]
mod imp {
    extern crate std;
    use crate::ksync;
    use core::sync::atomic::{AtomicU64, Ordering};
    use drm_facade_sys as f;

    fn w<'a>(l: *mut f::spinlock_t) -> &'a AtomicU64 {
        // SAFETY: a kmutex_t is one pointer-aligned word.
        unsafe { AtomicU64::from_ptr(core::ptr::addr_of_mut!((*l).rlock) as *mut u64) }
    }
    pub(crate) unsafe fn init(l: *mut f::spinlock_t) {
        w(l).store(0, Ordering::Release);
    }
    pub(crate) unsafe fn destroy(l: *mut f::spinlock_t) {
        assert_eq!(w(l).load(Ordering::Acquire), 0, "destroying a held fence lock");
    }
    pub(crate) unsafe fn enter(l: *mut f::spinlock_t) {
        let me = ksync::thread_id();
        assert_ne!(w(l).load(Ordering::Acquire), me, "recursive fence lock");
        while w(l).compare_exchange_weak(0, me, Ordering::Acquire, Ordering::Relaxed).is_err() {
            std::thread::yield_now();
        }
    }
    pub(crate) unsafe fn exit(l: *mut f::spinlock_t) {
        assert_eq!(w(l).swap(0, Ordering::Release), ksync::thread_id(), "fence lock not held");
    }
    pub(crate) unsafe fn owned(l: *mut f::spinlock_t) -> bool {
        w(l).load(Ordering::Acquire) == ksync::thread_id()
    }
}

pub(crate) use imp::{destroy, enter, exit, init, owned};
