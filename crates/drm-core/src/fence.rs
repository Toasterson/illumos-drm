// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Fences: the completion of one piece of work, Linux's `dma_fence`, with
//! fence arrays (`dma_fence_array`) and fence chains (`dma_fence_chain`).
//!
//! A [`Fence`] belongs to a context (a timeline, from
//! [`FenceDomain::alloc_contexts`]) and has a sequence number on it. It
//! starts unsignalled and is signalled exactly once, optionally with an
//! error ([`Fence::set_error`], Linux's `dma_fence_set_error`), which records
//! the time ([`Fence::timestamp`]), wakes every waiter and runs every
//! callback. Sequence numbers of one context are compared with
//! [`Fence::is_later`], which is wraparound-safe: the difference is read as
//! a signed 64-bit number, as Linux does for 32-bit contexts.
//!
//! # Contexts: who may do what
//!
//! * Creating a fence allocates: [`Fence::new`] with the [`Sleepable`]
//!   token (`KM_SLEEP`), [`Fence::try_new`] from anywhere (`KM_NOSLEEP`).
//! * Signalling ([`Fence::signal`]) never allocates and never blocks for
//!   long: it takes the fence's mutex briefly and runs the callbacks, which
//!   are allocated beforehand ([`FenceCb::new`], sleepable, or
//!   [`FenceCb::try_new`]). It takes a [`Ctx`] (the [`Sleepable`] or the
//!   [`ddi::Atomic`] token), which the callbacks receive, so that what a
//!   callback may do follows from the type of the context it runs in.
//! * Signalling is allowed in thread context, from callouts (`timeout(9F)`)
//!   and from soft interrupts and low-level interrupt handlers whose
//!   priority is at most the priority the device's [`FenceDomain`] was
//!   created with: every mutex a signal takes is created with that
//!   priority, as `mutex_init(9F)` requires for locks an interrupt handler
//!   takes. A high-level handler, whose locks would have to spin, hands the
//!   work to a soft interrupt; [`FenceDomain::new`] refuses a high-level
//!   priority.
//! * Waiting ([`Fence::wait`]) blocks and needs the [`Sleepable`] token. It
//!   takes an absolute deadline on the monotonic clock and whether a
//!   signal may interrupt it, and fails with `ETIME` or `EINTR` like
//!   Linux.
//! * Dropping a reference follows [`crate::defer`]: [`Fence::put`] with
//!   `Ctx::Sleep` destroys a fence whose last reference it was at once;
//!   with `Ctx::Atomic` (and for an implicit drop) the fence is parked on
//!   the domain's list and destroyed by its task queue, because destroying
//!   the fence's mutex and condition variable is only allowed in user or
//!   kernel context (`mutex(9F)`). Destroying a fence also drops the fences
//!   it holds (array members, chain links) iteratively, so a long chain
//!   cannot overflow the kernel stack.
//!
//! # Callbacks
//!
//! A callback is a value of a type implementing [`FenceCallback`] in a
//! [`FenceCb`], allocated ahead of time. [`Fence::add_callback`] links it
//! into the fence without allocating, or reports that the fence is already
//! signalled (Linux's `-ENOENT`). When the fence signals, its callbacks are
//! taken off the list under the fence's mutex and run outside it, in the
//! order they were added, so a callback may take locks that code calling
//! into the fence holds. A callback may return another fence to signal
//! next; that is how arrays and chains complete, and the signalling loop
//! runs such cascades iteratively rather than recursively.
//! [`Fence::remove_callback`] (thread context) unlinks a callback that has
//! not started, or else waits until it has finished, so that after it
//! returns the callback is not running and never will: that is what lets a
//! callback refer to state its owner frees afterwards. The value inside a
//! [`FenceCb`] may be dropped in the signaller's context, so its `Drop`
//! must be safe in interrupt context; fences and GEM references are, since
//! they defer, and the core's own callbacks hold only such references,
//! atomics and raw pointers to state that outlives them.
//!
//! # Arrays and chains
//!
//! [`Fence::array`] signals when all of its fences have (or, with
//! `signal_on_any`, the first), with the first error of a member.
//! [`Fence::chain`] is one point of a timeline: it signals when its own
//! fence and the previous point have, takes over the previous point's
//! context when its number is later, and so orders a timeline sync object;
//! [`chain_find_seqno`] and [`chain_query`] give the fence of a point and
//! the last point reached. A chain point drops its link to the previous
//! one as soon as that has signalled, which keeps timelines short.
//!
//! Arrays and chain points register their callbacks when they are
//! created, and each callback holds a reference to its array or point. An
//! unsignalled array or point therefore lives until its members signal,
//! even if nobody else refers to it any more. That is the contract of
//! Linux fences as well: every fence must signal eventually, and a driver
//! that is torn down signals what it has left (with an error such as
//! `ECANCELED`); [`FenceDomain::quiesce`] refuses a detach while fences
//! remain.
//!
//! The logic is plain data over [`crate::ksync`] and is tested on the host
//! with real threads (signal against add and remove of callbacks, waits,
//! arrays, chains, long chains); the kernel layer uses it unchanged.

use core::mem::ManuallyDrop;
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicBool, AtomicI32, AtomicPtr, AtomicU8, AtomicU32, AtomicU64, Ordering};

use ddi::{Errno, IntrPriority, Sleepable};

use crate::defer::{Ctx, DeferLink, DeferList, Deferrable};
use crate::karc::{KArc, KArcLast};
use crate::ksync::{self, KCondVar, KMutex, Nanos, Wake};
use crate::kvec::KVec;

// --- statistics -------------------------------------------------------------

/// Counters of a device's synchronisation objects, updated with atomics
/// from any context. The kernel layer publishes them as the `sync` kstat.
#[derive(Default)]
pub struct SyncStats {
    /// Live fences.
    pub fences: AtomicU64,
    /// Fences signalled so far.
    pub signaled: AtomicU64,
    /// Fences signalled with an error so far.
    pub errors: AtomicU64,
    /// Callbacks registered and not yet run or removed.
    pub callbacks: AtomicU64,
    /// Live fence arrays.
    pub arrays: AtomicU64,
    /// Live chain points.
    pub chains: AtomicU64,
    /// Fences whose last reference was dropped where they could not be
    /// destroyed, so far.
    pub deferred: AtomicU64,
    /// Runs of the deferred-release task so far.
    pub drains: AtomicU64,
    /// Waits that blocked, so far.
    pub waits: AtomicU64,
    /// Waits that timed out, so far.
    pub timeouts: AtomicU64,
    /// Waits that a signal interrupted, so far.
    pub interrupted: AtomicU64,
    /// Live sync objects.
    pub syncobjs: AtomicU64,
    /// Live sync files.
    pub sync_files: AtomicU64,
    /// Live dma-bufs (exported GEM objects).
    pub dmabufs: AtomicU64,
    /// Open file descriptors the core created (dma-buf, sync object and
    /// sync file files), counted by device number.
    pub anon_files: AtomicU64,
}

impl SyncStats {
    pub(crate) fn inc(c: &AtomicU64) {
        c.fetch_add(1, Ordering::Relaxed);
    }
    pub(crate) fn dec(c: &AtomicU64) {
        c.fetch_sub(1, Ordering::Relaxed);
    }
    /// The live counters; all zero when nothing is left.
    pub fn live(&self) -> [u64; 8] {
        let l = |a: &AtomicU64| a.load(Ordering::Relaxed);
        [
            l(&self.fences),
            l(&self.callbacks),
            l(&self.arrays),
            l(&self.chains),
            l(&self.syncobjs),
            l(&self.sync_files),
            l(&self.dmabufs),
            l(&self.anon_files),
        ]
    }
}

#[cfg(not(test))]
ddi::named_kstat! {
    /// The `sync` kstat of a DRM device (`kstat -m <driver> -n sync`).
    pub struct SyncKstat {
        /// Live fences.
        pub fences: u64,
        /// Fences signalled so far.
        pub signaled: u64,
        /// Fences signalled with an error so far.
        pub errors: u64,
        /// Registered fence callbacks.
        pub callbacks: u64,
        /// Live fence arrays.
        pub arrays: u64,
        /// Live timeline points.
        pub chains: u64,
        /// Fences released by the task queue so far.
        pub deferred: u64,
        /// Runs of the release task so far.
        pub drains: u64,
        /// Blocking waits so far.
        pub waits: u64,
        /// Waits that timed out so far.
        pub timeouts: u64,
        /// Waits a signal interrupted so far.
        pub interrupted: u64,
        /// Live sync objects.
        pub syncobjs: u64,
        /// Live sync files.
        pub sync_files: u64,
        /// Live dma-bufs.
        pub dmabufs: u64,
        /// Anonymous minors in use.
        pub anon_files: u64,
    }
}

// --- the domain -------------------------------------------------------------

/// Fence contexts are unique across the module, so that fences of two
/// devices never share one.
static NEXT_CONTEXT: AtomicU64 = AtomicU64::new(1);

/// The fences of one device: the interrupt priority their locks allow, the
/// statistics, and the list of fences waiting to be destroyed in thread
/// context. Every fence holds a reference to its domain.
pub struct FenceDomain {
    // Declared first so that it is dropped first: destroying the queue
    // waits for a drain task that may still run.
    #[cfg(not(test))]
    tq: ddi::TaskQueue,
    pri: Option<IntrPriority>,
    deferred: DeferList<FenceInner>,
    drain_scheduled: AtomicBool,
    /// The counters.
    pub stats: SyncStats,
    /// The `sync` kstat the counters are copied to.
    #[cfg(not(test))]
    kstat: Option<ddi::TypedKstat<SyncKstat>>,
}

/// A raw pointer to the domain for the drain task.
#[cfg(not(test))]
struct DomainPtr(*const FenceDomain);
// SAFETY: the task runs only while the domain exists (quiesce waits for it,
// and the task queue is dropped first).
#[cfg(not(test))]
unsafe impl Send for DomainPtr {}

impl FenceDomain {
    /// Creates the domain of a device. `pri` is the highest interrupt
    /// priority fences of this device are signalled at (a soft interrupt's,
    /// say), or `None` for thread context and callouts only. A high-level
    /// priority is refused with `EINVAL`: locks at such a level spin, and
    /// waiters could not sleep on them.
    #[cfg(not(test))]
    pub fn new(dev: ddi::DevInfo, pri: Option<IntrPriority>, ctx: &Sleepable) -> Result<KArc<FenceDomain>, Errno> {
        if let Some(p) = pri {
            // SAFETY: ddi_intr_get_hilevel_pri(9F) has no preconditions.
            if p.get() >= unsafe { ddi::sys::ddi_intr_get_hilevel_pri() } {
                return Err(Errno::EINVAL);
            }
        }
        let tq = ddi::TaskQueue::new(dev, c"fencefree", core::num::NonZeroU32::MIN, ctx).map_err(|_| Errno::ENOMEM)?;
        let kstat = ddi::TypedKstat::<SyncKstat>::new(
            ddi::KstatId { module: dev.driver_name(), instance: dev.instance(), name: c"sync", class: c"misc" },
            ctx,
        );
        if kstat.is_err() {
            dev.warn(format_args!("cannot create the sync kstat; statistics are unavailable"));
        }
        Self::with_queue(tq, pri, kstat.ok(), ctx)
    }

    /// Creates the fence domain of the core module itself, for fences that
    /// belong to no device (the facade's `dma_fence_init` on a driver's C
    /// structure): its task queue `drm_fencefree` has no device node, and
    /// its statistics are the kstat `drm:0:sync`. Its locks are created
    /// without an interrupt priority; they are adaptive mutexes, which
    /// soft interrupts and low-level interrupt handlers may take.
    #[cfg(not(test))]
    pub fn for_module(ctx: &Sleepable) -> Result<KArc<FenceDomain>, Errno> {
        let tq = ddi::TaskQueue::without_node(c"drm_fencefree", core::num::NonZeroU32::MIN, ctx)
            .map_err(|_| Errno::ENOMEM)?;
        let kstat = ddi::TypedKstat::<SyncKstat>::new(
            ddi::KstatId { module: c"drm", instance: ddi::Instance(0), name: c"sync", class: c"misc" },
            ctx,
        );
        Self::with_queue(tq, None, kstat.ok(), ctx)
    }

    #[cfg(not(test))]
    fn with_queue(
        tq: ddi::TaskQueue,
        pri: Option<IntrPriority>,
        kstat: Option<ddi::TypedKstat<SyncKstat>>,
        ctx: &Sleepable,
    ) -> Result<KArc<FenceDomain>, Errno> {
        KArc::new(
            FenceDomain {
                tq,
                pri,
                deferred: DeferList::new(),
                drain_scheduled: AtomicBool::new(false),
                stats: SyncStats::default(),
                kstat,
            },
            ctx,
        )
    }

    /// Creates a domain for host tests, without a task queue; parked fences
    /// are destroyed by [`FenceDomain::drain`].
    #[cfg(test)]
    pub fn new_test(ctx: &Sleepable) -> KArc<FenceDomain> {
        KArc::new(
            FenceDomain {
                pri: None,
                deferred: DeferList::new(),
                drain_scheduled: AtomicBool::new(false),
                stats: SyncStats::default(),
            },
            ctx,
        )
        .ok()
        .unwrap()
    }

    /// Returns the interrupt priority of the domain's locks.
    pub fn priority(&self) -> Option<IntrPriority> {
        self.pri
    }

    /// Allocates `n` consecutive fence contexts and returns the first
    /// (Linux's `dma_fence_context_alloc`).
    pub fn alloc_contexts(&self, n: u64) -> u64 {
        NEXT_CONTEXT.fetch_add(n.max(1), Ordering::Relaxed)
    }

    fn defer(&self, last: KArcLast<FenceInner>) {
        SyncStats::inc(&self.stats.deferred);
        self.deferred.push(last);
        if self.drain_scheduled.swap(true, Ordering::AcqRel) {
            return;
        }
        #[cfg(not(test))]
        {
            let me = DomainPtr(self as *const FenceDomain);
            let task = move |ctx: &Sleepable| {
                let me = me;
                // SAFETY: see DomainPtr.
                let d = unsafe { &*me.0 };
                SyncStats::inc(&d.stats.drains);
                d.drain_scheduled.store(false, Ordering::Release);
                d.drain(ctx);
            };
            if self.tq.try_dispatch(task).is_err() {
                // No memory for the task now; the next thread-context
                // entry point or the detach drains.
                self.drain_scheduled.store(false, Ordering::Release);
            }
        }
    }

    /// Destroys every parked fence, including those that destroying others
    /// parks, and publishes the counters. Thread context; cheap when
    /// nothing is parked.
    pub fn drain(&self, ctx: &Sleepable) {
        if self.deferred.is_empty() {
            return;
        }
        while !self.deferred.is_empty() {
            drop(self.deferred.take_all(ctx));
        }
        #[cfg(test)]
        self.drain_scheduled.store(false, Ordering::Release);
        self.publish();
    }

    /// Copies the counters to the `sync` kstat. Thread context (the kstat
    /// mutex is adaptive); a no-op on the host.
    pub fn publish(&self) {
        #[cfg(not(test))]
        if let Some(k) = &self.kstat {
            let s = &self.stats;
            let l = |a: &AtomicU64| a.load(Ordering::Relaxed);
            let mut g = k.lock();
            g.fences.set(l(&s.fences));
            g.signaled.set(l(&s.signaled));
            g.errors.set(l(&s.errors));
            g.callbacks.set(l(&s.callbacks));
            g.arrays.set(l(&s.arrays));
            g.chains.set(l(&s.chains));
            g.deferred.set(l(&s.deferred));
            g.drains.set(l(&s.drains));
            g.waits.set(l(&s.waits));
            g.timeouts.set(l(&s.timeouts));
            g.interrupted.set(l(&s.interrupted));
            g.syncobjs.set(l(&s.syncobjs));
            g.sync_files.set(l(&s.sync_files));
            g.dmabufs.set(l(&s.dmabufs));
            g.anon_files.set(l(&s.anon_files));
        }
    }

    /// Prepares for detach: waits for the drain task, destroys what is
    /// parked, and reports whether anything is left: a live fence, or a
    /// reference to the domain other than the caller's, such as the one an
    /// interrupt-context release holds while it parks a fence and
    /// dispatches the drain (see `Fence::defer_put`). A release racing with
    /// detach therefore makes the detach fail rather than free the domain
    /// under it; drivers stop their signallers in `before_detach`.
    #[cfg(not(test))]
    pub fn quiesce(this: &KArc<FenceDomain>, ctx: &Sleepable) -> Result<(), [u64; 8]> {
        this.tq.wait(ctx);
        this.drain(ctx);
        let live = this.stats.live();
        if live.iter().any(|n| *n != 0) || KArc::strong_count(this) != 1 {
            return Err(live);
        }
        Ok(())
    }
}

// --- callbacks --------------------------------------------------------------

/// Something to run when a fence signals. See the module documentation.
pub trait FenceCallback: Send + Sync + 'static {
    /// Runs once, when `fence` signals, in the signaller's context `ctx`,
    /// without any lock of the fence held. Must not block. May return a
    /// fence to signal next (arrays and chains do).
    fn signaled(&self, fence: &Fence, ctx: Ctx<'_>) -> Option<Fence>;
}

const CB_IDLE: u8 = 0;
const CB_LISTED: u8 = 1;
const CB_RUNNING: u8 = 2;
const CB_DONE: u8 = 3;
const CB_REMOVED: u8 = 4;

/// The type-independent head of a callback node, first in the allocation.
#[repr(C)]
struct CbHdr {
    /// The next node on the fence's list; changed with the fence locked,
    /// or by the signaller once it owns the detached list. Atomic because
    /// shared references to the header exist while it changes.
    next: AtomicPtr<CbHdr>,
    /// The fence the node was added to, for `remove_callback`.
    owner: AtomicPtr<FenceInner>,
    state: AtomicU8,
    call: unsafe fn(NonNull<CbHdr>, &Fence, Ctx<'_>) -> Option<Fence>,
    release: unsafe fn(NonNull<CbHdr>),
}

#[repr(C)]
struct CbNode<T: FenceCallback> {
    hdr: CbHdr,
    value: T,
}

unsafe fn cb_call<T: FenceCallback>(h: NonNull<CbHdr>, f: &Fence, ctx: Ctx<'_>) -> Option<Fence> {
    // SAFETY: the header is the first field of a live CbNode<T> (the list
    // holds a reference).
    let node = unsafe { h.cast::<CbNode<T>>().as_ref() };
    node.value.signaled(f, ctx)
}

unsafe fn cb_release<T: FenceCallback>(h: NonNull<CbHdr>) {
    // SAFETY: the list's reference, from KArc::into_raw in add_callback.
    drop(unsafe { KArc::from_raw(h.cast::<CbNode<T>>()) });
}

/// A callback node, allocated ahead of time so that signalling never
/// allocates. The handle keeps its own reference; a registered node is
/// also referenced by the fence until the callback has run or been
/// removed. A node can be registered once.
pub struct FenceCb<T: FenceCallback> {
    node: KArc<CbNode<T>>,
}

impl<T: FenceCallback> FenceCb<T> {
    fn wrap(value: T) -> CbNode<T> {
        CbNode {
            hdr: CbHdr {
                next: AtomicPtr::new(ptr::null_mut()),
                owner: AtomicPtr::new(ptr::null_mut()),
                state: AtomicU8::new(CB_IDLE),
                call: cb_call::<T>,
                release: cb_release::<T>,
            },
            value,
        }
    }
    /// Allocates a node with `KM_SLEEP`.
    pub fn new(value: T, ctx: &Sleepable) -> Result<FenceCb<T>, Errno> {
        Ok(FenceCb { node: KArc::new(Self::wrap(value), ctx)? })
    }
    /// Allocates a node with `KM_NOSLEEP`, from any context.
    pub fn try_new(value: T) -> Result<FenceCb<T>, T> {
        KArc::try_new(Self::wrap(value)).map(|node| FenceCb { node }).map_err(|n| n.value)
    }
    /// Returns the callback value.
    pub fn get(&self) -> &T {
        &self.node.value
    }
    /// Reports whether the callback has run to completion.
    pub fn has_run(&self) -> bool {
        self.node.hdr.state.load(Ordering::Acquire) == CB_DONE
    }
    fn hdr(&self) -> NonNull<CbHdr> {
        KArc::as_ptr(&self.node).cast()
    }
}

// SAFETY: the node is shared between the owner and the signaller; T is
// Send + Sync and the header is only changed with atomics or under the
// fence lock.
unsafe impl<T: FenceCallback> Send for CbNode<T> {}
// SAFETY: as above.
unsafe impl<T: FenceCallback> Sync for CbNode<T> {}

/// Why [`Fence::add_callback`] refused a callback.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AddError {
    /// The fence has already signalled (Linux `-ENOENT`); the callback will
    /// not run.
    Signaled,
    /// The node is or was registered already.
    Busy,
}

// --- the fence --------------------------------------------------------------

const SIGNALED: u32 = 1 << 0;
const QUEUED: u32 = 1 << 1;

struct State {
    /// Callbacks, most recently added first.
    cbs: *mut CbHdr,
    timestamp: Nanos,
}

// SAFETY: the raw pointers are nodes the list owns references to.
unsafe impl Send for State {}

/// What kind of fence it is.
enum Kind {
    Plain,
    Array(ArrayData),
    Chain(ChainData),
}

struct ArrayData {
    fences: KVec<Fence>,
    pending: AtomicU32,
}

struct ChainData {
    /// The previous point, dropped once this point has signalled.
    prev: KMutex<Option<Fence>>,
    /// This point's own fence.
    fence: KMutex<Option<Fence>>,
    /// The number of the previous point, 0 if there is none or it belongs
    /// to another context.
    prev_seqno: u64,
    pending: AtomicU32,
}

/// The shared part of a fence. See the module documentation.
pub struct FenceInner {
    link: DeferLink,
    /// The signalling worklist and the destruction stack link through here.
    work_next: AtomicPtr<FenceInner>,
    domain: KArc<FenceDomain>,
    context: u64,
    seqno: u64,
    flags: AtomicU32,
    /// 0, or the negative errno the fence signalled with.
    error: AtomicI32,
    state: KMutex<State>,
    cv: KCondVar,
    kind: Kind,
}

impl Deferrable for FenceInner {
    fn defer_link(&self) -> &DeferLink {
        &self.link
    }
}

impl FenceInner {
    /// Moves the fences this one holds out, for destruction.
    fn take_children(&mut self, out: &mut impl FnMut(Fence)) {
        match &mut self.kind {
            Kind::Plain => {}
            Kind::Array(a) => {
                while let Some(f) = a.fences.swap_remove(a.fences.len().wrapping_sub(1)) {
                    out(f);
                }
            }
            Kind::Chain(c) => {
                if let Some(p) = c.prev.lock().take() {
                    out(p);
                }
                if let Some(f) = c.fence.lock().take() {
                    out(f);
                }
            }
        }
    }
}

impl Drop for FenceInner {
    /// Runs in thread context only: a fence's last reference is released
    /// by [`Fence::put`] with `Ctx::Sleep` or by the domain's drain (see
    /// the module documentation). The fences it holds are released here
    /// too, iteratively: a fence whose last reference goes is linked onto a
    /// local stack through `work_next` instead of being dropped from inside
    /// this drop.
    fn drop(&mut self) {
        let stats = &self.domain.stats;
        SyncStats::dec(&stats.fences);
        match self.kind {
            Kind::Plain => {}
            Kind::Array(_) => SyncStats::dec(&stats.arrays),
            Kind::Chain(_) => SyncStats::dec(&stats.chains),
        }
        // Callbacks that never ran (the fence was not signalled) are
        // dropped with it.
        let mut cb = self.state.lock().cbs;
        while let Some(h) = NonNull::new(cb) {
            // SAFETY: nodes on the list are live and referenced by it.
            unsafe {
                cb = (*h.as_ptr()).next.load(Ordering::Relaxed);
                SyncStats::dec(&stats.callbacks);
                (*h.as_ptr()).state.store(CB_REMOVED, Ordering::Release);
                ((*h.as_ptr()).release)(h);
            }
        }
        let mut stack = DeadStack(ptr::null_mut());
        self.take_children(&mut |f| stack.push(f));
        while let Some(mut last) = stack.pop() {
            KArcLast::get_mut(&mut last).take_children(&mut |f| stack.push(f));
            // Its own drop finds no children left.
            drop(last);
        }
    }
}

/// Fences whose last reference went while another fence was destroyed,
/// linked through `work_next` (free once a fence is dead).
struct DeadStack(*mut FenceInner);

impl DeadStack {
    fn push(&mut self, f: Fence) {
        if let Some(last) = KArc::release(f.into_arc()) {
            let p = KArcLast::into_raw(last);
            // SAFETY: we own the dead fence; its link is ours.
            unsafe { p.as_ref() }.work_next.store(self.0, Ordering::Relaxed);
            self.0 = p.as_ptr();
        }
    }
    fn pop(&mut self) -> Option<KArcLast<FenceInner>> {
        let p = NonNull::new(self.0)?;
        // SAFETY: pushed from KArcLast::into_raw above, taken once.
        let last = unsafe { KArcLast::from_raw(p) };
        self.0 = last.work_next.load(Ordering::Relaxed);
        Some(last)
    }
}

/// A counted reference to a fence. See the module documentation for how to
/// drop it; an implicit drop takes the interrupt-safe path.
pub struct Fence {
    arc: ManuallyDrop<KArc<FenceInner>>,
}

impl Clone for Fence {
    fn clone(&self) -> Fence {
        Fence { arc: ManuallyDrop::new(KArc::clone(&self.arc)) }
    }
}

impl Drop for Fence {
    fn drop(&mut self) {
        // SAFETY: taken once, in drop.
        let arc = unsafe { ManuallyDrop::take(&mut self.arc) };
        Fence::defer_put(arc);
    }
}

impl core::fmt::Debug for Fence {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Fence")
            .field("context", &self.context())
            .field("seqno", &self.seqno())
            .field("signaled", &self.is_signaled())
            .finish()
    }
}

impl Fence {
    fn from_arc(arc: KArc<FenceInner>) -> Fence {
        Fence { arc: ManuallyDrop::new(arc) }
    }

    fn into_arc(self) -> KArc<FenceInner> {
        let mut me = ManuallyDrop::new(self);
        // SAFETY: taken once; `me` is never dropped.
        unsafe { ManuallyDrop::take(&mut me.arc) }
    }

    fn inner(&self) -> &FenceInner {
        &self.arc
    }

    fn defer_put(arc: KArc<FenceInner>) {
        if let Some(last) = KArc::release(arc) {
            // Once `last` is on the list, a drain on another processor may
            // destroy it and with it its domain reference, so a reference of
            // our own keeps the domain alive until `defer` has returned. It
            // cannot be the last one: the device holds one, and
            // `FenceDomain::quiesce` refuses while any other exists, so this
            // drop only decrements (as `GemRef` does for the GEM state).
            let d = KArc::clone(&last.domain);
            d.defer(last);
            drop(d);
        }
    }

    /// Drops the reference in the context `ctx`: at once in thread context,
    /// parked for the domain's task queue otherwise.
    pub fn put(self, ctx: Ctx<'_>) {
        match ctx {
            Ctx::Sleep(_) => {
                if let Some(last) = KArc::release(self.into_arc()) {
                    drop(last);
                }
            }
            Ctx::Atomic(_) => Fence::defer_put(self.into_arc()),
        }
    }

    fn build(domain: &KArc<FenceDomain>, context: u64, seqno: u64, kind: Kind) -> FenceInner {
        FenceInner {
            link: DeferLink::new(),
            work_next: AtomicPtr::new(ptr::null_mut()),
            domain: KArc::clone(domain),
            context,
            seqno,
            flags: AtomicU32::new(0),
            error: AtomicI32::new(0),
            state: KMutex::with_priority(State { cbs: ptr::null_mut(), timestamp: 0 }, domain.pri),
            cv: KCondVar::new(),
            kind,
        }
    }

    fn counted(inner: KArc<FenceInner>) -> Fence {
        let s = &inner.domain.stats;
        SyncStats::inc(&s.fences);
        match inner.kind {
            Kind::Plain => {}
            Kind::Array(_) => SyncStats::inc(&s.arrays),
            Kind::Chain(_) => SyncStats::inc(&s.chains),
        }
        Fence::from_arc(inner)
    }

    /// Creates an unsignalled fence with `KM_SLEEP`.
    pub fn new(domain: &KArc<FenceDomain>, context: u64, seqno: u64, ctx: &Sleepable) -> Result<Fence, Errno> {
        Ok(Fence::counted(KArc::new(Self::build(domain, context, seqno, Kind::Plain), ctx)?))
    }

    /// Creates an unsignalled fence with `KM_NOSLEEP`, from any context;
    /// `ENOMEM` if memory is short.
    pub fn try_new(domain: &KArc<FenceDomain>, context: u64, seqno: u64) -> Result<Fence, Errno> {
        let inner = KArc::try_new(Self::build(domain, context, seqno, Kind::Plain)).map_err(|_| Errno::ENOMEM)?;
        Ok(Fence::counted(inner))
    }

    /// Creates a fence that is already signalled, the stand-in for "no work
    /// pending" (Linux's `dma_fence_get_stub`), on a context of its own.
    pub fn signaled_stub(domain: &KArc<FenceDomain>, ctx: &Sleepable) -> Result<Fence, Errno> {
        let f = Fence::new(domain, domain.alloc_contexts(1), 0, ctx)?;
        f.signal(Ctx::Sleep(ctx));
        Ok(f)
    }

    /// Returns the fence's domain.
    pub fn domain(&self) -> &KArc<FenceDomain> {
        &self.inner().domain
    }
    /// Returns the context.
    pub fn context(&self) -> u64 {
        self.inner().context
    }
    /// Returns the sequence number.
    pub fn seqno(&self) -> u64 {
        self.inner().seqno
    }
    /// Reports whether both refer to the same fence.
    pub fn ptr_eq(a: &Fence, b: &Fence) -> bool {
        KArc::ptr_eq(&a.arc, &b.arc)
    }
    /// Reports whether the fence has signalled.
    pub fn is_signaled(&self) -> bool {
        self.inner().flags.load(Ordering::Acquire) & SIGNALED != 0
    }
    /// Returns the error the fence signalled with, `None` for success or
    /// while it is unsignalled.
    pub fn error(&self) -> Option<Errno> {
        if !self.is_signaled() {
            return None;
        }
        Errno::new(-self.inner().error.load(Ordering::Acquire))
    }
    /// Returns the monotonic time at which the fence signalled, `None` while
    /// it is unsignalled.
    pub fn timestamp(&self) -> Option<Nanos> {
        if !self.is_signaled() {
            return None;
        }
        Some(self.inner().state.lock().timestamp)
    }
    /// Reports whether this fence comes after `other` on the same context,
    /// wraparound-safe: the difference of the sequence numbers is read as a
    /// signed 64-bit number. Fences of different contexts are never later.
    pub fn is_later(&self, other: &Fence) -> bool {
        self.context() == other.context() && seqno_later(self.seqno(), other.seqno())
    }

    /// Sets the error the fence will signal with (Linux's
    /// `dma_fence_set_error`); the first error wins, and a signalled fence
    /// keeps its state.
    pub fn set_error(&self, e: Errno) {
        if self.is_signaled() {
            return;
        }
        let _ = self.inner().error.compare_exchange(0, -e.get(), Ordering::AcqRel, Ordering::Acquire);
    }

    /// Signals the fence and everything that completes because of it (see
    /// the module documentation). Returns `false` if it had signalled, or
    /// was being signalled, already.
    pub fn signal(&self, ctx: Ctx<'_>) -> bool {
        let mut work = Worklist::new();
        if !work.push(self.clone(), ctx) {
            return false;
        }
        work.run(ctx);
        true
    }

    /// Signals the fence with the error `e`.
    pub fn signal_with_error(&self, e: Errno, ctx: Ctx<'_>) -> bool {
        self.set_error(e);
        self.signal(ctx)
    }

    /// Marks the fence signalled under its lock, wakes the waiters and
    /// returns the callbacks in the order they were added, each marked
    /// running.
    fn mark_signaled(&self) -> *mut CbHdr {
        let inner = self.inner();
        let mut st = inner.state.lock();
        st.timestamp = ksync::now();
        inner.flags.fetch_or(SIGNALED, Ordering::AcqRel);
        let stats = &inner.domain.stats;
        SyncStats::inc(&stats.signaled);
        if inner.error.load(Ordering::Acquire) != 0 {
            SyncStats::inc(&stats.errors);
        }
        // Reverse the list into adding order and mark the nodes.
        let mut rev: *mut CbHdr = ptr::null_mut();
        let mut cur = core::mem::replace(&mut st.cbs, ptr::null_mut());
        while let Some(h) = NonNull::new(cur) {
            // SAFETY: nodes on the list are live and referenced by it; they
            // change only with the lock held, which it is.
            unsafe {
                cur = (*h.as_ptr()).next.load(Ordering::Relaxed);
                (*h.as_ptr()).next.store(rev, Ordering::Relaxed);
                (*h.as_ptr()).state.store(CB_RUNNING, Ordering::Release);
            }
            rev = h.as_ptr();
        }
        inner.cv.notify_all();
        drop(st);
        rev
    }

    /// Registers `cb` to run when the fence signals. See the module
    /// documentation.
    pub fn add_callback<T: FenceCallback>(&self, cb: &FenceCb<T>) -> Result<(), AddError> {
        let inner = self.inner();
        let h = cb.hdr();
        // SAFETY: the node is live (the handle holds it).
        let hdr = unsafe { h.as_ref() };
        let mut st = inner.state.lock();
        if inner.flags.load(Ordering::Acquire) & SIGNALED != 0 {
            return Err(AddError::Signaled);
        }
        if hdr.state.compare_exchange(CB_IDLE, CB_LISTED, Ordering::AcqRel, Ordering::Acquire).is_err() {
            return Err(AddError::Busy);
        }
        let list_ref = KArc::into_raw(KArc::clone(&cb.node)).cast::<CbHdr>();
        // SAFETY: the node is ours to link while the fence is locked.
        unsafe {
            (*list_ref.as_ptr()).next.store(st.cbs, Ordering::Relaxed);
            (*list_ref.as_ptr()).owner.store(inner as *const FenceInner as *mut FenceInner, Ordering::Release);
        }
        st.cbs = list_ref.as_ptr();
        SyncStats::inc(&inner.domain.stats.callbacks);
        Ok(())
    }

    /// Unregisters `cb`. Returns `true` if it was removed before running;
    /// `false` if it has run (this waits until a running callback has
    /// finished) or was never registered with this fence.
    pub fn remove_callback<T: FenceCallback>(&self, cb: &FenceCb<T>, ctx: &Sleepable) -> bool {
        self.remove_callback_with(cb, || ksync::yield_briefly(ctx))
    }

    /// As [`Fence::remove_callback`], for a caller without the token, such
    /// as a destructor that runs in thread context: while the callback
    /// runs on another processor, this spins. Callbacks do not block, so
    /// the spin is short. Never call it from the fence's own callback.
    pub fn remove_callback_spin<T: FenceCallback>(&self, cb: &FenceCb<T>) -> bool {
        self.remove_callback_with(cb, core::hint::spin_loop)
    }

    fn remove_callback_with<T: FenceCallback>(&self, cb: &FenceCb<T>, pause: impl Fn()) -> bool {
        let inner = self.inner();
        let h = cb.hdr();
        // SAFETY: the handle keeps the node alive.
        let hdr = unsafe { h.as_ref() };
        if !ptr::eq(hdr.owner.load(Ordering::Acquire), inner) {
            return false;
        }
        {
            let mut st = inner.state.lock();
            if hdr.state.load(Ordering::Acquire) == CB_LISTED {
                // Unlink `h`: from the head, or from its predecessor.
                let mut prev: Option<NonNull<CbHdr>> = None;
                let mut cur = st.cbs;
                while let Some(c) = NonNull::new(cur) {
                    // SAFETY: nodes on the list are live and referenced by
                    // it; their links change only with the lock held.
                    let next = unsafe { c.as_ref() }.next.load(Ordering::Relaxed);
                    if c == h {
                        match prev {
                            // SAFETY: as above.
                            Some(p) => unsafe { p.as_ref() }.next.store(next, Ordering::Relaxed),
                            None => st.cbs = next,
                        }
                        hdr.state.store(CB_REMOVED, Ordering::Release);
                        SyncStats::dec(&inner.domain.stats.callbacks);
                        drop(st);
                        // SAFETY: the list's reference, now unlinked.
                        unsafe { (hdr.release)(h) };
                        return true;
                    }
                    prev = Some(c);
                    cur = next;
                }
            }
        }
        while hdr.state.load(Ordering::Acquire) == CB_RUNNING {
            pause();
        }
        false
    }

    /// Waits until the fence has signalled. `deadline` is an absolute time
    /// on the monotonic clock (`None`: no limit); with `intr` a signal may
    /// end the wait. `ETIME` on timeout, `EINTR` on a signal. The fence's
    /// error does not make the wait fail; see [`Fence::error`].
    pub fn wait(&self, intr: bool, deadline: Option<Nanos>, ctx: &Sleepable) -> Result<(), Errno> {
        if self.is_signaled() {
            return Ok(());
        }
        let inner = self.inner();
        let stats = &inner.domain.stats;
        SyncStats::inc(&stats.waits);
        let mut g = inner.state.lock();
        loop {
            if inner.flags.load(Ordering::Acquire) & SIGNALED != 0 {
                return Ok(());
            }
            let (ng, w) = inner.cv.wait(g, deadline, intr, ctx);
            g = ng;
            if let Some(e) = w.errno() {
                if inner.flags.load(Ordering::Acquire) & SIGNALED != 0 {
                    return Ok(());
                }
                SyncStats::inc(if w == Wake::TimedOut { &stats.timeouts } else { &stats.interrupted });
                return Err(e);
            }
        }
    }

    // --- arrays ---

    /// Creates a fence that signals when every fence of `fences` has, or,
    /// with `signal_on_any`, when the first has (Linux's
    /// `dma_fence_array_create`), with the first error among them. An empty
    /// array signals at once.
    pub fn array(
        domain: &KArc<FenceDomain>,
        fences: KVec<Fence>,
        context: u64,
        seqno: u64,
        signal_on_any: bool,
        ctx: &Sleepable,
    ) -> Result<Fence, Errno> {
        let n = u32::try_from(fences.len()).map_err(|_| Errno::EINVAL)?;
        let pending = if signal_on_any { n.min(1) } else { n };
        // Callback nodes first, so that a failed allocation leaves nothing
        // registered.
        let mut cbs: KVec<FenceCb<Completer>> = KVec::new();
        for _ in 0..fences.len() {
            cbs.push(FenceCb::new(Completer { target: AtomicPtr::new(ptr::null_mut()) }, ctx)?, ctx)?;
        }
        let data = ArrayData { fences, pending: AtomicU32::new(pending.max(1)) };
        let arr = Fence::counted(KArc::new(Self::build(domain, context, seqno, Kind::Array(data)), ctx)?);
        let Kind::Array(a) = &arr.inner().kind else { return Err(Errno::EINVAL) };
        let mut done = pending == 0;
        for (f, cb) in a.fences.iter().zip(cbs.iter()) {
            cb.get().set_target(&arr);
            match f.add_callback(cb) {
                Ok(()) => {}
                Err(_) => {
                    cb.get().clear_target();
                    if complete_one(&arr, f) {
                        done = true;
                    }
                }
            }
        }
        if done {
            arr.signal(Ctx::Sleep(ctx));
        }
        Ok(arr)
    }

    /// Returns the member fences of an array, `None` for other fences.
    pub fn array_members(&self) -> Option<&[Fence]> {
        match &self.inner().kind {
            Kind::Array(a) => Some(&a.fences),
            _ => None,
        }
    }

    // --- chains ---

    /// Creates the chain point `seqno` of a timeline whose previous point is
    /// `prev` (Linux's `dma_fence_chain_init`): it signals when `fence` and
    /// `prev` have, with `fence`'s error. If `prev` is a chain point with an
    /// earlier number, the new point shares its context and records its
    /// number as the previous one; otherwise it gets a context of its own
    /// and, after a chain point, the larger of both numbers.
    pub fn chain(
        domain: &KArc<FenceDomain>,
        prev: Option<Fence>,
        fence: Fence,
        seqno: u64,
        ctx: &Sleepable,
    ) -> Result<Fence, Errno> {
        let cb_fence = FenceCb::new(Completer { target: AtomicPtr::new(ptr::null_mut()) }, ctx)?;
        let cb_prev = FenceCb::new(Completer { target: AtomicPtr::new(ptr::null_mut()) }, ctx)?;
        let prev_chain = prev.as_ref().filter(|p| p.is_chain());
        let (context, seqno, prev_seqno) = match prev_chain {
            Some(p) if seqno > p.seqno() => (p.context(), seqno, p.seqno()),
            Some(p) => (domain.alloc_contexts(1), seqno.max(p.seqno()), 0),
            None => (domain.alloc_contexts(1), seqno, 0),
        };
        let pending = 1 + u32::from(prev.is_some());
        let own = fence.clone();
        let prev_ref = prev.clone();
        let data = ChainData {
            prev: KMutex::with_priority(prev, domain.pri),
            fence: KMutex::with_priority(Some(fence), domain.pri),
            prev_seqno,
            pending: AtomicU32::new(pending),
        };
        let node = Fence::counted(KArc::new(Self::build(domain, context, seqno, Kind::Chain(data)), ctx)?);
        let mut done = false;
        let mut watch = |src: &Fence, cb: &FenceCb<Completer>| {
            cb.get().set_target(&node);
            if src.add_callback(cb).is_err() {
                cb.get().clear_target();
                if complete_one(&node, src) {
                    done = true;
                }
            }
        };
        watch(&own, &cb_fence);
        if let Some(p) = &prev_ref {
            watch(p, &cb_prev);
        }
        own.put(Ctx::Sleep(ctx));
        if let Some(p) = prev_ref {
            p.put(Ctx::Sleep(ctx));
        }
        if done {
            node.signal(Ctx::Sleep(ctx));
        }
        Ok(node)
    }

    /// Reports whether this is a chain point.
    pub fn is_chain(&self) -> bool {
        matches!(self.inner().kind, Kind::Chain(_))
    }

    fn chain_data(&self) -> Option<&ChainData> {
        match &self.inner().kind {
            Kind::Chain(c) => Some(c),
            _ => None,
        }
    }

    /// Returns the previous point of a chain point, while it is kept.
    pub fn chain_prev(&self) -> Option<Fence> {
        self.chain_data().and_then(|c| c.prev.lock().clone())
    }

    /// Returns the own fence of a chain point, while it is unsignalled.
    pub fn chain_fence(&self) -> Option<Fence> {
        self.chain_data().and_then(|c| c.fence.lock().clone())
    }

    /// Returns the number of the previous point of a chain point.
    pub fn chain_prev_seqno(&self) -> Option<u64> {
        self.chain_data().map(|c| c.prev_seqno)
    }
}

/// Reports whether `a` is later than `b` on one context: the difference
/// read as a signed 64-bit number is positive.
pub fn seqno_later(a: u64, b: u64) -> bool {
    (a.wrapping_sub(b) as i64) > 0
}

/// The callback that completes an array or a chain point: it counts the
/// member down and returns the target to signal when it was the last.
/// Holds a counted reference to the target (a [`Fence`], whose drop is
/// interrupt-safe) as a raw pointer so that it can be set after the target
/// exists.
struct Completer {
    target: AtomicPtr<FenceInner>,
}

impl Completer {
    fn set_target(&self, t: &Fence) {
        let p = KArc::into_raw(KArc::clone(&t.arc));
        self.target.store(p.as_ptr(), Ordering::Release);
    }
    fn clear_target(&self) {
        let p = self.target.swap(ptr::null_mut(), Ordering::AcqRel);
        if let Some(p) = NonNull::new(p) {
            // SAFETY: from set_target, taken once.
            drop(Fence::from_arc(unsafe { KArc::from_raw(p) }));
        }
    }
}

impl Drop for Completer {
    fn drop(&mut self) {
        self.clear_target();
    }
}

impl FenceCallback for Completer {
    fn signaled(&self, fence: &Fence, ctx: Ctx<'_>) -> Option<Fence> {
        let p = NonNull::new(self.target.swap(ptr::null_mut(), Ordering::AcqRel))?;
        // SAFETY: from set_target, taken once.
        let target = Fence::from_arc(unsafe { KArc::from_raw(p) });
        let done = complete_one(&target, fence);
        if let Some(c) = target.chain_data() {
            // The previous point has been reached: the link is no longer
            // needed (Linux collects it on the next chain walk).
            let prev = {
                let mut g = c.prev.lock();
                if g.as_ref().is_some_and(|p| Fence::ptr_eq(p, fence)) { g.take() } else { None }
            };
            if let Some(p) = prev {
                p.put(ctx);
            }
        }
        if done {
            return Some(target);
        }
        target.put(ctx);
        None
    }
}

/// Accounts for one signalled member `src` of the array or chain point
/// `target`; returns `true` when that completed the target.
fn complete_one(target: &Fence, src: &Fence) -> bool {
    let pending = match &target.inner().kind {
        Kind::Array(a) => &a.pending,
        Kind::Chain(c) => {
            // A chain point takes the error of its own fence only.
            let own = c.fence.lock().as_ref().is_some_and(|f| Fence::ptr_eq(f, src));
            if own && let Some(e) = src.error() {
                target.set_error(e);
            }
            &c.pending
        }
        Kind::Plain => return false,
    };
    if matches!(target.inner().kind, Kind::Array(_))
        && let Some(e) = src.error()
    {
        target.set_error(e);
    }
    pending.fetch_update(Ordering::AcqRel, Ordering::Acquire, |n| n.checked_sub(1)).is_ok_and(|n| n == 1)
}

/// The fences a signal is working through, linked through `work_next`,
/// each holding a reference.
struct Worklist {
    head: *mut FenceInner,
}

impl Worklist {
    fn new() -> Worklist {
        Worklist { head: ptr::null_mut() }
    }

    /// Queues `f` unless it is queued or signalled already, in which case
    /// the reference is dropped and `false` returned.
    fn push(&mut self, f: Fence, ctx: Ctx<'_>) -> bool {
        if f.inner().flags.fetch_or(QUEUED, Ordering::AcqRel) & (QUEUED | SIGNALED) != 0 {
            f.put(ctx);
            return false;
        }
        let p = KArc::into_raw(f.into_arc());
        // SAFETY: the QUEUED bit makes this list the link's only user.
        unsafe { p.as_ref() }.work_next.store(self.head, Ordering::Relaxed);
        self.head = p.as_ptr();
        true
    }

    fn run(&mut self, ctx: Ctx<'_>) {
        while let Some(p) = NonNull::new(self.head) {
            // SAFETY: pushed from KArc::into_raw, taken once.
            let f = Fence::from_arc(unsafe { KArc::from_raw(p) });
            self.head = f.inner().work_next.swap(ptr::null_mut(), Ordering::Relaxed);
            let mut cb = f.mark_signaled();
            if let Kind::Chain(c) = &f.inner().kind {
                // A signalled point needs neither link any more; dropping
                // them keeps timelines short (Linux's chain walk does the
                // same garbage collection).
                let prev = c.prev.lock().take();
                let own = c.fence.lock().take();
                if let Some(x) = prev {
                    x.put(ctx);
                }
                if let Some(x) = own {
                    x.put(ctx);
                }
            }
            while let Some(h) = NonNull::new(cb) {
                // SAFETY: taken off the list by mark_signaled; the list's
                // reference is ours now.
                let next = unsafe {
                    cb = (*h.as_ptr()).next.load(Ordering::Relaxed);
                    ((*h.as_ptr()).call)(h, &f, ctx)
                };
                // SAFETY: as above. DONE is published before the reference
                // is dropped, and the owner's handle keeps the node alive for
                // remove_callback.
                unsafe {
                    (*h.as_ptr()).state.store(CB_DONE, Ordering::Release);
                    SyncStats::dec(&f.inner().domain.stats.callbacks);
                    ((*h.as_ptr()).release)(h);
                }
                if let Some(n) = next {
                    self.push(n, ctx);
                }
            }
            f.put(ctx);
        }
    }
}

// --- chain queries ---------------------------------------------------------

/// Returns the fence that signals when point `point` of the timeline whose
/// latest point is `head` has been reached (Linux's
/// `dma_fence_chain_find_seqno`): `head` itself for point 0, the earliest
/// point on the same context that covers `point`, the fence before the
/// timeline for a point before its first, or `None` if the point has
/// certainly been reached. `EINVAL` if `head` is not a chain point or
/// `point` is beyond it.
pub fn chain_find_seqno(head: &Fence, point: u64) -> Result<Option<Fence>, Errno> {
    if point == 0 {
        return Ok(Some(head.clone()));
    }
    let Some(hc) = head.chain_data() else { return Err(Errno::EINVAL) };
    if head.seqno() < point {
        return Err(Errno::EINVAL);
    }
    let mut node = head.clone();
    let mut prev_seqno = hc.prev_seqno;
    loop {
        if prev_seqno < point {
            return Ok(Some(node));
        }
        let Some(prev) = node.chain_prev() else {
            // Dropped because the point signalled: everything up to it has
            // been reached.
            return Ok(None);
        };
        if prev.is_signaled() {
            return Ok(None);
        }
        match prev.chain_data() {
            Some(pc) if prev.context() == node.context() => {
                prev_seqno = pc.prev_seqno;
                node = prev;
            }
            // The timeline started after a fence of another context.
            _ => return Ok(Some(prev)),
        }
    }
}

/// Returns the last point of the timeline whose latest point is `head`
/// that has been reached, as `SYNCOBJ_QUERY` reports it; with
/// `last_submitted`, the number of the latest point. 0 for a fence that is
/// not a chain point.
pub fn chain_query(head: &Fence, last_submitted: bool) -> u64 {
    let Some(hc) = head.chain_data() else { return 0 };
    if last_submitted || head.is_signaled() {
        return head.seqno();
    }
    // Find the oldest unsignalled point on the same context: everything
    // before it has been reached.
    let mut node_prev_seqno = hc.prev_seqno;
    let mut node = head.clone();
    while let Some(prev) = node.chain_prev() {
        if prev.is_signaled() || prev.context() != head.context() {
            break;
        }
        let Some(pc) = prev.chain_data() else { break };
        node_prev_seqno = pc.prev_seqno;
        node = prev;
    }
    node_prev_seqno
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::sync::Arc;
    use std::sync::atomic::AtomicUsize;
    use std::thread;
    use std::vec::Vec;

    pub(crate) fn sleep() -> Sleepable {
        // SAFETY: host test threads may block.
        unsafe { Sleepable::assume() }
    }
    fn atomic() -> ddi::Atomic {
        // SAFETY: a host test simulating interrupt context.
        unsafe { ddi::Atomic::assume() }
    }

    /// Records the order callbacks run in.
    struct Record {
        id: usize,
        log: Arc<std::sync::Mutex<Vec<usize>>>,
        runs: Arc<AtomicUsize>,
    }
    impl FenceCallback for Record {
        fn signaled(&self, _f: &Fence, _ctx: Ctx<'_>) -> Option<Fence> {
            self.log.lock().unwrap().push(self.id);
            self.runs.fetch_add(1, Ordering::SeqCst);
            None
        }
    }

    fn fence(d: &KArc<FenceDomain>, s: u64) -> Fence {
        Fence::new(d, 1, s, &sleep()).unwrap()
    }

    fn finish(d: KArc<FenceDomain>) {
        d.drain(&sleep());
        assert_eq!(d.stats.live(), [0; 8], "every fence and callback is gone");
        assert_eq!(KArc::strong_count(&d), 1);
    }

    #[test]
    fn signal_runs_callbacks_once_in_order() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        let f = fence(&d, 1);
        let log = Arc::new(std::sync::Mutex::new(Vec::new()));
        let runs = Arc::new(AtomicUsize::new(0));
        let cbs: Vec<_> =
            (0..5).map(|id| FenceCb::new(Record { id, log: log.clone(), runs: runs.clone() }, &ctx).unwrap()).collect();
        for cb in &cbs {
            f.add_callback(cb).unwrap();
        }
        assert_eq!(f.add_callback(&cbs[0]), Err(AddError::Busy));
        assert!(f.remove_callback(&cbs[2], &ctx), "removed before running");
        assert!(!f.is_signaled());
        assert_eq!(f.timestamp(), None);
        assert!(f.signal(Ctx::Sleep(&ctx)));
        assert!(!f.signal(Ctx::Sleep(&ctx)), "signalled once");
        assert_eq!(*log.lock().unwrap(), [0, 1, 3, 4]);
        assert!(f.timestamp().is_some());
        let late = FenceCb::new(Record { id: 9, log: log.clone(), runs: runs.clone() }, &ctx).unwrap();
        assert_eq!(f.add_callback(&late), Err(AddError::Signaled));
        assert!(!f.remove_callback(&cbs[0], &ctx), "already ran");
        assert!(cbs[0].has_run());
        assert_eq!(runs.load(Ordering::SeqCst), 4);
        drop(cbs);
        drop(late);
        f.put(Ctx::Sleep(&ctx));
        finish(d);
    }

    #[test]
    fn errors_and_seqno_wraparound() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        let f = fence(&d, u64::MAX);
        f.set_error(Errno::EIO);
        f.set_error(Errno::EINVAL);
        assert_eq!(f.error(), None, "no error before the signal");
        f.signal(Ctx::Atomic(&atomic()));
        assert_eq!(f.error(), Some(Errno::EIO), "the first error wins");
        f.set_error(Errno::ENOMEM);
        assert_eq!(f.error(), Some(Errno::EIO), "a signalled fence keeps its error");
        let g = fence(&d, 2);
        assert!(g.is_later(&f), "2 comes after u64::MAX on the same context");
        assert!(!f.is_later(&g));
        assert!(!g.is_later(&g));
        let other = Fence::new(&d, 7, 3, &ctx).unwrap();
        assert!(!other.is_later(&f) && !f.is_later(&other), "other contexts are never later");
        assert!(seqno_later(3, 1) && !seqno_later(1, 3) && seqno_later(1, u64::MAX - 5));
        assert!(seqno_later((1 << 63) - 1 + 7, 7), "just under half the range ahead is later");
        assert!(!seqno_later((1 << 63) + 7, 7), "half the range or more is earlier");
        drop((f, g, other));
        finish(d);
    }

    #[test]
    fn waits_time_out_are_interrupted_and_complete() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        let f = fence(&d, 1);
        let soon = ksync::now() + 20_000_000;
        assert_eq!(f.wait(true, Some(soon), &ctx), Err(ksync::ETIME));
        assert_eq!(f.wait(false, Some(ksync::now() - 1), &ctx), Err(ksync::ETIME), "a past deadline polls");
        ksync::SIGNAL_PENDING.with(|s| s.set(true));
        assert_eq!(f.wait(true, None, &ctx), Err(Errno::EINTR));
        let g = f.clone();
        let t = thread::spawn(move || {
            thread::sleep(std::time::Duration::from_millis(30));
            g.signal(Ctx::Atomic(&atomic()));
            drop(g);
        });
        assert_eq!(f.wait(true, Some(ksync::now() + 5_000_000_000), &ctx), Ok(()));
        t.join().unwrap();
        assert_eq!(f.wait(true, Some(0), &ctx), Ok(()), "a signalled fence never waits");
        assert!(d.stats.timeouts.load(Ordering::SeqCst) >= 1);
        assert_eq!(d.stats.interrupted.load(Ordering::SeqCst), 1);
        f.put(Ctx::Sleep(&ctx));
        finish(d);
    }

    #[test]
    fn arrays_all_any_and_errors() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        let fs: Vec<_> = (0..4).map(|i| fence(&d, i)).collect();
        let mut members = KVec::new();
        for f in &fs {
            members.push(f.clone(), &ctx).unwrap();
        }
        let all = Fence::array(&d, members, 50, 1, false, &ctx).unwrap();
        let mut members = KVec::new();
        for f in &fs {
            members.push(f.clone(), &ctx).unwrap();
        }
        let any = Fence::array(&d, members, 51, 1, true, &ctx).unwrap();
        assert_eq!(all.array_members().map(<[Fence]>::len), Some(4));
        fs[2].signal_with_error(Errno::EIO, Ctx::Sleep(&ctx));
        assert!(any.is_signaled() && !all.is_signaled());
        assert_eq!(any.error(), Some(Errno::EIO));
        fs[0].signal(Ctx::Atomic(&atomic()));
        fs[1].signal(Ctx::Sleep(&ctx));
        assert!(!all.is_signaled());
        fs[3].signal(Ctx::Sleep(&ctx));
        assert!(all.is_signaled());
        assert_eq!(all.error(), Some(Errno::EIO), "the member's error propagates");
        // Already signalled members, and an empty array.
        let mut members = KVec::new();
        members.push(fs[0].clone(), &ctx).unwrap();
        let done = Fence::array(&d, members, 52, 1, false, &ctx).unwrap();
        assert!(done.is_signaled());
        let empty = Fence::array(&d, KVec::new(), 53, 1, false, &ctx).unwrap();
        assert!(empty.is_signaled());
        drop((all, any, done, empty, fs));
        finish(d);
    }

    /// Points signalled out of order complete the timeline in order; the
    /// query and the point lookup follow Linux's syncobj semantics.
    #[test]
    fn chain_orders_a_timeline() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        let pts: Vec<_> = (0..4).map(|i| fence(&d, i)).collect();
        let mut head: Option<Fence> = None;
        for (i, p) in pts.iter().enumerate() {
            let point = (i as u64 + 1) * 10;
            head = Some(Fence::chain(&d, head.take(), p.clone(), point, &ctx).unwrap());
        }
        let head = head.unwrap();
        assert_eq!(head.seqno(), 40);
        assert_eq!(head.chain_prev_seqno(), Some(30));
        assert_eq!(chain_query(&head, false), 0);
        assert_eq!(chain_query(&head, true), 40);
        let p25 = chain_find_seqno(&head, 25).unwrap().unwrap();
        assert_eq!(p25.seqno(), 30, "the point covering 25 is 30");
        assert_eq!(chain_find_seqno(&head, 41).err(), Some(Errno::EINVAL));
        assert!(Fence::ptr_eq(&chain_find_seqno(&head, 0).unwrap().unwrap(), &head));
        // Signal points 3 and 2 first: nothing is reached before point 1.
        pts[2].signal(Ctx::Sleep(&ctx));
        pts[1].signal(Ctx::Atomic(&atomic()));
        assert!(!p25.is_signaled());
        assert_eq!(chain_query(&head, false), 0);
        pts[0].signal(Ctx::Sleep(&ctx));
        assert!(p25.is_signaled(), "points 1 to 3 are reached");
        assert_eq!(chain_query(&head, false), 30);
        assert!(chain_find_seqno(&head, 25).unwrap().is_none(), "reached");
        assert!(!head.is_signaled());
        assert!(head.chain_prev().is_none(), "the signalled point was collected");
        pts[3].signal_with_error(Errno::ECANCELED, Ctx::Sleep(&ctx));
        assert!(head.is_signaled());
        assert_eq!(head.error(), Some(Errno::ECANCELED));
        assert_eq!(chain_query(&head, false), 40);
        // A point that is not later starts a new context.
        let x = fence(&d, 9);
        let odd = Fence::chain(&d, Some(head.clone()), x.clone(), 5, &ctx).unwrap();
        assert_ne!(odd.context(), head.context());
        assert_eq!(odd.seqno(), 40, "the larger number is kept");
        assert_eq!(chain_find_seqno(&x, 1).err(), Some(Errno::EINVAL), "not a chain");
        x.signal(Ctx::Sleep(&ctx));
        assert!(odd.is_signaled());
        drop((odd, x, p25, head, pts));
        finish(d);
    }

    /// A timeline that starts after a plain (binary) fence waits for it.
    #[test]
    fn chain_after_binary_fence() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        let bin = fence(&d, 1);
        let p = fence(&d, 2);
        let head = Fence::chain(&d, Some(bin.clone()), p.clone(), 5, &ctx).unwrap();
        assert_eq!(head.chain_prev_seqno(), Some(0));
        assert!(Fence::ptr_eq(&chain_find_seqno(&head, 3).unwrap().unwrap(), &head));
        p.signal(Ctx::Sleep(&ctx));
        assert!(!head.is_signaled(), "the binary fence is still pending");
        bin.signal(Ctx::Sleep(&ctx));
        assert!(head.is_signaled());
        drop((bin, p, head));
        finish(d);
    }

    /// A long unsignalled timeline is destroyed without recursion, and a
    /// long timeline signalled from its first point completes iteratively.
    #[test]
    fn long_chains_do_not_recurse() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        const N: u64 = 100_000;
        let first = fence(&d, 0);
        let mut head = Fence::chain(&d, None, first.clone(), 1, &ctx).unwrap();
        let mut later = Vec::new();
        for i in 2..=N {
            let p = fence(&d, i);
            p.signal(Ctx::Sleep(&ctx));
            if i % 10_000 == 0 {
                later.push(p.clone());
            }
            head = Fence::chain(&d, Some(head), p, i, &ctx).unwrap();
        }
        assert_eq!(chain_query(&head, false), 0);
        // Run on a small stack to prove the cascade is not recursive.
        let h = head.clone();
        thread::Builder::new()
            .stack_size(64 << 10)
            .spawn(move || {
                first.signal(Ctx::Sleep(&sleep()));
                first.put(Ctx::Sleep(&sleep()));
                assert!(h.is_signaled());
                h.put(Ctx::Sleep(&sleep()));
            })
            .unwrap()
            .join()
            .unwrap();
        assert_eq!(chain_query(&head, false), N);
        head.put(Ctx::Sleep(&ctx));
        drop(later);
        // An unsignalled timeline of N points whose points signal from the
        // last to the first: nothing completes until the first, which then
        // completes all N on a small stack.
        let mut pts = Vec::new();
        let first = fence(&d, 0);
        let mut head = Fence::chain(&d, None, first.clone(), 1, &ctx).unwrap();
        for i in 2..=N {
            let p = fence(&d, i);
            pts.push(p.clone());
            head = Fence::chain(&d, Some(head), p, i, &ctx).unwrap();
        }
        for p in pts.iter().rev() {
            p.signal(Ctx::Sleep(&ctx));
        }
        assert_eq!(chain_query(&head, false), 0);
        drop(pts);
        d.drain(&ctx);
        thread::Builder::new()
            .stack_size(64 << 10)
            .spawn(move || {
                first.signal(Ctx::Sleep(&sleep()));
                first.put(Ctx::Sleep(&sleep()));
                assert!(head.is_signaled());
                head.put(Ctx::Sleep(&sleep()));
            })
            .unwrap()
            .join()
            .unwrap();
        finish(d);
    }

    /// Eight threads race callback registration and removal against the
    /// signal, which runs in (simulated) interrupt context. Every callback
    /// runs at most once; a callback whose registration succeeded runs
    /// exactly once unless its removal succeeded; after a removal returns,
    /// the callback is not running any more.
    #[test]
    fn concurrent_add_remove_signal() {
        struct Flag {
            ran: Arc<AtomicUsize>,
            running: Arc<AtomicBool>,
        }
        impl FenceCallback for Flag {
            fn signaled(&self, _f: &Fence, _ctx: Ctx<'_>) -> Option<Fence> {
                self.running.store(true, Ordering::SeqCst);
                thread::yield_now();
                self.ran.fetch_add(1, Ordering::SeqCst);
                self.running.store(false, Ordering::SeqCst);
                None
            }
        }
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        for round in 0..200 {
            let f = fence(&d, round);
            let hs: Vec<_> = (0..8)
                .map(|t| {
                    let f = f.clone();
                    thread::spawn(move || {
                        let ctx = sleep();
                        let (ran, running) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicBool::new(false)));
                        let cb = FenceCb::new(Flag { ran: ran.clone(), running: running.clone() }, &ctx).unwrap();
                        let added = f.add_callback(&cb).is_ok();
                        let removed = if (t + round as usize) % 2 == 0 { f.remove_callback(&cb, &ctx) } else { false };
                        if removed {
                            assert_eq!(ran.load(Ordering::SeqCst), 0);
                        }
                        if removed || !added {
                            // Neither ran nor will run.
                        } else if t % 2 == 0 || round % 3 == 0 {
                            // remove_callback returned false: finished.
                            if (t + round as usize) % 2 == 0 {
                                assert!(!running.load(Ordering::SeqCst), "still running after remove");
                                assert_eq!(ran.load(Ordering::SeqCst), 1);
                            }
                        }
                        f.wait(false, None, &ctx).unwrap();
                        let expected = usize::from(added && !removed);
                        // The signal has returned for this thread's view
                        // once the node reports it has run.
                        while added && !removed && !cb.has_run() {
                            thread::yield_now();
                        }
                        assert_eq!(ran.load(Ordering::SeqCst), expected);
                        drop(cb);
                        f.put(Ctx::Sleep(&ctx));
                    })
                })
                .collect();
            thread::yield_now();
            f.signal(Ctx::Atomic(&atomic()));
            for h in hs {
                h.join().unwrap();
            }
            f.put(Ctx::Sleep(&ctx));
        }
        finish(d);
    }

    #[test]
    fn implicit_and_atomic_drops_defer() {
        let ctx = sleep();
        let d = FenceDomain::new_test(&ctx);
        let f = fence(&d, 1);
        let g = f.clone();
        f.put(Ctx::Atomic(&atomic()));
        drop(g);
        assert_eq!(d.stats.fences.load(Ordering::SeqCst), 1, "parked, not destroyed");
        assert_eq!(d.stats.deferred.load(Ordering::SeqCst), 1);
        finish(d);
    }
}
