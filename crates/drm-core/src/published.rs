// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! A pointer that is published for concurrent users and withdrawn by its
//! owner, who then waits until no user holds it any more: the registered
//! device of `misc/drm_core`, which facade calls from any context use
//! while its driver may be unregistering it.
//!
//! A user increments the count, then loads the pointer; the owner clears
//! the pointer, then waits for the count to drop to zero. Both pairs are
//! sequentially consistent, so a user either sees the pointer cleared or
//! is counted before the owner's wait reads the count (the store-load
//! pattern needs `SeqCst`; acquire/release alone would let both sides miss
//! each other).

use core::ptr;
use core::sync::atomic::{AtomicPtr, AtomicU32, Ordering};

/// A published `*mut T` and the number of its current users.
pub struct Published<T> {
    p: AtomicPtr<T>,
    users: AtomicU32,
}

impl<T> Default for Published<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T> Published<T> {
    /// Nothing published.
    pub const fn new() -> Published<T> {
        Published { p: AtomicPtr::new(ptr::null_mut()), users: AtomicU32::new(0) }
    }

    /// Publishes `p` (non-null, live until it is withdrawn).
    pub fn publish(&self, p: *mut T) {
        self.p.store(p, Ordering::SeqCst);
    }

    /// The published value, held until the guard is dropped; `None` if
    /// nothing is published. Any context: it neither blocks nor allocates.
    pub fn enter(&self) -> Option<PubGuard<'_, T>> {
        self.users.fetch_add(1, Ordering::SeqCst);
        let p = self.p.load(Ordering::SeqCst);
        if p.is_null() {
            self.users.fetch_sub(1, Ordering::SeqCst);
            return None;
        }
        Some(PubGuard { owner: self, p })
    }

    /// Whether something is published (a snapshot).
    pub fn is_published(&self) -> bool {
        !self.p.load(Ordering::SeqCst).is_null()
    }

    /// Withdraws the published value and returns it once no guard holds it
    /// any more, calling `pause` while waiting (a sleep in thread context).
    /// Null if nothing was published.
    pub fn withdraw(&self, mut pause: impl FnMut()) -> *mut T {
        let p = self.p.swap(ptr::null_mut(), Ordering::SeqCst);
        while self.users.load(Ordering::SeqCst) != 0 {
            pause();
        }
        p
    }
}

/// A use of a [`Published`] value; the owner's `withdraw` waits for it.
pub struct PubGuard<'a, T> {
    owner: &'a Published<T>,
    p: *mut T,
}

impl<T> PubGuard<'_, T> {
    /// The published pointer, valid while the guard lives.
    pub fn get(&self) -> *mut T {
        self.p
    }
}

impl<T> Drop for PubGuard<'_, T> {
    fn drop(&mut self) {
        self.owner.users.fetch_sub(1, Ordering::SeqCst);
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::boxed::Box;
    use std::sync::Arc;
    use std::sync::atomic::{AtomicBool, AtomicUsize};
    use std::thread;
    use std::vec::Vec;

    struct Canary {
        alive: AtomicBool,
    }

    impl Drop for Canary {
        fn drop(&mut self) {
            self.alive.store(false, Ordering::SeqCst);
        }
    }

    /// Eight threads use the value while the owner withdraws and frees it:
    /// no guard ever sees a freed value, and every use after the withdrawal
    /// finds nothing. The freed box is kept (leaked into a list) so that a
    /// use after free reads `alive == false` instead of undefined memory.
    #[test]
    fn withdraw_waits_for_every_user() {
        for _round in 0..50 {
            let pubd = Arc::new(Published::<Canary>::new());
            let p = Box::into_raw(Box::new(Canary { alive: AtomicBool::new(true) }));
            pubd.publish(p);
            let stop = Arc::new(AtomicBool::new(false));
            let seen = Arc::new(AtomicUsize::new(0));
            let mut ts = Vec::new();
            for _ in 0..8 {
                let (pubd, stop, seen) = (pubd.clone(), stop.clone(), seen.clone());
                ts.push(thread::spawn(move || {
                    while !stop.load(Ordering::SeqCst) {
                        if let Some(g) = pubd.enter() {
                            // SAFETY: the guard keeps the value.
                            let c = unsafe { &*g.get() };
                            assert!(c.alive.load(Ordering::SeqCst), "a guard saw a freed value");
                            seen.fetch_add(1, Ordering::Relaxed);
                            thread::yield_now();
                            assert!(c.alive.load(Ordering::SeqCst), "freed under a guard");
                        }
                    }
                }));
            }
            while seen.load(Ordering::Relaxed) < 100 {
                thread::yield_now();
            }
            let w = pubd.withdraw(thread::yield_now);
            assert_eq!(w, p);
            // Free it in place (drop, keep the memory) and check that
            // nobody gets it any more.
            // SAFETY: withdrawn, no guard holds it.
            unsafe { ptr::drop_in_place(w) };
            assert!(pubd.enter().is_none());
            stop.store(true, Ordering::SeqCst);
            for t in ts {
                t.join().unwrap();
            }
            // SAFETY: the memory of the box, its value dropped above.
            unsafe { std::alloc::dealloc(w as *mut u8, std::alloc::Layout::new::<Canary>()) };
        }
    }

    #[test]
    fn nothing_published() {
        let p = Published::<u32>::new();
        assert!(p.enter().is_none());
        assert!(p.withdraw(|| panic!("no user to wait for")).is_null());
        let mut v = 7u32;
        p.publish(&mut v);
        let g = p.enter().unwrap();
        assert_eq!(unsafe { *g.get() }, 7);
        drop(g);
        assert_eq!(p.withdraw(|| panic!("no user left")), &mut v as *mut u32);
    }
}
