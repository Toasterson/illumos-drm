// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Wound/wait mutexes: locking several objects in any order without
//! deadlock (Linux's `ww_mutex`).
//!
//! A job that touches several buffers must lock all their reservations
//! ([`crate::resv::Resv`]) at once, and two jobs may name the same buffers
//! in different orders. A [`WwAcquireCtx`] gives a locking transaction a
//! stamp from its [`WwClass`], older transactions having smaller stamps,
//! and [`WwMutex::lock`] applies the wait-die rule that Linux's reservation
//! objects use (`DEFINE_WD_CLASS(reservation_ww_class)`):
//!
//! * a transaction that asks for a mutex held by a *younger* transaction,
//!   or by a caller without a transaction, waits;
//! * a transaction that asks for a mutex held by an *older* transaction
//!   and already holds another mutex of the class "dies": the call fails
//!   with `EDEADLK`, the caller releases everything it holds, sleeps on the
//!   contended mutex with [`WwMutex::lock_slow`], and starts over, keeping
//!   its stamp so that it grows older relative to newcomers and eventually
//!   wins.
//!
//! Waits therefore only go from older to younger holders, which cannot
//! form a cycle. A transaction that holds nothing never dies. Asking again
//! for a mutex the transaction holds fails with `EALREADY`, as in Linux,
//! which lets callers ignore duplicates in their object lists. The rule is
//! re-evaluated every time a waiter wakes, since the holder may have
//! changed. [`lock_all`] implements the standard back-off loop.
//!
//! The mutexes block, so every lock takes the [`Sleepable`] token; the
//! interruptible variants fail with `EINTR` when a signal is pending. The
//! guards release on drop. A transaction is tied to its thread (neither
//! `Send` nor `Sync`), like the guards.
//!
//! The rules are tested on the host with threads, among them two
//! transactions that lock two mutexes in opposite orders at the same time.

use core::cell::{Cell, UnsafeCell};
use core::marker::PhantomData;
use core::ops::{Deref, DerefMut};
use core::sync::atomic::{AtomicU64, Ordering};

use ddi::{Errno, Sleepable};

use crate::ksync::{KCondVar, KMutex, Wake};
use crate::kvec::KVec;

/// `EDEADLK`: back off and retry. `ddi::Errno` has no named constant.
pub const EDEADLK: Errno = match Errno::new(ddi::sys::errno::EDEADLK) {
    Some(e) => e,
    None => Errno::EINVAL,
};
/// `EALREADY`: the transaction holds this mutex already.
pub const EALREADY: Errno = match Errno::new(ddi::sys::errno::EALREADY) {
    Some(e) => e,
    None => Errno::EINVAL,
};

/// A class of wound/wait mutexes: the source of transaction stamps. All
/// mutexes locked together must belong to one class.
pub struct WwClass {
    next: AtomicU64,
}

impl WwClass {
    /// A new class.
    pub const fn new() -> WwClass {
        WwClass { next: AtomicU64::new(1) }
    }

    /// A new transaction stamp of the class, for a transaction a C caller
    /// keeps itself ([`WwMutex::lock_raw_ctx`]); smaller is older.
    pub fn next_stamp(&self) -> u64 {
        self.next.fetch_add(1, Ordering::Relaxed)
    }
}

impl Default for WwClass {
    fn default() -> Self {
        Self::new()
    }
}

/// The class of the reservation objects of this module.
pub static RESERVATION_CLASS: WwClass = WwClass::new();

/// A locking transaction (Linux's `ww_acquire_ctx`). See the module
/// documentation.
pub struct WwAcquireCtx {
    class: &'static WwClass,
    stamp: u64,
    acquired: Cell<u32>,
    _thread: PhantomData<*mut ()>,
}

impl WwAcquireCtx {
    /// Starts a transaction with a new stamp of `class`.
    pub fn new(class: &'static WwClass) -> WwAcquireCtx {
        WwAcquireCtx { class, stamp: class.next_stamp(), acquired: Cell::new(0), _thread: PhantomData }
    }
    /// Returns the stamp; smaller is older.
    pub fn stamp(&self) -> u64 {
        self.stamp
    }
    /// Returns the number of mutexes the transaction holds.
    pub fn acquired(&self) -> u32 {
        self.acquired.get()
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Owner {
    Free,
    /// Held without a transaction.
    Plain,
    /// Held without a transaction and without a guard by the thread with
    /// this id (`lock_raw`, the facade's `dma_resv_lock`).
    Raw(u64),
    /// Held by the transaction with this stamp.
    Ctx(u64),
    /// Held without a guard by a C transaction (the facade's
    /// `drm_illumos_resv_lock_ctx`) with this stamp, on the thread with
    /// this id.
    RawCtx(u64, u64),
}

/// A wound/wait mutex of one [`WwClass`] guarding a `T`.
pub struct WwMutex<T> {
    class: &'static WwClass,
    owner: KMutex<Owner>,
    cv: KCondVar,
    data: UnsafeCell<T>,
}

// SAFETY: the data is reached only through a guard, one at a time.
unsafe impl<T: Send> Send for WwMutex<T> {}
// SAFETY: as above.
unsafe impl<T: Send> Sync for WwMutex<T> {}

/// The guard of a held [`WwMutex`]; releases it when dropped.
pub struct WwGuard<'a, T> {
    m: &'a WwMutex<T>,
    ctx: Option<&'a WwAcquireCtx>,
    _thread: PhantomData<*mut ()>,
}

impl<T> WwMutex<T> {
    /// A new, unlocked mutex of `class`. Thread context only: the mutex
    /// blocks.
    pub fn new(value: T, class: &'static WwClass) -> WwMutex<T> {
        WwMutex { class, owner: KMutex::new(Owner::Free), cv: KCondVar::new(), data: UnsafeCell::new(value) }
    }

    fn acquire(&self, ctx: Option<&WwAcquireCtx>, slow: bool, intr: bool, sctx: &Sleepable) -> Result<(), Errno> {
        if let Some(c) = ctx
            && !core::ptr::eq(c.class, self.class)
        {
            return Err(Errno::EINVAL);
        }
        let mut g = self.owner.lock();
        loop {
            match (*g, ctx) {
                (Owner::Free, None) => {
                    *g = Owner::Plain;
                    return Ok(());
                }
                (Owner::Free, Some(c)) => {
                    *g = Owner::Ctx(c.stamp);
                    c.acquired.set(c.acquired.get() + 1);
                    return Ok(());
                }
                (Owner::Ctx(s) | Owner::RawCtx(s, _), Some(c)) if s == c.stamp => return Err(EALREADY),
                // Wait-die: a younger transaction that holds locks backs off
                // from an older holder. lock_slow is called holding nothing.
                (Owner::Ctx(s) | Owner::RawCtx(s, _), Some(c)) if !slow && c.acquired.get() > 0 && s < c.stamp => {
                    return Err(EDEADLK);
                }
                _ => {}
            }
            let (ng, w) = self.cv.wait(g, None, intr, sctx);
            g = ng;
            if w == Wake::Interrupted {
                return Err(Errno::EINTR);
            }
        }
    }

    /// Locks the mutex without a transaction; waits for any holder.
    pub fn lock_plain(&self, ctx: &Sleepable) -> WwGuard<'_, T> {
        // Cannot fail: no transaction, not interruptible.
        let _ = self.acquire(None, false, false, ctx);
        WwGuard { m: self, ctx: None, _thread: PhantomData }
    }

    /// Locks the mutex in transaction `tx`: `EDEADLK` to back off,
    /// `EALREADY` if `tx` holds it, `EINVAL` for a mutex of another class.
    pub fn lock<'a>(&'a self, tx: &'a WwAcquireCtx, ctx: &Sleepable) -> Result<WwGuard<'a, T>, Errno> {
        self.acquire(Some(tx), false, false, ctx)?;
        Ok(WwGuard { m: self, ctx: Some(tx), _thread: PhantomData })
    }

    /// As [`WwMutex::lock`], but a pending signal ends the wait with `EINTR`.
    pub fn lock_interruptible<'a>(&'a self, tx: &'a WwAcquireCtx, ctx: &Sleepable) -> Result<WwGuard<'a, T>, Errno> {
        self.acquire(Some(tx), false, true, ctx)?;
        Ok(WwGuard { m: self, ctx: Some(tx), _thread: PhantomData })
    }

    /// Sleeps until the mutex is free and locks it, after `EDEADLK` and
    /// releasing everything else (Linux's `ww_mutex_lock_slow`): `EINVAL`
    /// if `tx` still holds a mutex, since only a transaction that holds
    /// nothing may wait for an older one. `intr` makes the wait
    /// interruptible (`EINTR`).
    pub fn lock_slow<'a>(&'a self, tx: &'a WwAcquireCtx, intr: bool, ctx: &Sleepable) -> Result<WwGuard<'a, T>, Errno> {
        if tx.acquired.get() != 0 {
            return Err(Errno::EINVAL);
        }
        self.acquire(Some(tx), true, intr, ctx)?;
        Ok(WwGuard { m: self, ctx: Some(tx), _thread: PhantomData })
    }

    /// Acquires the mutex without a transaction and without a guard, for a
    /// caller whose unlock comes in a separate call (the facade's
    /// `dma_resv_lock`). `EINTR` if `intr` and a signal arrives while
    /// waiting.
    /// The mutex records the calling thread as its holder: only that
    /// thread may [`unlock_raw`](WwMutex::unlock_raw) it, and it gets
    /// `EDEADLK` instead of waiting for itself when it locks it again.
    pub fn lock_raw(&self, intr: bool, ctx: &Sleepable) -> Result<(), Errno> {
        let me = crate::ksync::thread_id();
        if matches!(*self.owner.lock(), Owner::Raw(t) | Owner::RawCtx(_, t) if t == me) {
            return Err(EDEADLK);
        }
        self.acquire(None, false, intr, ctx)?;
        // Held: acquire set Plain, and nobody else changes a held owner.
        *self.owner.lock() = Owner::Raw(me);
        Ok(())
    }

    /// Releases a mutex the calling thread took with
    /// [`WwMutex::lock_raw`]; `EPERM` (and nothing changes) if it does not
    /// hold it that way, whoever else holds it.
    pub fn unlock_raw(&self) -> Result<(), Errno> {
        let mut g = self.owner.lock();
        if *g != Owner::Raw(crate::ksync::thread_id()) {
            return Err(Errno::EPERM);
        }
        *g = Owner::Free;
        drop(g);
        self.cv.notify_all();
        Ok(())
    }

    /// Takes the mutex without waiting, without a transaction and without
    /// a guard, as [`WwMutex::lock_raw`] (Linux's `dma_resv_trylock`):
    /// false if anyone holds it.
    pub fn try_lock_raw(&self) -> bool {
        let mut g = self.owner.lock();
        if *g != Owner::Free {
            return false;
        }
        *g = Owner::Raw(crate::ksync::thread_id());
        true
    }

    /// Locks the mutex for a transaction that a C caller keeps (a
    /// compatibility layer's `ww_acquire_ctx`): the wait-die rule of
    /// [`WwMutex::lock`] with the transaction's `stamp` (from
    /// [`WwClass::next_stamp`] of this mutex's class) and the number of
    /// mutexes it holds, `acquired`, which the caller counts. `slow` is
    /// [`WwMutex::lock_slow`] (`EINVAL` unless `acquired` is 0). `EDEADLK`
    /// to back off, `EALREADY` if the transaction holds it, `EINTR`.
    /// The mutex is then held by the calling thread for that transaction:
    /// it releases it with [`WwMutex::unlock_raw_ctx`], and
    /// [`WwMutex::held_raw_by_current`] is true meanwhile.
    pub fn lock_raw_ctx(
        &self,
        stamp: u64,
        acquired: u32,
        slow: bool,
        intr: bool,
        sctx: &Sleepable,
    ) -> Result<(), Errno> {
        if slow && acquired != 0 {
            return Err(Errno::EINVAL);
        }
        let me = crate::ksync::thread_id();
        let mut g = self.owner.lock();
        loop {
            match *g {
                Owner::Free => {
                    *g = Owner::RawCtx(stamp, me);
                    return Ok(());
                }
                Owner::Ctx(s) | Owner::RawCtx(s, _) if s == stamp => return Err(EALREADY),
                Owner::Ctx(s) | Owner::RawCtx(s, _) if !slow && acquired > 0 && s < stamp => return Err(EDEADLK),
                Owner::Raw(t) if t == me => return Err(EDEADLK),
                _ => {}
            }
            let (ng, w) = self.cv.wait(g, None, intr, sctx);
            g = ng;
            if w == Wake::Interrupted {
                return Err(Errno::EINTR);
            }
        }
    }

    /// Takes the mutex for the C transaction `stamp` without waiting (Linux's
    /// `ww_mutex_trylock` with a context): false if anyone holds it.
    pub fn try_lock_raw_ctx(&self, stamp: u64) -> bool {
        let mut g = self.owner.lock();
        if *g != Owner::Free {
            return false;
        }
        *g = Owner::RawCtx(stamp, crate::ksync::thread_id());
        true
    }

    /// Releases a mutex the calling thread took with
    /// [`WwMutex::lock_raw_ctx`] for the transaction `stamp`; `EPERM` (and
    /// nothing changes) otherwise.
    pub fn unlock_raw_ctx(&self, stamp: u64) -> Result<(), Errno> {
        let mut g = self.owner.lock();
        if *g != Owner::RawCtx(stamp, crate::ksync::thread_id()) {
            return Err(Errno::EPERM);
        }
        *g = Owner::Free;
        drop(g);
        self.cv.notify_all();
        Ok(())
    }

    /// Reports whether the calling thread holds the mutex through
    /// [`WwMutex::lock_raw`] or [`WwMutex::lock_raw_ctx`].
    pub fn held_raw_by_current(&self) -> bool {
        let me = crate::ksync::thread_id();
        matches!(*self.owner.lock(), Owner::Raw(t) | Owner::RawCtx(_, t) if t == me)
    }

    /// Reports whether the mutex is held by anyone.
    pub fn is_locked(&self) -> bool {
        *self.owner.lock() != Owner::Free
    }

    /// Reports whether the guard belongs to this mutex.
    pub fn owns<U>(&self, g: &WwGuard<'_, U>) -> bool {
        core::ptr::eq(g.m as *const WwMutex<U> as *const u8, self as *const WwMutex<T> as *const u8)
    }
}

impl<T> Deref for WwGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: the guard holds the mutex.
        unsafe { &*self.m.data.get() }
    }
}

impl<T> DerefMut for WwGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: the guard holds the mutex exclusively.
        unsafe { &mut *self.m.data.get() }
    }
}

impl<T> Drop for WwGuard<'_, T> {
    fn drop(&mut self) {
        let mut g = self.m.owner.lock();
        *g = Owner::Free;
        if let Some(c) = self.ctx {
            c.acquired.set(c.acquired.get().saturating_sub(1));
        }
        self.m.cv.notify_all();
    }
}

/// Locks every mutex of `ms` in transaction `tx`, backing off and retrying
/// on `EDEADLK` as the module documentation describes. Duplicates are
/// locked once. Returns the guards (in no particular order) and the number
/// of back-offs; `EINTR` if `intr` and a signal arrived while waiting.
pub fn lock_all<'a, T>(
    ms: &[&'a WwMutex<T>],
    tx: &'a WwAcquireCtx,
    intr: bool,
    ctx: &Sleepable,
) -> Result<(KVec<WwGuard<'a, T>>, u32), Errno> {
    let mut backoffs = 0u32;
    let mut contended: Option<usize> = None;
    'retry: loop {
        let mut held: KVec<WwGuard<'a, T>> = KVec::new();
        if let Some(i) = contended {
            let m = ms.get(i).ok_or(Errno::EINVAL)?;
            held.push(m.lock_slow(tx, intr, ctx)?, ctx)?;
        }
        for (i, m) in ms.iter().enumerate() {
            if contended == Some(i) {
                continue;
            }
            let r = if intr { m.lock_interruptible(tx, ctx) } else { m.lock(tx, ctx) };
            match r {
                Ok(g) => held.push(g, ctx)?,
                Err(e) if e == EALREADY => {}
                Err(e) if e == EDEADLK => {
                    drop(held);
                    backoffs += 1;
                    contended = Some(i);
                    continue 'retry;
                }
                Err(e) => return Err(e),
            }
        }
        return Ok((held, backoffs));
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::sync::atomic::{AtomicBool, AtomicU32};
    use std::sync::{Arc, Barrier};
    use std::thread;
    use std::vec::Vec;

    fn sleep() -> Sleepable {
        // SAFETY: host test threads may block.
        unsafe { Sleepable::assume() }
    }

    static CLASS: WwClass = WwClass::new();
    static OTHER: WwClass = WwClass::new();

    #[test]
    fn raw_lock_belongs_to_its_thread() {
        let ctx = sleep();
        let m = Arc::new(WwMutex::new(0u32, &CLASS));
        m.lock_raw(false, &ctx).unwrap();
        assert!(m.held_raw_by_current());
        assert_eq!(m.lock_raw(false, &ctx).err(), Some(EDEADLK), "relock by the holder");
        let m2 = m.clone();
        thread::spawn(move || {
            assert!(!m2.held_raw_by_current());
            assert_eq!(m2.unlock_raw().err(), Some(Errno::EPERM), "unlock by another thread");
        })
        .join()
        .unwrap();
        assert!(m.is_locked(), "a foreign unlock changes nothing");
        // A guard holder is not a raw holder either.
        m.unlock_raw().unwrap();
        let g = m.lock_plain(&ctx);
        assert_eq!(m.unlock_raw().err(), Some(Errno::EPERM));
        drop(g);
        assert!(!m.is_locked());
        // Another thread waits for the raw holder, then takes it.
        m.lock_raw(false, &ctx).unwrap();
        let m3 = m.clone();
        let t = thread::spawn(move || {
            let c = sleep();
            m3.lock_raw(false, &c).unwrap();
            m3.unlock_raw().unwrap();
        });
        thread::sleep(std::time::Duration::from_millis(20));
        m.unlock_raw().unwrap();
        t.join().unwrap();
        assert!(!m.is_locked());
    }

    #[test]
    fn try_lock_raw_takes_only_a_free_mutex() {
        let ctx = sleep();
        let m = Arc::new(WwMutex::new(0u32, &CLASS));
        assert!(m.try_lock_raw());
        assert!(m.held_raw_by_current());
        assert!(!m.try_lock_raw(), "held: no second take, not even by the holder");
        let m2 = m.clone();
        thread::spawn(move || assert!(!m2.try_lock_raw())).join().unwrap();
        m.unlock_raw().unwrap();
        let g = m.lock_plain(&ctx);
        assert!(!m.try_lock_raw(), "a guard holder blocks it too");
        drop(g);
        assert!(m.try_lock_raw());
        m.unlock_raw().unwrap();
        assert!(!m.is_locked());
        let s = CLASS.next_stamp();
        assert!(m.try_lock_raw_ctx(s));
        assert!(!m.try_lock_raw_ctx(s), "no recursion");
        assert!(m.held_raw_by_current());
        assert_eq!(m.unlock_raw().err(), Some(Errno::EPERM), "a transaction's lock is not a plain one");
        m.unlock_raw_ctx(s).unwrap();
        assert!(!m.is_locked());
    }

    #[test]
    fn raw_ctx_follows_wait_die_with_rust_transactions() {
        let ctx = sleep();
        let a = Arc::new(WwMutex::new(0u32, &CLASS));
        let b = Arc::new(WwMutex::new(0u32, &CLASS));
        // A C transaction: its own stamp and count.
        let older = CLASS.next_stamp();
        let younger = WwAcquireCtx::new(&CLASS);
        assert!(younger.stamp() > older);
        a.lock_raw_ctx(older, 0, false, false, &ctx).unwrap();
        assert!(a.held_raw_by_current(), "the C transaction's holder is its thread");
        assert_eq!(a.lock_raw_ctx(older, 1, false, false, &ctx).err(), Some(EALREADY));
        assert_eq!(a.lock_raw(false, &ctx).err(), Some(EDEADLK), "the holder may not wait for itself");
        // A younger Rust transaction holding something dies on the C holder.
        let gb = b.lock(&younger, &ctx).unwrap();
        assert_eq!(a.lock(&younger, &ctx).err(), Some(EDEADLK));
        drop(gb);
        // A younger C transaction holding something dies too; holding
        // nothing it would wait (tested below with the slow path).
        let c_young = CLASS.next_stamp();
        b.lock_raw_ctx(c_young, 0, false, false, &ctx).unwrap();
        assert_eq!(a.lock_raw_ctx(c_young, 1, false, false, &ctx).err(), Some(EDEADLK));
        assert_eq!(a.lock_raw_ctx(c_young, 1, true, false, &ctx).err(), Some(Errno::EINVAL), "slow holding one");
        b.unlock_raw_ctx(c_young).unwrap();
        // Only the transaction's thread releases it, with its stamp.
        assert_eq!(a.unlock_raw_ctx(c_young).err(), Some(Errno::EPERM));
        let a2 = a.clone();
        thread::spawn(move || assert_eq!(a2.unlock_raw_ctx(older).err(), Some(Errno::EPERM))).join().unwrap();
        // The slow path waits for the holder, then takes it.
        let a3 = a.clone();
        let t = thread::spawn(move || {
            let c = sleep();
            a3.lock_raw_ctx(c_young, 0, true, false, &c).unwrap();
            a3.unlock_raw_ctx(c_young).unwrap();
        });
        thread::sleep(std::time::Duration::from_millis(20));
        a.unlock_raw_ctx(older).unwrap();
        t.join().unwrap();
        assert!(!a.is_locked() && !b.is_locked());
    }

    #[test]
    fn older_transaction_waits_for_younger_c_holder() {
        // Wait-die: a transaction older than the holder waits for it, even
        // while holding other locks; a younger one would die (EDEADLK).
        let ctx = sleep();
        let m = Arc::new(WwMutex::new(0u32, &CLASS));
        let other = Arc::new(WwMutex::new(0u32, &CLASS));
        let (made, go) = (Arc::new(Barrier::new(2)), Arc::new(Barrier::new(2)));
        let older_stamp = Arc::new(AtomicU64::new(0));
        let got = Arc::new(AtomicBool::new(false));
        let (m3, o3, mk, g2, os, g3) =
            (m.clone(), other.clone(), made.clone(), go.clone(), older_stamp.clone(), got.clone());
        let w = thread::spawn(move || {
            let c = sleep();
            // The older transaction: its stamp is taken before the C one.
            let tx = WwAcquireCtx::new(&CLASS);
            os.store(tx.stamp(), Ordering::SeqCst);
            let held = o3.lock(&tx, &c).unwrap();
            mk.wait();
            g2.wait();
            let g = m3.lock(&tx, &c).expect("an older transaction waits, it does not die");
            g3.store(true, Ordering::SeqCst);
            drop(g);
            drop(held);
        });
        made.wait();
        let young = CLASS.next_stamp();
        assert!(older_stamp.load(Ordering::SeqCst) < young);
        m.lock_raw_ctx(young, 0, false, false, &ctx).unwrap();
        // Only the holder's thread may release it.
        let m2 = m.clone();
        thread::spawn(move || assert_eq!(m2.unlock_raw_ctx(young).err(), Some(Errno::EPERM))).join().unwrap();
        go.wait();
        thread::sleep(std::time::Duration::from_millis(50));
        assert!(!got.load(Ordering::SeqCst), "waits while the younger C holder holds it");
        m.unlock_raw_ctx(young).unwrap();
        w.join().unwrap();
        assert!(got.load(Ordering::SeqCst));
        assert!(!m.is_locked() && !other.is_locked());
    }

    /// Rust transactions and C transactions (raw contexts, as the
    /// LinuxKPI layer's ww_acquire_ctx) lock random sets of the same
    /// mutexes at once, backing off on EDEADLK as Linux's protocol does:
    /// the run must finish (no deadlock) and no mutex has two holders.
    #[test]
    fn mixed_c_and_rust_transactions_lock_all() {
        const N: usize = 6;
        let ms: Arc<Vec<WwMutex<()>>> = Arc::new((0..N).map(|_| WwMutex::new((), &CLASS)).collect());
        let busy: Arc<Vec<AtomicBool>> = Arc::new((0..N).map(|_| AtomicBool::new(false)).collect());
        let hs: Vec<_> = (0..8u64)
            .map(|t| {
                let (ms, busy) = (ms.clone(), busy.clone());
                thread::spawn(move || {
                    let ctx = sleep();
                    let mut seed = t * 104729 + 3;
                    for _ in 0..300 {
                        seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
                        let pick: Vec<usize> = (0..N).filter(|i| (seed >> (i + 20)) & 1 == 1).collect();
                        if t % 2 == 0 {
                            // A Rust transaction.
                            let refs: Vec<&WwMutex<()>> = pick.iter().map(|&i| &ms[i]).collect();
                            let tx = WwAcquireCtx::new(&CLASS);
                            let (gs, _) = lock_all(&refs, &tx, false, &ctx).unwrap();
                            for &i in &pick {
                                assert!(!busy[i].swap(true, Ordering::SeqCst), "two holders");
                            }
                            thread::yield_now();
                            for &i in &pick {
                                busy[i].store(false, Ordering::SeqCst);
                            }
                            drop(gs);
                        } else {
                            // A C transaction: stamp, lock in order, back
                            // off on EDEADLK, then take the contended one
                            // with the slow path first.
                            let stamp = CLASS.next_stamp();
                            let mut held: Vec<usize> = Vec::new();
                            let mut first: Option<usize> = None;
                            'retry: loop {
                                if let Some(c) = first.take() {
                                    ms[c].lock_raw_ctx(stamp, 0, true, false, &ctx).unwrap();
                                    held.push(c);
                                }
                                for &i in &pick {
                                    if held.contains(&i) {
                                        continue;
                                    }
                                    match ms[i].lock_raw_ctx(stamp, held.len() as u32, false, false, &ctx) {
                                        Ok(()) => held.push(i),
                                        Err(e) if e == EDEADLK => {
                                            for &h in &held {
                                                ms[h].unlock_raw_ctx(stamp).unwrap();
                                            }
                                            held.clear();
                                            first = Some(i);
                                            continue 'retry;
                                        }
                                        Err(e) => panic!("lock_raw_ctx: {e:?}"),
                                    }
                                }
                                break;
                            }
                            for &i in &held {
                                assert!(!busy[i].swap(true, Ordering::SeqCst), "two holders");
                            }
                            thread::yield_now();
                            for &i in &held {
                                busy[i].store(false, Ordering::SeqCst);
                                ms[i].unlock_raw_ctx(stamp).unwrap();
                            }
                        }
                    }
                })
            })
            .collect();
        for h in hs {
            h.join().unwrap();
        }
        assert!(ms.iter().all(|m| !m.is_locked()));
    }

    #[test]
    fn already_class_and_slow_rules() {
        let ctx = sleep();
        let m = WwMutex::new(0u32, &CLASS);
        let o = WwMutex::new(0u32, &OTHER);
        let tx = WwAcquireCtx::new(&CLASS);
        let g = m.lock(&tx, &ctx).unwrap();
        assert_eq!(m.lock(&tx, &ctx).err(), Some(EALREADY));
        assert_eq!(o.lock(&tx, &ctx).err(), Some(Errno::EINVAL), "one class per transaction");
        assert_eq!(m.lock_slow(&tx, false, &ctx).err(), Some(Errno::EINVAL), "slow path while holding");
        assert_eq!(tx.acquired(), 1);
        drop(g);
        assert_eq!(tx.acquired(), 0);
        assert!(!m.is_locked());
        let mut p = m.lock_plain(&ctx);
        *p += 1;
        assert!(m.owns(&p));
        drop(p);
    }

    /// The deadlock the mutex exists for: two transactions take two
    /// mutexes in opposite orders at the same moment. The younger one gets
    /// EDEADLK, backs off, and both complete.
    #[test]
    fn opposite_order_deadlock_is_avoided() {
        let (a, b) = (Arc::new(WwMutex::new(0u32, &CLASS)), Arc::new(WwMutex::new(0u32, &CLASS)));
        let bar = Arc::new(Barrier::new(2));
        let deadlks = Arc::new(AtomicU32::new(0));
        let run = |first: Arc<WwMutex<u32>>, second: Arc<WwMutex<u32>>, bar: Arc<Barrier>, d: Arc<AtomicU32>| {
            thread::spawn(move || {
                let ctx = sleep();
                let tx = WwAcquireCtx::new(&CLASS);
                let mut g1 = Some(first.lock(&tx, &ctx).unwrap());
                bar.wait();
                loop {
                    match second.lock(&tx, &ctx) {
                        Ok(mut g2) => {
                            *g2 += 1;
                            **g1.as_mut().unwrap() += 1;
                            drop(g2);
                            break;
                        }
                        Err(e) => {
                            assert_eq!(e, EDEADLK);
                            d.fetch_add(1, Ordering::SeqCst);
                            // Back off: release, sleep on the contended one,
                            // then take the other again.
                            drop(g1.take());
                            let g2 = second.lock_slow(&tx, false, &ctx).unwrap();
                            g1 = Some(first.lock(&tx, &ctx).unwrap());
                            drop(g2);
                            // Retake second in the loop.
                        }
                    }
                }
                drop(g1);
            })
        };
        let t1 = run(a.clone(), b.clone(), bar.clone(), deadlks.clone());
        let t2 = run(b.clone(), a.clone(), bar, deadlks.clone());
        t1.join().unwrap();
        t2.join().unwrap();
        assert!(deadlks.load(Ordering::SeqCst) >= 1, "the younger transaction had to back off");
        let ctx = sleep();
        assert_eq!(*a.lock_plain(&ctx), 2);
        assert_eq!(*b.lock_plain(&ctx), 2);
    }

    /// Eight threads lock random subsets of six mutexes in random orders
    /// with lock_all. Each critical section checks that nobody else holds
    /// its mutexes; the run must finish (no deadlock) and count every
    /// increment.
    #[test]
    fn concurrent_lock_all() {
        const N: usize = 6;
        struct Cell2 {
            busy: AtomicBool,
            count: u64,
        }
        let ms: Arc<Vec<WwMutex<Cell2>>> =
            Arc::new((0..N).map(|_| WwMutex::new(Cell2 { busy: AtomicBool::new(false), count: 0 }, &CLASS)).collect());
        let backoffs = Arc::new(AtomicU32::new(0));
        let hs: Vec<_> = (0..8u64)
            .map(|t| {
                let (ms, backoffs) = (ms.clone(), backoffs.clone());
                thread::spawn(move || {
                    let ctx = sleep();
                    let mut seed = t * 7919 + 1;
                    for _ in 0..400 {
                        seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
                        let mut pick: Vec<&WwMutex<Cell2>> = Vec::new();
                        for i in 0..N {
                            if (seed >> (i + 20)) & 1 == 1 {
                                pick.push(&ms[(i + (seed >> 40) as usize) % N]);
                            }
                        }
                        let tx = WwAcquireCtx::new(&CLASS);
                        let (mut gs, b) = lock_all(&pick, &tx, false, &ctx).unwrap();
                        backoffs.fetch_add(b, Ordering::SeqCst);
                        for g in gs.iter_mut() {
                            assert!(!g.busy.swap(true, Ordering::SeqCst), "two holders");
                            g.count += 1;
                        }
                        thread::yield_now();
                        for g in gs.iter_mut() {
                            g.busy.store(false, Ordering::SeqCst);
                        }
                        drop(gs);
                        assert_eq!(tx.acquired(), 0);
                    }
                })
            })
            .collect();
        for h in hs {
            h.join().unwrap();
        }
        let ctx = sleep();
        let total: u64 = ms.iter().map(|m| m.lock_plain(&ctx).count).sum();
        assert!(total > 0);
        std::println!("lock_all: {total} increments, {} back-offs", backoffs.load(Ordering::SeqCst));
    }

    #[test]
    fn interruptible_lock() {
        let ctx = sleep();
        let m = Arc::new(WwMutex::new((), &CLASS));
        let held = m.lock_plain(&ctx);
        let m2 = m.clone();
        let t = thread::spawn(move || {
            let ctx = sleep();
            let tx = WwAcquireCtx::new(&CLASS);
            crate::ksync::SIGNAL_PENDING.with(|s| s.set(true));
            m2.lock_interruptible(&tx, &ctx).err()
        });
        assert_eq!(t.join().unwrap(), Some(Errno::EINTR));
        drop(held);
    }
}
