// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Sync objects: a slot for a fence that user space names by handle (Linux's
//! `drm_syncobj`), binary or timeline.
//!
//! A binary sync object holds one fence or none: `SIGNAL` puts a signalled
//! fence in, `RESET` takes it out, a driver's submission replaces it. A
//! timeline sync object holds the latest point of a chain
//! ([`Fence::chain`]): `TIMELINE_SIGNAL` and submissions add points,
//! [`SyncobjInner::find_fence`] gives the fence of a point, [`SyncobjInner::query`]
//! the last point reached. Both kinds are one type, as in Linux; point 0
//! means the binary behaviour.
//!
//! [`wait_many`] implements `SYNCOBJ_WAIT` and `SYNCOBJ_TIMELINE_WAIT`: wait
//! for all or any of several sync objects (at points), optionally for
//! fences that have not been submitted yet (`WAIT_FOR_SUBMIT`) or only for
//! their submission (`WAIT_AVAILABLE`), until an absolute deadline on the
//! monotonic clock, interruptibly. It registers a callback on every fence
//! and, for pending submissions, a waiter on the sync object, all pointing
//! at one [`Waiter`] on the waiting thread's stack; the cleanup removes
//! them all (waiting for callbacks that are running) before the waiter
//! goes away.
//!
//! Everything here runs in thread context; the fence callbacks run in the
//! signaller's context and only touch the [`Waiter`], whose mutex has the
//! domain's interrupt priority. The logic is tested on the host from
//! several threads; the ioctl layer is in the device code.

use ddi::{Errno, Sleepable};

use crate::defer::Ctx;
use crate::fence::{self, Fence, FenceCallback, FenceCb, FenceDomain, SyncStats};
use crate::karc::KArc;
use crate::ksync::{KCondVar, KMutex, Nanos, Wake};
use crate::kvec::KVec;

/// `DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL`.
pub const WAIT_ALL: u32 = drm_uapi::DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
/// `DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT`.
pub const WAIT_FOR_SUBMIT: u32 = drm_uapi::DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
/// `DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE`.
pub const WAIT_AVAILABLE: u32 = drm_uapi::DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;

/// How long [`SyncobjInner::find_fence`] waits for a submission with
/// `WAIT_FOR_SUBMIT` (Linux's `DRM_SYNCOBJ_WAIT_FOR_SUBMIT_TIMEOUT`, 5 s).
pub const SUBMIT_TIMEOUT: Nanos = 5_000_000_000;

/// The shared part of a sync object.
pub struct SyncobjInner {
    domain: KArc<FenceDomain>,
    state: KMutex<State>,
}

struct State {
    fence: Option<Fence>,
    /// Waiters for a submission, woken whenever the fence changes.
    waiters: KVec<*const Waiter>,
}

// SAFETY: the waiter pointers are registered and removed under the lock by
// threads that keep the waiters alive meanwhile (see wait_many).
unsafe impl Send for State {}

impl Drop for SyncobjInner {
    fn drop(&mut self) {
        SyncStats::dec(&self.domain.stats.syncobjs);
        // The fence reference parks itself if it was the last one.
        drop(self.state.lock().fence.take());
    }
}

/// A counted reference to a sync object; handle tables and sync object
/// files hold these. Dropped in thread context (the fence inside parks
/// itself for the domain if needed).
pub type Syncobj = KArc<SyncobjInner>;

/// Creates a sync object, empty or (`signaled`) with a signalled fence.
pub fn create(domain: &KArc<FenceDomain>, signaled: bool, ctx: &Sleepable) -> Result<Syncobj, Errno> {
    let fence = if signaled { Some(Fence::signaled_stub(domain, ctx)?) } else { None };
    let s = KArc::new(
        SyncobjInner { domain: KArc::clone(domain), state: KMutex::new(State { fence, waiters: KVec::new() }) },
        ctx,
    )?;
    SyncStats::inc(&domain.stats.syncobjs);
    Ok(s)
}

impl SyncobjInner {
    /// Returns the current fence.
    pub fn fence(&self) -> Option<Fence> {
        self.state.lock().fence.clone()
    }

    /// Replaces the fence (Linux's `drm_syncobj_replace_fence`) and wakes
    /// the waiters for a submission. `None` resets.
    pub fn replace_fence(&self, f: Option<Fence>, ctx: &Sleepable) {
        let old = {
            let mut st = self.state.lock();
            let old = core::mem::replace(&mut st.fence, f);
            wake_all(&st.waiters);
            old
        };
        if let Some(o) = old {
            o.put(Ctx::Sleep(ctx));
        }
    }

    /// Adds point `point` with `fence` to the timeline (Linux's
    /// `drm_syncobj_add_point`); point 0 replaces the fence as for a binary
    /// sync object.
    pub fn add_point(&self, fence: Fence, point: u64, ctx: &Sleepable) -> Result<(), Errno> {
        if point == 0 {
            self.replace_fence(Some(fence), ctx);
            return Ok(());
        }
        // Built under the lock, as in Linux, so that concurrent adds each
        // chain onto the other; the allocation may block, which is allowed
        // with this thread-context mutex held.
        let old = {
            let mut st = self.state.lock();
            let node = Fence::chain(&self.domain, st.fence.clone(), fence, point, ctx)?;
            let old = st.fence.replace(node);
            wake_all(&st.waiters);
            old
        };
        if let Some(o) = old {
            o.put(Ctx::Sleep(ctx));
        }
        Ok(())
    }

    /// `SYNCOBJ_SIGNAL` / `TIMELINE_SIGNAL`: a signalled fence at `point`.
    pub fn signal(&self, point: u64, ctx: &Sleepable) -> Result<(), Errno> {
        let stub = Fence::signaled_stub(&self.domain, ctx)?;
        self.add_point(stub, point, ctx)
    }

    /// Returns the fence of `point` if it has been submitted:
    /// `Ok(Some(fence))`, a signalled stub if the point is certainly
    /// reached, `Err(EINVAL)` if it is not submitted (or the object is
    /// empty).
    fn lookup(&self, point: u64, ctx: &Sleepable) -> Result<Fence, Errno> {
        let cur = self.fence().ok_or(Errno::EINVAL)?;
        let r = fence::chain_find_seqno(&cur, point);
        cur.put(Ctx::Sleep(ctx));
        match r? {
            Some(f) => Ok(f),
            None => Fence::signaled_stub(&self.domain, ctx),
        }
    }

    /// Returns the fence of `point` (Linux's `drm_syncobj_find_fence`).
    /// `EINVAL` if nothing is submitted for it, unless `wait_for_submit`,
    /// which waits up to [`SUBMIT_TIMEOUT`] for a submission (`ETIME`,
    /// `EINTR`).
    pub fn find_fence(&self, point: u64, wait_for_submit: bool, ctx: &Sleepable) -> Result<Fence, Errno> {
        match self.lookup(point, ctx) {
            Ok(f) => return Ok(f),
            Err(e) if !wait_for_submit || e != Errno::EINVAL => return Err(e),
            Err(_) => {}
        }
        let w = Waiter::new(&self.domain);
        let deadline = crate::ksync::now().saturating_add(SUBMIT_TIMEOUT);
        self.register(&w, ctx)?;
        let r = loop {
            let gen0 = w.generation();
            match self.lookup(point, ctx) {
                Ok(f) => break Ok(f),
                Err(e) if e != Errno::EINVAL => break Err(e),
                Err(_) => {}
            }
            if let Err(e) = w.wait(gen0, Some(deadline), true, ctx) {
                break Err(e);
            }
        };
        self.unregister(&w);
        r
    }

    /// `SYNCOBJ_QUERY`: the last point reached, or with `last_submitted`
    /// the latest point; 0 for a binary or empty sync object.
    pub fn query(&self, last_submitted: bool, ctx: &Sleepable) -> u64 {
        match self.fence() {
            Some(f) => {
                let p = fence::chain_query(&f, last_submitted);
                f.put(Ctx::Sleep(ctx));
                p
            }
            None => 0,
        }
    }

    fn register(&self, w: &Waiter, ctx: &Sleepable) -> Result<(), Errno> {
        self.state.lock().waiters.push(w as *const Waiter, ctx)
    }

    fn unregister(&self, w: &Waiter) {
        let mut st = self.state.lock();
        while let Some(i) = st.waiters.iter().position(|p| core::ptr::eq(*p, w)) {
            st.waiters.swap_remove(i);
        }
    }
}

fn wake_all(ws: &[*const Waiter]) {
    for w in ws {
        // SAFETY: registered waiters stay alive until they unregister, which
        // needs the sync object lock the caller holds.
        unsafe { &**w }.wake();
    }
}

/// What a waiting thread sleeps on: a generation counter bumped by every
/// wake-up, behind a mutex of the domain's interrupt priority, because
/// fence callbacks bump it.
pub struct Waiter {
    generation: KMutex<u64>,
    cv: KCondVar,
}

impl Waiter {
    /// A new waiter for fences of `domain`.
    pub fn new(domain: &FenceDomain) -> Waiter {
        Waiter { generation: KMutex::with_priority(0, domain.priority()), cv: KCondVar::new() }
    }
    /// Returns the current generation; pass it to [`Waiter::wait`] after
    /// checking the condition, so that no wake-up in between is lost.
    pub fn generation(&self) -> u64 {
        *self.generation.lock()
    }
    /// Wakes the waiting thread. Any context up to the domain's priority.
    pub fn wake(&self) {
        let mut g = self.generation.lock();
        *g = g.wrapping_add(1);
        self.cv.notify_all();
    }
    /// Sleeps until a wake-up after generation `gen0`, the deadline, or
    /// (with `intr`) a signal: `ETIME`, `EINTR`.
    pub fn wait(&self, gen0: u64, deadline: Option<Nanos>, intr: bool, ctx: &Sleepable) -> Result<(), Errno> {
        let mut g = self.generation.lock();
        while *g == gen0 {
            let (ng, w) = self.cv.wait(g, deadline, intr, ctx);
            g = ng;
            if *g != gen0 {
                break;
            }
            if w != Wake::Woken {
                return Err(w.errno().unwrap_or(Errno::EINVAL));
            }
        }
        Ok(())
    }
}

/// The fence callback of a wait: wakes the [`Waiter`]. Holds a raw pointer,
/// which is valid because [`wait_many`] removes every callback, waiting for
/// running ones, before the waiter goes away.
struct WakeCb(*const Waiter);

// SAFETY: see WakeCb; the Waiter is Sync.
unsafe impl Send for WakeCb {}
// SAFETY: as above.
unsafe impl Sync for WakeCb {}

impl FenceCallback for WakeCb {
    fn signaled(&self, _fence: &Fence, _ctx: Ctx<'_>) -> Option<Fence> {
        // SAFETY: see WakeCb.
        unsafe { &*self.0 }.wake();
        None
    }
}

struct Entry<'a> {
    obj: &'a SyncobjInner,
    point: u64,
    fence: Option<Fence>,
    cb: Option<FenceCb<WakeCb>>,
    registered: bool,
}

/// Waits for sync objects at points, as `SYNCOBJ_WAIT` (points all 0) and
/// `SYNCOBJ_TIMELINE_WAIT` do (Linux's `drm_syncobj_array_wait_timeout`).
/// `flags` is a combination of [`WAIT_ALL`], [`WAIT_FOR_SUBMIT`] and
/// [`WAIT_AVAILABLE`]; the caller has rejected others. Returns the index
/// of the first signalled entry. `EINVAL` if an entry has nothing
/// submitted and neither submit flag is given, `ETIME` when the deadline
/// passes (a deadline in the past polls), `EINTR` on a signal.
pub fn wait_many(
    objs: &[(&SyncobjInner, u64)],
    flags: u32,
    deadline: Nanos,
    domain: &FenceDomain,
    ctx: &Sleepable,
) -> Result<u32, Errno> {
    let all = flags & WAIT_ALL != 0;
    let submit = flags & (WAIT_FOR_SUBMIT | WAIT_AVAILABLE) != 0;
    let available = flags & WAIT_AVAILABLE != 0;
    let mut es: KVec<Entry<'_>> = KVec::new();
    for (obj, point) in objs {
        es.push(Entry { obj, point: *point, fence: None, cb: None, registered: false }, ctx)?;
    }
    let w = Waiter::new(domain);
    let r = wait_entries(&mut es, &w, all, submit, available, deadline, ctx);
    // Cleanup: after this nothing refers to `w` any more.
    for e in es.iter_mut() {
        if e.registered {
            e.obj.unregister(&w);
        }
        if let (Some(f), Some(cb)) = (&e.fence, &e.cb) {
            f.remove_callback(cb, ctx);
        }
        if let Some(f) = e.fence.take() {
            f.put(Ctx::Sleep(ctx));
        }
    }
    r
}

fn wait_entries(
    es: &mut [Entry<'_>],
    w: &Waiter,
    all: bool,
    submit: bool,
    available: bool,
    deadline: Nanos,
    ctx: &Sleepable,
) -> Result<u32, Errno> {
    let n = es.len();
    // First pass: find the fences, fail early without a submit flag.
    let mut first = 0u32;
    let mut count = 0;
    for (i, e) in es.iter_mut().enumerate() {
        match e.obj.lookup(e.point, ctx) {
            Ok(f) => e.fence = Some(f),
            Err(err) if submit && err == Errno::EINVAL => {}
            Err(err) => return Err(err),
        }
        if let Some(f) = &e.fence
            && (available || f.is_signaled())
        {
            if count == 0 {
                first = i as u32;
            }
            count += 1;
        }
    }
    if count == n || (count > 0 && !all) {
        return Ok(first);
    }
    if submit {
        for e in es.iter_mut() {
            if e.fence.is_none() {
                e.obj.register(w, ctx)?;
                e.registered = true;
            }
        }
    }
    loop {
        let gen0 = w.generation();
        let mut count = 0;
        for (i, e) in es.iter_mut().enumerate() {
            if e.fence.is_none() {
                match e.obj.lookup(e.point, ctx) {
                    Ok(f) => e.fence = Some(f),
                    Err(err) if err == Errno::EINVAL => continue,
                    Err(err) => return Err(err),
                }
            }
            let Some(f) = &e.fence else { continue };
            let mut done = available || f.is_signaled();
            if !done && e.cb.is_none() {
                let cb = FenceCb::new(WakeCb(w as *const Waiter), ctx)?;
                done = f.add_callback(&cb).is_err();
                e.cb = Some(cb);
            }
            if done {
                if count == 0 {
                    first = i as u32;
                }
                count += 1;
                if !all {
                    return Ok(first);
                }
            }
        }
        if count == n {
            return Ok(first);
        }
        w.wait(gen0, Some(deadline), true, ctx)?;
    }
}

/// Counts a domain's live sync objects, for tests.
#[cfg(test)]
fn live(d: &FenceDomain) -> u64 {
    d.stats.syncobjs.load(core::sync::atomic::Ordering::SeqCst)
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use crate::ksync::{ETIME, now};
    use core::sync::atomic::Ordering;
    use std::sync::Arc;
    use std::thread;
    use std::time::Duration;

    fn sleep() -> Sleepable {
        // SAFETY: host test threads may block.
        unsafe { Sleepable::assume() }
    }

    fn fence(d: &KArc<FenceDomain>) -> Fence {
        Fence::new(d, d.alloc_contexts(1), 1, &sleep()).unwrap()
    }

    #[test]
    fn binary_semantics() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        let s = create(&d, false, &ctx).unwrap();
        assert_eq!(live(&d), 1);
        assert!(s.fence().is_none());
        assert_eq!(s.find_fence(0, false, &ctx).err(), Some(Errno::EINVAL));
        let t0 = now();
        assert_eq!(wait_many(&[(&s, 0)], 0, t0, &d, &ctx), Err(Errno::EINVAL), "nothing submitted");
        assert_eq!(wait_many(&[(&s, 0)], WAIT_FOR_SUBMIT, t0, &d, &ctx), Err(ETIME), "poll");
        s.signal(0, &ctx).unwrap();
        assert!(s.find_fence(0, false, &ctx).unwrap().is_signaled());
        assert_eq!(wait_many(&[(&s, 0)], 0, t0, &d, &ctx), Ok(0));
        assert_eq!(s.query(false, &ctx), 0, "binary objects report 0");
        s.replace_fence(None, &ctx);
        assert!(s.fence().is_none());
        let f = fence(&d);
        s.replace_fence(Some(f.clone()), &ctx);
        assert_eq!(wait_many(&[(&s, 0)], 0, now() + 5_000_000, &d, &ctx), Err(ETIME));
        assert_eq!(s.find_fence(3, false, &ctx).err(), Some(Errno::EINVAL), "points need a timeline");
        f.signal(Ctx::Sleep(&ctx));
        assert_eq!(wait_many(&[(&s, 0)], WAIT_ALL, now() + 5_000_000, &d, &ctx), Ok(0));
        let signaled = create(&d, true, &ctx).unwrap();
        assert!(signaled.fence().unwrap().is_signaled());
        drop((s, signaled, f));
        d.drain(&ctx);
        assert_eq!(d.stats.live(), [0; 8]);
    }

    #[test]
    fn timeline_semantics() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        let s = create(&d, false, &ctx).unwrap();
        let pts: std::vec::Vec<Fence> = (0..3).map(|_| fence(&d)).collect();
        for (i, f) in pts.iter().enumerate() {
            s.add_point(f.clone(), (i as u64 + 1) * 2, &ctx).unwrap();
        }
        assert_eq!(s.query(true, &ctx), 6);
        assert_eq!(s.query(false, &ctx), 0);
        assert_eq!(s.find_fence(7, false, &ctx).err(), Some(Errno::EINVAL), "not submitted");
        let f3 = s.find_fence(3, false, &ctx).unwrap();
        assert_eq!(f3.seqno(), 4);
        pts[1].signal(Ctx::Sleep(&ctx));
        assert!(!f3.is_signaled(), "point 4 needs point 2");
        pts[0].signal(Ctx::Sleep(&ctx));
        assert!(f3.is_signaled());
        assert_eq!(s.query(false, &ctx), 4);
        // TIMELINE_SIGNAL of a later point, then a wait for it.
        s.signal(10, &ctx).unwrap();
        assert_eq!(s.query(true, &ctx), 10);
        assert_eq!(s.query(false, &ctx), 4, "point 6 still pending");
        assert_eq!(wait_many(&[(&s, 10)], 0, now(), &d, &ctx), Err(ETIME));
        pts[2].signal(Ctx::Sleep(&ctx));
        assert_eq!(wait_many(&[(&s, 10)], WAIT_ALL, now() + 1_000_000_000, &d, &ctx), Ok(0));
        assert_eq!(s.query(false, &ctx), 10);
        assert!(s.find_fence(1, false, &ctx).unwrap().is_signaled(), "reached points give a signalled fence");
        drop((s, f3, pts));
        d.drain(&ctx);
        assert_eq!(d.stats.live(), [0; 8]);
    }

    /// WAIT_FOR_SUBMIT and WAIT_AVAILABLE: a thread waits for points that
    /// another thread submits and signals later; ANY returns the first.
    #[test]
    fn wait_for_submit_and_available_across_threads() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        let a = Arc::new(create(&d, false, &ctx).unwrap());
        let b = Arc::new(create(&d, false, &ctx).unwrap());
        let (a2, b2, d2) = (a.clone(), b.clone(), d.clone());
        let t = thread::spawn(move || {
            let ctx = sleep();
            thread::sleep(Duration::from_millis(20));
            let fa = Fence::new(&d2, d2.alloc_contexts(1), 1, &ctx).unwrap();
            a2.add_point(fa.clone(), 5, &ctx).unwrap();
            thread::sleep(Duration::from_millis(20));
            let fb = Fence::new(&d2, d2.alloc_contexts(1), 1, &ctx).unwrap();
            b2.add_point(fb.clone(), 1, &ctx).unwrap();
            thread::sleep(Duration::from_millis(20));
            fb.signal(Ctx::Sleep(&ctx));
            thread::sleep(Duration::from_millis(20));
            fa.signal(Ctx::Sleep(&ctx));
            drop((fa, fb));
        });
        let dl = now() + 5_000_000_000;
        // Available: returns once point 5 of a is submitted.
        assert_eq!(wait_many(&[(&a, 5)], WAIT_AVAILABLE, dl, &d, &ctx), Ok(0));
        assert!(!a.find_fence(5, false, &ctx).unwrap().is_signaled());
        // Any, waiting for submission of b: b's point signals first.
        assert_eq!(wait_many(&[(&a, 5), (&b, 1)], WAIT_FOR_SUBMIT, dl, &d, &ctx), Ok(1));
        // All: returns when a signals too.
        assert_eq!(wait_many(&[(&a, 5), (&b, 1)], WAIT_ALL | WAIT_FOR_SUBMIT, dl, &d, &ctx), Ok(0));
        t.join().unwrap();
        // find_fence with WAIT_FOR_SUBMIT sees a later submission.
        let (a3, d3) = (a.clone(), d.clone());
        let t = thread::spawn(move || {
            let ctx = sleep();
            thread::sleep(Duration::from_millis(20));
            a3.signal(9, &ctx).unwrap();
            drop(d3);
        });
        assert!(a.find_fence(9, true, &ctx).unwrap().is_signaled());
        t.join().unwrap();
        crate::ksync::SIGNAL_PENDING.with(|s| s.set(true));
        assert_eq!(b.find_fence(50, true, &ctx).err(), Some(Errno::EINTR));
        drop((a, b));
        d.drain(&ctx);
        assert_eq!(d.stats.live(), [0; 8], "every callback and waiter was cleaned up");
    }

    /// Eight threads wait on sync objects that four others fill and signal
    /// in random order; every wait completes and nothing is left behind.
    #[test]
    fn concurrent_waits_and_signals() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        let objs: Arc<std::vec::Vec<Syncobj>> = Arc::new((0..4).map(|_| create(&d, false, &ctx).unwrap()).collect());
        let mut hs = std::vec::Vec::new();
        for t in 0..8usize {
            let (objs, d) = (objs.clone(), d.clone());
            hs.push(thread::spawn(move || {
                let ctx = sleep();
                for round in 1..=20u64 {
                    let pick = [(&*objs[t % 4], round), (&*objs[(t + 1) % 4], round)];
                    let flags = if t % 2 == 0 { WAIT_ALL | WAIT_FOR_SUBMIT } else { WAIT_FOR_SUBMIT };
                    wait_many(&pick, flags, now() + 10_000_000_000, &d, &ctx).unwrap();
                }
            }));
        }
        for t in 0..4usize {
            let (objs, d) = (objs.clone(), d.clone());
            hs.push(thread::spawn(move || {
                let ctx = sleep();
                for round in 1..=20u64 {
                    let f = Fence::new(&d, d.alloc_contexts(1), round, &ctx).unwrap();
                    objs[t].add_point(f.clone(), round, &ctx).unwrap();
                    thread::yield_now();
                    f.signal(Ctx::Sleep(&ctx));
                    f.put(Ctx::Sleep(&ctx));
                }
            }));
        }
        for h in hs {
            h.join().unwrap();
        }
        drop(objs);
        d.drain(&ctx);
        assert_eq!(d.stats.live(), [0; 8]);
        assert_eq!(d.stats.callbacks.load(Ordering::SeqCst), 0);
    }
}
