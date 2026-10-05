// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Reservation objects: the fences of the work that uses a buffer (Linux's
//! `dma_resv`).
//!
//! Every GEM object has a [`Resv`]. Work that reads or writes the buffer
//! adds its fence with a [`Usage`]: `Kernel` for memory management the
//! kernel itself does (moves, clears), `Write` for work that writes the
//! buffer (the "exclusive" fence of older kernels), `Read` for readers
//! (the "shared" fences) and `Bookkeep` for fences that only keep the
//! buffer alive. Usages are ordered, and asking for a usage covers every
//! more important one: waiting with `Read` (before writing the buffer)
//! waits for kernel, write and read fences, waiting with `Write` (before
//! reading it) for kernel and write fences.
//!
//! Adding fences requires the reservation's [`WwMutex`] held (the guard is
//! the proof, and a guard of another object is refused with `EINVAL`), so
//! that a job can lock all its buffers with a transaction
//! ([`crate::wwmutex::lock_all`]), check their fences and add its own
//! atomically. Readers (tests, waits, snapshots) take a short internal
//! mutex instead and iterate with it held: there is no RCU, and a waiter
//! copies the fences it needs before it sleeps. Adding a fence replaces a
//! signalled one and an older fence of the same context with a usage that
//! is not more important, as Linux does, so the list stays as long as the
//! number of timelines using the buffer.

use ddi::{Errno, Sleepable};

use crate::defer::Ctx;
use crate::fence::{Fence, FenceDomain};
use crate::karc::KArc;
use crate::ksync::{KMutex, Nanos};
use crate::kvec::KVec;
use crate::wwmutex::{RESERVATION_CLASS, WwGuard, WwMutex};

/// What a fence in a reservation stands for; ordered from most to least
/// important.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Usage {
    /// Kernel memory management.
    Kernel,
    /// Work writing the buffer.
    Write,
    /// Work reading the buffer.
    Read,
    /// Fences that only keep the buffer alive.
    Bookkeep,
}

/// A reservation object. See the module documentation.
pub struct Resv {
    lock: WwMutex<()>,
    fences: KMutex<KVec<(Fence, Usage)>>,
}

impl Resv {
    /// An empty reservation.
    pub fn new() -> Resv {
        Resv { lock: WwMutex::new((), &RESERVATION_CLASS), fences: KMutex::new(KVec::new()) }
    }

    /// The mutex that serialises changes, for [`crate::wwmutex::lock_all`]
    /// and [`WwMutex::lock_plain`].
    pub fn lock(&self) -> &WwMutex<()> {
        &self.lock
    }

    /// Adds `fence` with `usage`; the caller holds the reservation's
    /// mutex (`guard`). A signalled fence, and a fence of the same context
    /// that `fence` supersedes (not later, usage not more important), are
    /// replaced; the replaced references are dropped in thread context.
    pub fn add_fence(&self, guard: &WwGuard<'_, ()>, fence: Fence, usage: Usage, ctx: &Sleepable) -> Result<(), Errno> {
        if !self.lock.owns(guard) {
            fence.put(Ctx::Sleep(ctx));
            return Err(Errno::EINVAL);
        }
        self.add_locked(fence, usage, ctx)
    }

    /// [`Resv::add_fence`] for a lock taken with
    /// [`WwMutex::lock_raw`](crate::wwmutex::WwMutex::lock_raw) (the
    /// facade's `dma_resv_add_fence`): `EINVAL` unless the calling thread
    /// holds the lock that way.
    pub fn add_fence_raw(&self, fence: Fence, usage: Usage, ctx: &Sleepable) -> Result<(), Errno> {
        if !self.lock.held_raw_by_current() {
            fence.put(Ctx::Sleep(ctx));
            return Err(Errno::EINVAL);
        }
        self.add_locked(fence, usage, ctx)
    }

    /// Makes room for `n` more fences (`dma_resv_reserve_fences`).
    pub fn reserve(&self, n: usize, ctx: &Sleepable) -> Result<(), Errno> {
        self.fences.lock().reserve(n, ctx)
    }

    fn add_locked(&self, fence: Fence, usage: Usage, ctx: &Sleepable) -> Result<(), Errno> {
        let mut l = self.fences.lock();
        let slot = l.iter().position(|(old, u)| {
            old.is_signaled() || (old.context() == fence.context() && !old.is_later(&fence) && *u >= usage)
        });
        let old = match slot.and_then(|i| l.get_mut(i)) {
            Some(e) => Some(core::mem::replace(e, (fence, usage)).0),
            None => {
                l.push((fence, usage), ctx)?;
                None
            }
        };
        drop(l);
        if let Some(o) = old {
            o.put(Ctx::Sleep(ctx));
        }
        Ok(())
    }

    /// Returns references to the unsignalled fences with a usage at least
    /// as important as `usage`, as a snapshot.
    pub fn fences(&self, usage: Usage, ctx: &Sleepable) -> Result<KVec<Fence>, Errno> {
        let mut out = KVec::new();
        let l = self.fences.lock();
        for (f, u) in l.iter() {
            if *u <= usage && !f.is_signaled() {
                out.push(f.clone(), ctx)?;
            }
        }
        Ok(out)
    }

    /// Returns references to the fences with a usage at least as important
    /// as `usage`, each with its own usage, as a snapshot; signalled ones
    /// too if `signaled` (a holder of the lock sees every fence, as Linux's
    /// locked iteration does, including errors of signalled ones).
    pub fn entries(&self, usage: Usage, signaled: bool, ctx: &Sleepable) -> Result<KVec<(Fence, Usage)>, Errno> {
        let mut out = KVec::new();
        let l = self.fences.lock();
        for (f, u) in l.iter() {
            if *u <= usage && (signaled || !f.is_signaled()) {
                out.push((f.clone(), *u), ctx)?;
            }
        }
        Ok(out)
    }

    /// Reports whether every fence with a usage at least as important as
    /// `usage` has signalled.
    pub fn test_signaled(&self, usage: Usage) -> bool {
        self.fences.lock().iter().all(|(f, u)| *u > usage || f.is_signaled())
    }

    /// Waits for every fence with a usage at least as important as
    /// `usage`, as [`Fence::wait`] does (`ETIME`, `EINTR`).
    pub fn wait(&self, usage: Usage, intr: bool, deadline: Option<Nanos>, ctx: &Sleepable) -> Result<(), Errno> {
        let fs = self.fences(usage, ctx)?;
        let mut r = Ok(());
        for f in fs.iter() {
            if r.is_ok() {
                r = f.wait(intr, deadline, ctx);
            }
        }
        for f in drain(fs) {
            f.put(Ctx::Sleep(ctx));
        }
        r
    }

    /// Returns one fence that signals when all fences with a usage at least
    /// as important as `usage` have: `None` if there is none left, the
    /// fence itself if there is one, otherwise an array on a new context
    /// (Linux's `dma_resv_get_singleton`).
    pub fn singleton(&self, domain: &KArc<FenceDomain>, usage: Usage, ctx: &Sleepable) -> Result<Option<Fence>, Errno> {
        let mut fs = self.fences(usage, ctx)?;
        match fs.len() {
            0 => Ok(None),
            1 => Ok(fs.swap_remove(0)),
            _ => Fence::array(domain, fs, domain.alloc_contexts(1), 1, false, ctx).map(Some),
        }
    }

    /// Drops every fence, for an object being destroyed (thread context).
    pub fn clear(&self, ctx: &Sleepable) {
        let l = core::mem::take(&mut *self.fences.lock());
        for (f, _) in drain_pairs(l) {
            f.put(Ctx::Sleep(ctx));
        }
    }

    /// Returns the number of fences held, signalled ones included.
    pub fn len(&self) -> usize {
        self.fences.lock().len()
    }

    /// Reports whether no fence is held.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

impl Default for Resv {
    fn default() -> Self {
        Self::new()
    }
}

fn drain(mut v: KVec<Fence>) -> impl Iterator<Item = Fence> {
    core::iter::from_fn(move || v.swap_remove(v.len().wrapping_sub(1)))
}

fn drain_pairs(mut v: KVec<(Fence, Usage)>) -> impl Iterator<Item = (Fence, Usage)> {
    core::iter::from_fn(move || v.swap_remove(v.len().wrapping_sub(1)))
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use crate::wwmutex::{WwAcquireCtx, lock_all};
    use std::sync::Arc;
    use std::thread;
    use std::vec::Vec;

    fn sleep() -> Sleepable {
        // SAFETY: host test threads may block.
        unsafe { Sleepable::assume() }
    }

    #[test]
    fn usages_replacement_and_waits() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        let r = Resv::new();
        let other = Resv::new();
        let (c1, c2) = (d.alloc_contexts(1), d.alloc_contexts(1));
        let w1 = Fence::new(&d, c1, 1, &ctx).unwrap();
        let r1 = Fence::new(&d, c2, 1, &ctx).unwrap();
        {
            let g = r.lock().lock_plain(&ctx);
            let og = other.lock().lock_plain(&ctx);
            assert_eq!(r.add_fence(&og, w1.clone(), Usage::Write, &ctx), Err(Errno::EINVAL), "another object's guard");
            r.add_fence(&g, w1.clone(), Usage::Write, &ctx).unwrap();
            r.add_fence(&g, r1.clone(), Usage::Read, &ctx).unwrap();
            // A later fence of the same context and usage replaces w1.
            let w2 = Fence::new(&d, c1, 2, &ctx).unwrap();
            r.add_fence(&g, w2.clone(), Usage::Write, &ctx).unwrap();
            assert_eq!(r.len(), 2);
            w2.signal(Ctx::Sleep(&ctx));
            drop(w2);
        }
        assert!(r.test_signaled(Usage::Write), "the write fence signalled");
        assert!(!r.test_signaled(Usage::Read), "the reader has not");
        assert_eq!(r.wait(Usage::Read, true, Some(crate::ksync::now() + 10_000_000), &ctx), Err(crate::ksync::ETIME));
        let one = r.singleton(&d, Usage::Read, &ctx).unwrap().unwrap();
        assert!(Fence::ptr_eq(&one, &r1));
        {
            let g = r.lock().lock_plain(&ctx);
            let r2 = Fence::new(&d, d.alloc_contexts(1), 1, &ctx).unwrap();
            r.add_fence(&g, r2.clone(), Usage::Read, &ctx).unwrap();
            let both = r.singleton(&d, Usage::Bookkeep, &ctx).unwrap().unwrap();
            assert_eq!(both.array_members().map(<[Fence]>::len), Some(2));
            r1.signal(Ctx::Sleep(&ctx));
            assert!(!both.is_signaled());
            r2.signal(Ctx::Sleep(&ctx));
            assert!(both.is_signaled());
            drop((both, r2));
        }
        assert_eq!(r.wait(Usage::Bookkeep, true, None, &ctx), Ok(()));
        assert!(r.singleton(&d, Usage::Read, &ctx).unwrap().is_none());
        r.clear(&ctx);
        drop((one, w1, r1));
        d.drain(&ctx);
        assert_eq!(d.stats.live(), [0; 8]);
    }

    /// Jobs on eight threads lock the reservations of random buffer sets in
    /// random orders, wait for their fences, add their own and signal it
    /// later: every job sees the previous writer of each of its buffers
    /// signalled before it adds its own write fence.
    #[test]
    fn concurrent_jobs_serialise_through_reservations() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        let bufs: Arc<Vec<Resv>> = Arc::new((0..5).map(|_| Resv::new()).collect());
        let hs: Vec<_> = (0..8u64)
            .map(|t| {
                let (bufs, d) = (bufs.clone(), d.clone());
                thread::spawn(move || {
                    let ctx = sleep();
                    let context = d.alloc_contexts(1);
                    let mut seed = t + 11;
                    for n in 1..=100u64 {
                        seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
                        let pick: Vec<&WwMutex<()>> =
                            (0..5).filter(|i| (seed >> (30 + i)) & 1 == 1).map(|i| bufs[i].lock()).collect();
                        let tx = WwAcquireCtx::new(&RESERVATION_CLASS);
                        let (gs, _) = lock_all(&pick, &tx, false, &ctx).unwrap();
                        let job = Fence::new(&d, context, n, &ctx).unwrap();
                        for g in gs.iter() {
                            // lock_all returns the guards in its own order.
                            let b = bufs.iter().find(|b| b.lock().owns(g)).unwrap();
                            b.wait(Usage::Read, false, None, &ctx).unwrap();
                            assert!(b.test_signaled(Usage::Read));
                            b.add_fence(g, job.clone(), Usage::Write, &ctx).unwrap();
                        }
                        drop(gs);
                        thread::yield_now();
                        job.signal(Ctx::Sleep(&ctx));
                        job.put(Ctx::Sleep(&ctx));
                    }
                })
            })
            .collect();
        for h in hs {
            h.join().unwrap();
        }
        for b in bufs.iter() {
            assert!(b.test_signaled(Usage::Bookkeep));
            b.clear(&ctx);
        }
        drop(bufs);
        d.drain(&ctx);
        assert_eq!(d.stats.live(), [0; 8]);
    }
}
