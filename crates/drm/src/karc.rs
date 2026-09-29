// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! A reference-counted value on the kernel heap whose last reference can be
//! handed on instead of dropped.
//!
//! `KArc<T>` is `Arc<T>` for the kernel: a count of references kept next to
//! the value in one `kmem_alloc(9F)` allocation. It differs in three ways.
//!
//! * Construction is fallible and takes the context decision from the
//!   caller: [`KArc::new`] allocates with `KM_SLEEP` and demands the
//!   [`Sleepable`] token, [`KArc::try_new`] allocates with `KM_NOSLEEP`
//!   from any context. There is no global allocator involved.
//! * [`KArc::release`] drops a reference and, if it was the last one,
//!   returns the value still in its allocation as a [`KArcLast`] rather
//!   than dropping it. The caller decides where the value is destroyed: a
//!   caller in interrupt context parks it on a
//!   [`DeferList`](crate::defer::DeferList), which needs no memory, and a
//!   task later drops it in thread context. That is the rule GEM objects
//!   follow, because releasing their memory may block.
//! * [`KArc::upgrade`] takes a new reference from a bare pointer only if
//!   the count is not already zero, the equivalent of Linux's
//!   `kref_get_unless_zero`. Lookup tables that must not keep their
//!   entries alive (the mmap offset table) use it under the lock that the
//!   release path also takes before the memory goes away.
//!
//! Dropping a `KArc` normally is allowed and destroys the value in place if
//! it was the last reference; types whose destruction may block wrap the
//! `KArc` so that an implicit drop cannot happen in the wrong context (see
//! `gem::GemRef`).
//!
//! The count saturates instead of overflowing: a value that somehow gains
//! more than `isize::MAX` references is leaked, never freed early. No
//! operation panics.

use core::marker::PhantomData;
use core::mem::{ManuallyDrop, align_of, offset_of, size_of};
use core::ops::Deref;
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicUsize, Ordering, fence};

use ddi::{Errno, Sleepable};

use crate::kmem::{self, Flag};

/// The allocation: the count and the value.
#[repr(C)]
struct Inner<T> {
    refs: AtomicUsize,
    value: T,
}

/// Counts above this are saturated; the value is then never freed.
const MAX_REFS: usize = isize::MAX as usize;

/// A counted reference to a `T` on the kernel heap. See the module
/// documentation.
pub struct KArc<T> {
    ptr: NonNull<Inner<T>>,
    _owns: PhantomData<Inner<T>>,
}

// SAFETY: as for Arc: the value is shared between threads, so it must be
// Sync, and the last reference may drop it on any thread, so it must be
// Send.
unsafe impl<T: Send + Sync> Send for KArc<T> {}
// SAFETY: as above.
unsafe impl<T: Send + Sync> Sync for KArc<T> {}

impl<T> KArc<T> {
    const LAYOUT_OK: () = assert!(align_of::<Inner<T>>() <= 8, "kmem_alloc(9F) only guarantees 8-byte alignment");

    fn alloc(value: T, flag: Flag) -> Result<KArc<T>, T> {
        let () = Self::LAYOUT_OK;
        let p = kmem::alloc(size_of::<Inner<T>>(), flag) as *mut Inner<T>;
        let Some(ptr) = NonNull::new(p) else { return Err(value) };
        // SAFETY: fresh allocation of the right size and alignment.
        unsafe { ptr.as_ptr().write(Inner { refs: AtomicUsize::new(1), value }) };
        Ok(KArc { ptr, _owns: PhantomData })
    }

    /// Moves `value` to the kernel heap with `KM_SLEEP`. The allocation may
    /// block but does not fail for lack of memory in the kernel; the error
    /// (`ENOMEM`) exists for the host build and drops the value.
    pub fn new(value: T, _ctx: &Sleepable) -> Result<KArc<T>, Errno> {
        Self::alloc(value, Flag::Sleep).map_err(|_| Errno::ENOMEM)
    }

    /// Moves `value` to the kernel heap with `KM_NOSLEEP`, from any
    /// context. Hands the value back when memory is short.
    pub fn try_new(value: T) -> Result<KArc<T>, T> {
        Self::alloc(value, Flag::NoSleep)
    }

    fn inner(&self) -> &Inner<T> {
        // SAFETY: the allocation lives while this reference exists.
        unsafe { self.ptr.as_ref() }
    }

    /// Returns the number of references. Only a snapshot: other threads
    /// may change it at any time.
    pub fn strong_count(this: &KArc<T>) -> usize {
        this.inner().refs.load(Ordering::Acquire)
    }

    /// Reports whether two references point to the same value.
    pub fn ptr_eq(a: &KArc<T>, b: &KArc<T>) -> bool {
        a.ptr == b.ptr
    }

    /// Returns a pointer to the value, for lookup tables that do not own a
    /// reference; see [`KArc::upgrade`]. The pointer does not keep the
    /// value alive.
    pub fn as_ptr(this: &KArc<T>) -> NonNull<T> {
        // SAFETY: a field of a live allocation.
        unsafe { NonNull::new_unchecked(&raw mut (*this.ptr.as_ptr()).value) }
    }

    /// Turns the reference into a pointer to the value without dropping it,
    /// for storage in a C structure; [`KArc::from_raw`] takes it back.
    pub fn into_raw(this: KArc<T>) -> NonNull<T> {
        let p = Self::as_ptr(&this);
        core::mem::forget(this);
        p
    }

    /// Takes back a reference given away with [`KArc::into_raw`].
    ///
    /// # Safety
    /// `p` must come from [`KArc::into_raw`] of the same `T`, and each such
    /// pointer may be taken back only once.
    pub unsafe fn from_raw(p: NonNull<T>) -> KArc<T> {
        // SAFETY: the value is at a fixed offset in the #[repr(C)] Inner.
        let inner = unsafe { p.byte_sub(offset_of!(Inner<T>, value)) }.cast::<Inner<T>>();
        KArc { ptr: inner, _owns: PhantomData }
    }

    /// Takes a new reference to the value at `p` unless its count has
    /// already dropped to zero (Linux's `kref_get_unless_zero`).
    ///
    /// # Safety
    /// `p` must come from [`KArc::as_ptr`] of the same `T`, and the
    /// allocation must still exist: the caller holds a lock that whoever
    /// destroys a value whose count reached zero takes before freeing it.
    pub unsafe fn upgrade(p: NonNull<T>) -> Option<KArc<T>> {
        // SAFETY: as in from_raw; the allocation exists by the contract.
        let inner = unsafe { p.byte_sub(offset_of!(Inner<T>, value)) }.cast::<Inner<T>>();
        // SAFETY: as above.
        let refs = unsafe { &inner.as_ref().refs };
        let mut n = refs.load(Ordering::Relaxed);
        loop {
            if n == 0 {
                return None;
            }
            if n >= MAX_REFS {
                // Saturated: the value is never freed, sharing is safe.
                return Some(KArc { ptr: inner, _owns: PhantomData });
            }
            match refs.compare_exchange_weak(n, n + 1, Ordering::Acquire, Ordering::Relaxed) {
                Ok(_) => return Some(KArc { ptr: inner, _owns: PhantomData }),
                Err(cur) => n = cur,
            }
        }
    }

    /// Drops this reference. If it was the last one, the value is returned
    /// in its allocation, for the caller to destroy where it may.
    pub fn release(this: KArc<T>) -> Option<KArcLast<T>> {
        let this = ManuallyDrop::new(this);
        if this.inner().refs.fetch_sub(1, Ordering::Release) != 1 {
            return None;
        }
        // Pairs with the Release decrements of the other references, so
        // that everything they did with the value happens before its end.
        fence(Ordering::Acquire);
        Some(KArcLast { ptr: this.ptr, _owns: PhantomData })
    }
}

impl<T> Clone for KArc<T> {
    fn clone(&self) -> KArc<T> {
        let old = self.inner().refs.fetch_add(1, Ordering::Relaxed);
        if old >= MAX_REFS {
            // Keep it saturated; the extra references leak the value.
            self.inner().refs.store(MAX_REFS, Ordering::Relaxed);
        }
        KArc { ptr: self.ptr, _owns: PhantomData }
    }
}

impl<T> Deref for KArc<T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.inner().value
    }
}

impl<T> Drop for KArc<T> {
    fn drop(&mut self) {
        let me = KArc { ptr: self.ptr, _owns: PhantomData };
        if let Some(last) = KArc::release(me) {
            drop(last);
        }
    }
}

/// The value of a [`KArc`] whose last reference is gone, still in its
/// allocation. Dropping it destroys the value and frees the memory;
/// [`KArcLast::into_raw`] parks it, for instance on a deferral list.
pub struct KArcLast<T> {
    ptr: NonNull<Inner<T>>,
    _owns: PhantomData<Inner<T>>,
}

// SAFETY: sole owner of the value.
unsafe impl<T: Send> Send for KArcLast<T> {}

impl<T> KArcLast<T> {
    /// Gives up ownership; [`KArcLast::from_raw`] takes it back.
    pub fn into_raw(this: KArcLast<T>) -> NonNull<T> {
        let this = ManuallyDrop::new(this);
        // SAFETY: a field of a live allocation.
        unsafe { NonNull::new_unchecked(&raw mut (*this.ptr.as_ptr()).value) }
    }

    /// Takes back ownership given away with [`KArcLast::into_raw`].
    ///
    /// # Safety
    /// `p` must come from [`KArcLast::into_raw`] of the same `T`, once.
    pub unsafe fn from_raw(p: NonNull<T>) -> KArcLast<T> {
        // SAFETY: as in KArc::from_raw.
        let ptr = unsafe { p.byte_sub(offset_of!(Inner<T>, value)) }.cast::<Inner<T>>();
        KArcLast { ptr, _owns: PhantomData }
    }
}

impl<T> KArcLast<T> {
    /// Returns the value for modification: the last reference is the only
    /// one, so nobody else can see it.
    pub fn get_mut(this: &mut KArcLast<T>) -> &mut T {
        // SAFETY: sole owner of a live allocation.
        unsafe { &mut (*this.ptr.as_ptr()).value }
    }
}

impl<T> Deref for KArcLast<T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: sole owner of a live allocation.
        unsafe { &self.ptr.as_ref().value }
    }
}

impl<T> Drop for KArcLast<T> {
    fn drop(&mut self) {
        // SAFETY: the count is zero and this is the only owner; the value is
        // dropped once and the allocation freed with its size.
        unsafe {
            ptr::drop_in_place(&raw mut (*self.ptr.as_ptr()).value);
            kmem::free(self.ptr.as_ptr() as *mut u8, size_of::<Inner<T>>());
        }
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::sync::atomic::AtomicU32;
    use std::thread;
    use std::vec::Vec;

    fn ctx() -> Sleepable {
        // SAFETY: host tests run in ordinary threads, which may block.
        unsafe { Sleepable::assume() }
    }

    struct Counted<'a>(&'a AtomicU32);
    impl Drop for Counted<'_> {
        fn drop(&mut self) {
            self.0.fetch_add(1, Ordering::SeqCst);
        }
    }

    #[test]
    fn last_reference_is_handed_on() {
        let dropped = AtomicU32::new(0);
        let a = KArc::new(Counted(&dropped), &ctx()).unwrap();
        let b = a.clone();
        assert_eq!(KArc::strong_count(&a), 2);
        assert!(KArc::release(a).is_none());
        let last = KArc::release(b).expect("last reference");
        assert_eq!(dropped.load(Ordering::SeqCst), 0, "not dropped while handed on");
        let p = KArcLast::into_raw(last);
        // SAFETY: from into_raw above, once.
        let last = unsafe { KArcLast::from_raw(p) };
        drop(last);
        assert_eq!(dropped.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn upgrade_fails_at_zero_and_raw_round_trip() {
        let dropped = AtomicU32::new(0);
        let a = KArc::try_new(Counted(&dropped)).ok().unwrap();
        let p = KArc::as_ptr(&a);
        // SAFETY: a keeps the allocation alive.
        let b = unsafe { KArc::upgrade(p) }.unwrap();
        assert!(KArc::ptr_eq(&a, &b));
        let raw = KArc::into_raw(b);
        // SAFETY: from into_raw above, once.
        let b = unsafe { KArc::from_raw(raw) };
        drop(b);
        let last = KArc::release(a).unwrap();
        // The count is zero but the allocation still exists (held by last).
        // SAFETY: last keeps the allocation alive.
        assert!(unsafe { KArc::upgrade(p) }.is_none());
        drop(last);
        assert_eq!(dropped.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn concurrent_clone_and_drop_frees_once() {
        let dropped = AtomicU32::new(0);
        thread::scope(|s| {
            let a = KArc::new(Counted(&dropped), &ctx()).unwrap();
            let hs: Vec<_> = (0..8)
                .map(|_| {
                    let a = a.clone();
                    s.spawn(move || {
                        for _ in 0..10_000 {
                            let b = a.clone();
                            drop(b);
                        }
                        drop(a);
                    })
                })
                .collect();
            drop(a);
            for h in hs {
                h.join().unwrap();
            }
        });
        assert_eq!(dropped.load(Ordering::SeqCst), 1);
    }
}
