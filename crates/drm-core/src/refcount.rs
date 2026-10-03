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

use core::sync::atomic::{AtomicU32, Ordering};

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

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
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
}
