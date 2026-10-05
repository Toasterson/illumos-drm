// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! A reference count kept in a C structure: Linux's `struct kref` inside a
//! driver's `struct drm_gem_object` (`refcount`), which Linux drivers read
//! (`kref_read`) and take from lists that hold no reference
//! (`kref_get_unless_zero`). When an object's count lives there, it is
//! the object's only count (see `gem::GemObject::ext_count`), so these are
//! the operations every reference goes through: an increment from a
//! reference held, an increment unless zero, and a decrement that tells
//! the caller when it dropped the last one. The count never comes back
//! from zero, which is what makes `get_unless_zero` safe on an object that
//! is being destroyed.

use core::ptr::NonNull;
use core::sync::atomic::{AtomicU32, Ordering};

use crate::karc::{KArc, KArcLast};

/// What [`dec`] did.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Dec {
    /// Other references remain.
    Alive,
    /// That was the last reference: the caller destroys the object.
    Last,
    /// The count was zero already (a caller dropped a reference it did not
    /// hold); nothing changed.
    Underflow,
}

/// Takes a reference; the caller holds one. Saturates at `u32::MAX`
/// rather than wrap (the object then lives for ever).
pub fn inc(c: &AtomicU32) {
    let mut n = c.load(Ordering::Relaxed);
    loop {
        if n == u32::MAX {
            return;
        }
        match c.compare_exchange_weak(n, n + 1, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => return,
            Err(m) => n = m,
        }
    }
}

/// Takes a reference unless the count is zero (Linux's
/// `kref_get_unless_zero`).
pub fn inc_not_zero(c: &AtomicU32) -> bool {
    let mut n = c.load(Ordering::Acquire);
    loop {
        if n == 0 {
            return false;
        }
        if n == u32::MAX {
            return true;
        }
        match c.compare_exchange_weak(n, n + 1, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => return true,
            Err(m) => n = m,
        }
    }
}

/// Drops a reference. A saturated count stays.
pub fn dec(c: &AtomicU32) -> Dec {
    let mut n = c.load(Ordering::Acquire);
    loop {
        match n {
            0 => return Dec::Underflow,
            u32::MAX => return Dec::Alive,
            _ => {}
        }
        match c.compare_exchange_weak(n, n - 1, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => return if n == 1 { Dec::Last } else { Dec::Alive },
            Err(m) => n = m,
        }
    }
}

/// A value whose references may be counted outside its [`KArc`] (GEM
/// objects: `gem::GemObject::ext_count`). With an external count, every
/// reference counts there, and the `KArc` holds the value with one
/// reference of its own until that count reaches zero: the functions below
/// keep the two in step, and a value without one is a plain `KArc`.
pub trait ExtCounted {
    /// The external count, the same for the value's life; 1 when the value
    /// is created.
    fn ext_count(&self) -> Option<&AtomicU32>;
}

/// Another reference to `a`'s value.
pub fn clone_ref<T: ExtCounted>(a: &KArc<T>) -> KArc<T> {
    match a.ext_count() {
        Some(c) => {
            inc(c);
            // SAFETY: the allocation stays while the external count is not
            // zero, and the reference just counted is one of it.
            unsafe { KArc::from_raw(KArc::as_ptr(a)) }
        }
        None => KArc::clone(a),
    }
}

/// Drops the reference `a`; returns the value when it was the last. A
/// reference dropped twice (the count was zero already) leaves the value
/// alone.
pub fn release<T: ExtCounted>(a: KArc<T>) -> Option<KArcLast<T>> {
    let Some(c) = a.ext_count() else { return KArc::release(a) };
    match dec(c) {
        // The allocation's own reference, the only one the KArc counted.
        Dec::Last => KArc::release(a),
        Dec::Alive | Dec::Underflow => {
            core::mem::forget(a);
            None
        }
    }
}

/// A new reference to the value at `p` unless its count is zero (it is
/// being destroyed).
///
/// # Safety
/// `p` is a value of a `KArc` that the caller keeps from being freed (a
/// table lock that the value's destruction takes before the memory goes).
pub unsafe fn upgrade<T: ExtCounted>(p: NonNull<T>) -> Option<KArc<T>> {
    // SAFETY: the caller's contract.
    let v = unsafe { p.as_ref() };
    match v.ext_count() {
        // SAFETY: as above; the reference taken is the value's own.
        Some(c) => inc_not_zero(c).then(|| unsafe { KArc::from_raw(p) }),
        // SAFETY: as above.
        None => unsafe { KArc::upgrade(p) },
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use core::ptr::NonNull;
    use std::sync::atomic::AtomicBool;
    use std::thread;
    use std::vec::Vec;

    #[test]
    fn counts_and_refuses_to_come_back() {
        let c = AtomicU32::new(1);
        inc(&c);
        assert!(inc_not_zero(&c));
        assert_eq!(dec(&c), Dec::Alive);
        assert_eq!(dec(&c), Dec::Alive);
        assert_eq!(dec(&c), Dec::Last);
        assert!(!inc_not_zero(&c));
        assert_eq!(dec(&c), Dec::Underflow);
        assert_eq!(c.load(Ordering::SeqCst), 0);
        let s = AtomicU32::new(u32::MAX);
        inc(&s);
        assert_eq!(dec(&s), Dec::Alive);
        assert_eq!(s.load(Ordering::SeqCst), u32::MAX);
    }

    /// Threads that hold references take and drop more, threads that hold
    /// none try `inc_not_zero` (a driver's list walk): exactly one drop is
    /// the last, and no `inc_not_zero` succeeds after it.
    #[test]
    fn exactly_one_last_and_no_resurrection() {
        for _ in 0..200 {
            let c: &'static AtomicU32 = std::boxed::Box::leak(std::boxed::Box::new(AtomicU32::new(4)));
            let dead: &'static AtomicBool = std::boxed::Box::leak(std::boxed::Box::new(AtomicBool::new(false)));
            let lasts: &'static AtomicU32 = std::boxed::Box::leak(std::boxed::Box::new(AtomicU32::new(0)));
            let mut t: Vec<_> = (0..4)
                .map(|_| {
                    thread::spawn(move || {
                        for _ in 0..100 {
                            inc(c);
                            assert_eq!(dec(c), Dec::Alive);
                        }
                        if dec(c) == Dec::Last {
                            dead.store(true, Ordering::SeqCst);
                            lasts.fetch_add(1, Ordering::SeqCst);
                        }
                    })
                })
                .collect();
            t.extend((0..2).map(|_| {
                thread::spawn(move || {
                    for _ in 0..100 {
                        if inc_not_zero(c) {
                            assert!(!dead.load(Ordering::SeqCst), "a reference after the last");
                            if dec(c) == Dec::Last {
                                dead.store(true, Ordering::SeqCst);
                                lasts.fetch_add(1, Ordering::SeqCst);
                            }
                        }
                    }
                })
            }));
            for h in t {
                h.join().unwrap();
            }
            assert_eq!(lasts.load(Ordering::SeqCst), 1);
            assert_eq!(c.load(Ordering::SeqCst), 0);
        }
    }

    /// A value with an external count, whose destruction takes the table
    /// lock first (as a GEM object's removes its mmap offset).
    struct Obj {
        count: AtomicU32,
        external: bool,
        table: &'static std::sync::Mutex<Option<usize>>,
        dropped: &'static AtomicU32,
    }
    impl ExtCounted for Obj {
        fn ext_count(&self) -> Option<&AtomicU32> {
            self.external.then_some(&self.count)
        }
    }
    impl Drop for Obj {
        fn drop(&mut self) {
            if self.external {
                assert_eq!(self.count.load(Ordering::SeqCst), 0, "destroyed with references");
            }
            *self.table.lock().unwrap() = None;
            self.dropped.fetch_add(1, Ordering::SeqCst);
        }
    }

    fn leak<T>(v: T) -> &'static T {
        std::boxed::Box::leak(std::boxed::Box::new(v))
    }

    fn ctx() -> ddi::Sleepable {
        // SAFETY: host test threads may block.
        unsafe { ddi::Sleepable::assume() }
    }

    /// Clones and puts across threads while other threads upgrade from a
    /// table under its lock: the value is destroyed exactly once, after
    /// the last reference, and no upgrade succeeds once the count was
    /// zero (the destructor asserts the count; an upgrade after it would
    /// find no table entry). Both kinds of value.
    #[test]
    fn external_count_clone_upgrade_and_last_put() {
        let mut upgrades = 0u32;
        for external in [true, false] {
            for _ in 0..300 {
                let table: &'static std::sync::Mutex<Option<usize>> = leak(std::sync::Mutex::new(None));
                let dropped: &'static AtomicU32 = leak(AtomicU32::new(0));
                let a = KArc::new(Obj { count: AtomicU32::new(1), external, table, dropped }, &ctx()).ok().unwrap();
                *table.lock().unwrap() = Some(KArc::as_ptr(&a).as_ptr() as usize);
                let refs: Vec<KArc<Obj>> = (0..3).map(|_| clone_ref(&a)).collect();
                if external {
                    assert_eq!(a.count.load(Ordering::SeqCst), 4);
                    assert_eq!(KArc::strong_count(&a), 1, "the KArc holds one reference only");
                }
                let t: Vec<_> = refs
                    .into_iter()
                    .map(|r| {
                        thread::spawn(move || {
                            for _ in 0..50 {
                                let c = clone_ref(&r);
                                assert!(release(c).is_none());
                            }
                            if let Some(last) = release(r) {
                                drop(last);
                            }
                        })
                    })
                    .collect();
                let up: Vec<_> = (0..2)
                    .map(|_| {
                        thread::spawn(move || {
                            let mut got = 0u32;
                            for _ in 0..100 {
                                let g = table.lock().unwrap();
                                let u = g.and_then(|p| {
                                    // SAFETY: the table entry is cleared by
                                    // the destructor under this lock before
                                    // the memory goes.
                                    unsafe { upgrade(NonNull::new(p as *mut Obj).unwrap()) }
                                });
                                drop(g);
                                if let Some(u) = u {
                                    got += 1;
                                    if let Some(last) = release(u) {
                                        drop(last);
                                    }
                                }
                            }
                            got
                        })
                    })
                    .collect();
                if let Some(last) = release(a) {
                    drop(last);
                }
                for h in t {
                    h.join().unwrap();
                }
                for h in up {
                    upgrades += h.join().unwrap();
                }
                assert_eq!(dropped.load(Ordering::SeqCst), 1, "destroyed exactly once");
                assert!(table.lock().unwrap().is_none());
            }
        }
        assert!(upgrades > 0, "upgrades raced the puts");
    }

    /// An extra put of an external count leaves the value alone.
    #[test]
    fn external_underflow_is_ignored() {
        let table: &'static std::sync::Mutex<Option<usize>> = leak(std::sync::Mutex::new(None));
        let dropped: &'static AtomicU32 = leak(AtomicU32::new(0));
        let a = KArc::new(Obj { count: AtomicU32::new(1), external: true, table, dropped }, &ctx()).ok().unwrap();
        let p = KArc::as_ptr(&a);
        let extra = clone_ref(&a);
        assert!(release(extra).is_none());
        // A driver's put without a reference: the count goes to 0 through
        // the KArc of `a`'s value, then a second put underflows.
        // SAFETY: a counterfeit reference to the live value, as a C driver's
        // stray drm_gem_object_put() makes one.
        let stray = unsafe { KArc::from_raw(p) };
        a.count.fetch_add(1, Ordering::SeqCst);
        assert!(release(stray).is_none());
        a.count.store(0, Ordering::SeqCst);
        // SAFETY: as above.
        let stray = unsafe { KArc::from_raw(p) };
        assert!(release(stray).is_none(), "an underflow releases nothing");
        assert_eq!(dropped.load(Ordering::SeqCst), 0);
        a.count.store(1, Ordering::SeqCst);
        drop(release(a));
        assert_eq!(dropped.load(Ordering::SeqCst), 1);
    }
}
