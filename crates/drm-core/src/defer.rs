// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Releasing the last reference from any context.
//!
//! GEM objects can lose their last reference in interrupt context: from R2
//! on, a fence callback running in the virtqueue interrupt drops the
//! references its job held. Destroying an object may block
//! (`ddi_umem_free(9F)` returns pages to the system, unbinding DMA takes
//! adaptive locks, the mmap offset table is behind a mutex), so it must not
//! happen there. The rule is:
//!
//! * Every API that drops a reference takes a [`Ctx`]: the [`Sleepable`]
//!   token of thread context or the [`Atomic`] token of interrupt context.
//! * With `Ctx::Sleep`, the last reference destroys the object at once.
//! * With `Ctx::Atomic`, the last reference parks the object on the
//!   device's [`DeferList`] and asks for a task queue thread to drain the
//!   list. Parking needs no memory and no lock: the list is linked through
//!   a [`DeferLink`] inside the object and pushed with a compare-and-swap.
//!   Asking for the task is a `ddi_taskq_dispatch(9F)` with `DDI_NOSLEEP`,
//!   which low-level interrupt handlers, soft interrupts and callouts may
//!   do but high-level interrupt handlers may not; a high-level handler
//!   passes its references to a soft interrupt instead.
//! * The drain runs in thread context and destroys the parked objects. If
//!   the task cannot be dispatched (no memory in interrupt context), the
//!   objects stay parked and the next thread-context entry point of the
//!   device, or its detach, drains them. Nothing is lost; release is only
//!   delayed.
//!
//! [`put`] implements the decision and is unit-tested on the host from
//! several threads.

use core::marker::PhantomData;
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicPtr, Ordering};

use ddi::{Atomic, Sleepable};

use crate::karc::{KArc, KArcLast};

/// The execution context of a caller that drops a reference.
#[derive(Clone, Copy)]
pub enum Ctx<'a> {
    /// Thread context: the release may block.
    Sleep(&'a Sleepable),
    /// Interrupt context: the release must not block.
    Atomic(&'a Atomic),
}

/// The link through which an object is parked on a [`DeferList`]. Every
/// type that can be deferred embeds one.
pub struct DeferLink(AtomicPtr<()>);

impl DeferLink {
    /// A link that is not on any list.
    pub const fn new() -> DeferLink {
        DeferLink(AtomicPtr::new(ptr::null_mut()))
    }
}

impl Default for DeferLink {
    fn default() -> Self {
        Self::new()
    }
}

/// A type whose destruction can be deferred.
pub trait Deferrable {
    /// Returns the object's link.
    fn defer_link(&self) -> &DeferLink;
}

/// A lock-free list of objects whose last reference was dropped in
/// interrupt context.
pub struct DeferList<T: Deferrable> {
    head: AtomicPtr<()>,
    _items: PhantomData<KArcLast<T>>,
}

// SAFETY: the list owns KArcLast<T> values, which are Send when T is;
// access is through atomics only.
unsafe impl<T: Deferrable + Send> Send for DeferList<T> {}
// SAFETY: as above.
unsafe impl<T: Deferrable + Send> Sync for DeferList<T> {}

impl<T: Deferrable> DeferList<T> {
    /// An empty list.
    pub const fn new() -> DeferList<T> {
        DeferList { head: AtomicPtr::new(ptr::null_mut()), _items: PhantomData }
    }

    /// Parks `last`. Never blocks, never allocates, callable from any
    /// context.
    pub fn push(&self, last: KArcLast<T>) {
        let p = KArcLast::into_raw(last);
        // SAFETY: we own the parked value; its link is ours to write until
        // the push is visible.
        let link = unsafe { &p.as_ref().defer_link().0 };
        let mut head = self.head.load(Ordering::Relaxed);
        loop {
            link.store(head, Ordering::Relaxed);
            match self.head.compare_exchange_weak(head, p.as_ptr().cast(), Ordering::Release, Ordering::Relaxed) {
                Ok(_) => return,
                Err(h) => head = h,
            }
        }
    }

    /// Reports whether anything is parked.
    pub fn is_empty(&self) -> bool {
        self.head.load(Ordering::Acquire).is_null()
    }

    /// Takes every parked object. The caller drops them, which must happen
    /// in thread context.
    pub fn take_all(&self, _ctx: &Sleepable) -> Drain<T> {
        Drain { next: self.head.swap(ptr::null_mut(), Ordering::Acquire).cast(), _items: PhantomData }
    }
}

impl<T: Deferrable> Default for DeferList<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T: Deferrable> Drop for DeferList<T> {
    fn drop(&mut self) {
        // The owner (the device) drops the list in detach, thread context.
        let mut next: *mut T = self.head.swap(ptr::null_mut(), Ordering::Acquire).cast();
        while let Some(p) = NonNull::new(next) {
            // SAFETY: every entry was pushed from KArcLast::into_raw.
            let last = unsafe { KArcLast::from_raw(p) };
            next = last.defer_link().0.load(Ordering::Relaxed).cast();
            drop(last);
        }
    }
}

/// The objects taken from a [`DeferList`], in reverse order of parking.
pub struct Drain<T: Deferrable> {
    next: *mut T,
    _items: PhantomData<KArcLast<T>>,
}

impl<T: Deferrable> Iterator for Drain<T> {
    type Item = KArcLast<T>;
    fn next(&mut self) -> Option<KArcLast<T>> {
        let p = NonNull::new(self.next)?;
        // SAFETY: every entry was pushed from KArcLast::into_raw and is
        // taken back exactly once, here.
        let last = unsafe { KArcLast::from_raw(p) };
        self.next = last.defer_link().0.load(Ordering::Relaxed).cast();
        Some(last)
    }
}

impl<T: Deferrable> Drop for Drain<T> {
    fn drop(&mut self) {
        for last in self.by_ref() {
            drop(last);
        }
    }
}

/// What [`put`] did with a reference.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Put {
    /// Other references remain.
    Alive,
    /// It was the last one and the object was destroyed.
    Released,
    /// It was the last one, in interrupt context: the object was parked on
    /// the list, and the caller must arrange for the list to be drained.
    Deferred,
}

/// Drops `r` in the context `ctx`, following the rule of the module
/// documentation.
pub fn put<T: Deferrable>(r: KArc<T>, ctx: Ctx<'_>, list: &DeferList<T>) -> Put {
    let Some(last) = KArc::release(r) else { return Put::Alive };
    match ctx {
        Ctx::Sleep(_) => {
            drop(last);
            Put::Released
        }
        Ctx::Atomic(_) => {
            list.push(last);
            Put::Deferred
        }
    }
}

/// A deferred release of memory the core does not own as a [`KArc`]: a
/// structure of a driver's (a C `struct dma_fence`) whose release must run
/// in thread context. The node lives inside that structure (in its private
/// tail), so parking it needs no memory, and `run` is called once, by the
/// drain, with the node's address; it may free the memory around the node.
#[repr(C)]
pub struct RawDefer {
    next: AtomicPtr<RawDefer>,
    run: Option<unsafe fn(NonNull<RawDefer>, &Sleepable)>,
}

impl RawDefer {
    /// The node's size, which the structures that embed one reserve.
    pub const SIZE: usize = core::mem::size_of::<RawDefer>();
}

/// A lock-free list of [`RawDefer`] nodes, pushed from any context and
/// drained in thread context.
pub struct RawDeferList {
    head: AtomicPtr<RawDefer>,
}

impl RawDeferList {
    /// An empty list.
    pub const fn new() -> RawDeferList {
        RawDeferList { head: AtomicPtr::new(ptr::null_mut()) }
    }

    /// Parks `node`, to be passed to `run` by the next [`RawDeferList::drain`].
    /// Any context: one compare-and-swap loop, no memory.
    ///
    /// # Safety
    /// `node` points to writable memory of [`RawDefer::SIZE`] bytes, aligned
    /// for a pointer, that is on no list and stays valid until `run` has
    /// been called with it; `run` may be called on any thread.
    pub unsafe fn push(&self, node: NonNull<RawDefer>, run: unsafe fn(NonNull<RawDefer>, &Sleepable)) {
        // SAFETY: the caller's contract; the node is ours until the drain.
        unsafe { ptr::write(node.as_ptr(), RawDefer { next: AtomicPtr::new(ptr::null_mut()), run: Some(run) }) };
        let mut head = self.head.load(Ordering::Relaxed);
        loop {
            // SAFETY: as above.
            unsafe { node.as_ref() }.next.store(head, Ordering::Relaxed);
            match self.head.compare_exchange_weak(head, node.as_ptr(), Ordering::Release, Ordering::Relaxed) {
                Ok(_) => return,
                Err(h) => head = h,
            }
        }
    }

    /// Whether nothing is parked.
    pub fn is_empty(&self) -> bool {
        self.head.load(Ordering::Acquire).is_null()
    }

    /// Runs every parked node, oldest first, and returns how many ran. A
    /// `run` may park new nodes; they wait for the next drain. Thread
    /// context.
    pub fn drain(&self, ctx: &Sleepable) -> usize {
        let mut rev: *mut RawDefer = ptr::null_mut();
        let mut cur = self.head.swap(ptr::null_mut(), Ordering::Acquire);
        // Reverse into pushing order.
        while let Some(n) = NonNull::new(cur) {
            // SAFETY: parked nodes stay valid until run.
            cur = unsafe { n.as_ref() }.next.load(Ordering::Relaxed);
            // SAFETY: as above.
            unsafe { n.as_ref() }.next.store(rev, Ordering::Relaxed);
            rev = n.as_ptr();
        }
        let mut ran = 0;
        while let Some(n) = NonNull::new(rev) {
            // SAFETY: as above; read before `run`, which may free the node.
            let (next, run) = unsafe { (n.as_ref().next.load(Ordering::Relaxed), n.as_ref().run) };
            rev = next;
            if let Some(run) = run {
                // SAFETY: the pusher's contract.
                unsafe { run(n, ctx) };
            }
            ran += 1;
        }
        ran
    }
}

impl Default for RawDeferList {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::cell::Cell;
    use std::sync::atomic::{AtomicBool, AtomicU32};
    use std::thread;
    use std::vec::Vec;

    std::thread_local! {
        /// Set while a test thread pretends to be in interrupt context.
        static IN_INTERRUPT: Cell<bool> = const { Cell::new(false) };
    }

    struct Obj<'a> {
        link: DeferLink,
        dropped: &'a AtomicU32,
        bad: &'a AtomicBool,
    }
    impl Deferrable for Obj<'_> {
        fn defer_link(&self) -> &DeferLink {
            &self.link
        }
    }
    impl Drop for Obj<'_> {
        fn drop(&mut self) {
            // The release must never run in (simulated) interrupt context.
            if IN_INTERRUPT.with(Cell::get) {
                self.bad.store(true, Ordering::SeqCst);
            }
            self.dropped.fetch_add(1, Ordering::SeqCst);
        }
    }

    fn sleep() -> Sleepable {
        // SAFETY: host test threads may block.
        unsafe { Sleepable::assume() }
    }
    fn atomic() -> Atomic {
        // SAFETY: a host test simulating interrupt context.
        unsafe { Atomic::assume() }
    }

    #[test]
    fn atomic_drop_defers_until_drain() {
        let (dropped, bad) = (AtomicU32::new(0), AtomicBool::new(false));
        let list = DeferList::new();
        let a = KArc::new(Obj { link: DeferLink::new(), dropped: &dropped, bad: &bad }, &sleep()).unwrap();
        let b = a.clone();
        let at = atomic();
        IN_INTERRUPT.with(|c| c.set(true));
        assert_eq!(put(a, Ctx::Atomic(&at), &list), Put::Alive);
        assert_eq!(put(b, Ctx::Atomic(&at), &list), Put::Deferred);
        IN_INTERRUPT.with(|c| c.set(false));
        assert_eq!(dropped.load(Ordering::SeqCst), 0);
        assert!(!list.is_empty());
        let n = list.take_all(&sleep()).count();
        assert_eq!(n, 1);
        assert_eq!(dropped.load(Ordering::SeqCst), 1);
        assert!(!bad.load(Ordering::SeqCst));
        let c = KArc::new(Obj { link: DeferLink::new(), dropped: &dropped, bad: &bad }, &sleep()).unwrap();
        assert_eq!(put(c, Ctx::Sleep(&sleep()), &list), Put::Released);
        assert_eq!(dropped.load(Ordering::SeqCst), 2);
        assert!(list.is_empty());
    }

    /// Eight threads drop references to shared objects, half of them in
    /// simulated interrupt context, while a drainer thread empties the list.
    /// Every object is destroyed exactly once, never in interrupt context.
    #[test]
    fn concurrent_mixed_context_drops() {
        const OBJECTS: usize = 2000;
        const THREADS: usize = 8;
        let (dropped, bad, stop) = (AtomicU32::new(0), AtomicBool::new(false), AtomicBool::new(false));
        let deferred = AtomicU32::new(0);
        let list = DeferList::new();
        thread::scope(|s| {
            let objs: Vec<_> = (0..OBJECTS)
                .map(|_| KArc::new(Obj { link: DeferLink::new(), dropped: &dropped, bad: &bad }, &sleep()).unwrap())
                .collect();
            let drainer = s.spawn(|| {
                while !stop.load(Ordering::SeqCst) {
                    drop(list.take_all(&sleep()));
                    thread::yield_now();
                }
            });
            let workers: Vec<_> = (0..THREADS)
                .map(|t| {
                    let mine: Vec<_> = objs.iter().map(KArc::clone).collect();
                    let (list, deferred) = (&list, &deferred);
                    s.spawn(move || {
                        for (i, r) in mine.into_iter().enumerate() {
                            if (i + t) % 2 == 0 {
                                let at = atomic();
                                IN_INTERRUPT.with(|c| c.set(true));
                                if put(r, Ctx::Atomic(&at), list) == Put::Deferred {
                                    deferred.fetch_add(1, Ordering::SeqCst);
                                }
                                IN_INTERRUPT.with(|c| c.set(false));
                            } else {
                                put(r, Ctx::Sleep(&sleep()), list);
                            }
                        }
                    })
                })
                .collect();
            for r in objs {
                put(r, Ctx::Sleep(&sleep()), &list);
            }
            for w in workers {
                w.join().unwrap();
            }
            stop.store(true, Ordering::SeqCst);
            drainer.join().unwrap();
        });
        drop(list.take_all(&sleep()));
        assert_eq!(dropped.load(Ordering::SeqCst) as usize, OBJECTS);
        assert!(!bad.load(Ordering::SeqCst), "an object was destroyed in interrupt context");
        assert!(deferred.load(Ordering::SeqCst) > 0, "the test never exercised deferral");
    }

    #[repr(C)]
    struct RawItem {
        node: [u64; 2],
        id: usize,
        ran: &'static AtomicU32,
        order: &'static std::sync::Mutex<Vec<usize>>,
    }

    unsafe fn raw_run(node: NonNull<RawDefer>, _ctx: &Sleepable) {
        // SAFETY: the node is the first field of a boxed RawItem.
        let item = unsafe { std::boxed::Box::from_raw(node.as_ptr() as *mut RawItem) };
        item.order.lock().unwrap().push(item.id);
        item.ran.fetch_add(1, Ordering::SeqCst);
    }

    /// Nodes parked from several threads all run once, each thread's in
    /// the order it parked them; a run may free its node.
    #[test]
    fn raw_defer_runs_every_node_once() {
        let ran: &'static AtomicU32 = std::boxed::Box::leak(std::boxed::Box::new(AtomicU32::new(0)));
        let order: &'static std::sync::Mutex<Vec<usize>> =
            std::boxed::Box::leak(std::boxed::Box::new(std::sync::Mutex::new(Vec::new())));
        let list: &'static RawDeferList = std::boxed::Box::leak(std::boxed::Box::new(RawDeferList::new()));
        let threads: Vec<_> = (0..4)
            .map(|t| {
                thread::spawn(move || {
                    for i in 0..250 {
                        let item = std::boxed::Box::new(RawItem { node: [0; 2], id: t * 1000 + i, ran, order });
                        let p = NonNull::new(std::boxed::Box::into_raw(item) as *mut RawDefer).unwrap();
                        // SAFETY: a boxed item freed by raw_run.
                        unsafe { list.push(p, raw_run) };
                    }
                })
            })
            .collect();
        let mut drained = 0;
        while drained < 1000 {
            drained += list.drain(&sleep());
            thread::yield_now();
        }
        for t in threads {
            t.join().unwrap();
        }
        drained += list.drain(&sleep());
        assert_eq!(drained, 1000);
        assert_eq!(ran.load(Ordering::SeqCst), 1000);
        assert!(list.is_empty());
        let o = order.lock().unwrap();
        for t in 0..4 {
            let mine: Vec<usize> = o.iter().copied().filter(|id| id / 1000 == t).collect();
            assert!(mine.windows(2).all(|w| w[0] < w[1]), "thread {t}'s nodes in order");
        }
    }
}
