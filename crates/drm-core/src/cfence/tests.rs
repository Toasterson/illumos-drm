// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Host tests of the Linux-layout fences: a mock driver whose ops count
//! their calls, real threads for the races.

extern crate std;

use super::*;
use core::ffi::c_long;
use core::sync::atomic::AtomicI64;
use std::boxed::Box;
use std::sync::Mutex;
use std::thread;
use std::vec::Vec;

fn sleep() -> Sleepable {
    // SAFETY: host test threads may block.
    unsafe { Sleepable::assume() }
}

fn atomic() -> Atomic {
    // SAFETY: a host test standing in for interrupt context.
    unsafe { Atomic::assume() }
}

fn domain() -> KArc<FenceDomain> {
    FenceDomain::new_test(&sleep())
}

/// One tick is a millisecond on the host.
struct Ms;
impl TickClock for Ms {
    fn ticks_to_ns(&self, t: i64) -> i64 {
        t.saturating_mul(1_000_000)
    }
    fn ns_to_ticks(&self, ns: i64) -> i64 {
        ns / 1_000_000
    }
}

#[derive(Default)]
struct Counters {
    released: AtomicU32,
    enable: AtomicU32,
    enable_false: AtomicBool,
    signaled: AtomicU32,
    hw_done: AtomicBool,
    wait: AtomicU32,
    deadline: AtomicI64,
    /// The release ran while the test said "interrupt context".
    released_in_irq: AtomicBool,
}

std::thread_local! {
    static IN_IRQ: core::cell::Cell<bool> = const { core::cell::Cell::new(false) };
}

/// A driver's fence: the base first, an external lock, the counters.
#[repr(C)]
struct Mock {
    base: f::dma_fence,
    lock: f::spinlock_t,
    c: *const Counters,
}

fn counters<'a>(fence: *mut f::dma_fence) -> &'a Counters {
    // SAFETY: every mock fence is the first field of a live Mock.
    unsafe { &*(*(fence as *mut Mock)).c }
}

unsafe extern "C" fn m_drv(_f: *mut f::dma_fence) -> *const c_char {
    c"mock".as_ptr()
}
unsafe extern "C" fn m_tl(_f: *mut f::dma_fence) -> *const c_char {
    c"mock-timeline".as_ptr()
}
unsafe extern "C" fn m_enable(fence: *mut f::dma_fence) -> bool {
    let c = counters(fence);
    c.enable.fetch_add(1, Ordering::SeqCst);
    !c.enable_false.load(Ordering::SeqCst)
}
unsafe extern "C" fn m_signaled(fence: *mut f::dma_fence) -> bool {
    let c = counters(fence);
    c.signaled.fetch_add(1, Ordering::SeqCst);
    c.hw_done.load(Ordering::SeqCst)
}
unsafe extern "C" fn m_wait(fence: *mut f::dma_fence, _intr: bool, _t: c_long) -> c_long {
    counters(fence).wait.fetch_add(1, Ordering::SeqCst);
    77
}
unsafe extern "C" fn m_deadline(fence: *mut f::dma_fence, d: f::ktime_t) {
    counters(fence).deadline.store(d, Ordering::SeqCst);
}
unsafe extern "C" fn m_release(fence: *mut f::dma_fence) {
    let c = counters(fence);
    if IN_IRQ.with(|i| i.get()) {
        c.released_in_irq.store(true, Ordering::SeqCst);
    }
    c.released.fetch_add(1, Ordering::SeqCst);
    // SAFETY: made by mock() with Box.
    drop(unsafe { Box::from_raw(fence as *mut Mock) });
}

static FULL: f::dma_fence_ops = f::dma_fence_ops {
    get_driver_name: Some(m_drv),
    get_timeline_name: Some(m_tl),
    enable_signaling: Some(m_enable),
    signaled: Some(m_signaled),
    wait: None,
    release: Some(m_release),
    set_deadline: Some(m_deadline),
};
static PLAIN: f::dma_fence_ops = f::dma_fence_ops {
    get_driver_name: Some(m_drv),
    get_timeline_name: Some(m_tl),
    enable_signaling: None,
    signaled: None,
    wait: None,
    release: Some(m_release),
    set_deadline: None,
};
static WAITING: f::dma_fence_ops = f::dma_fence_ops {
    get_driver_name: Some(m_drv),
    get_timeline_name: Some(m_tl),
    enable_signaling: None,
    signaled: None,
    wait: Some(m_wait),
    release: Some(m_release),
    set_deadline: None,
};

/// A new mock fence with one reference; `extern_lock` uses the Mock's own
/// lock, otherwise the inline one.
fn mock(
    ops: &'static f::dma_fence_ops,
    extern_lock: bool,
    d: &KArc<FenceDomain>,
) -> (*mut f::dma_fence, &'static Counters) {
    let c: &'static Counters = Box::leak(Box::default());
    // SAFETY: plain data; zero is valid.
    let m: Box<Mock> = Box::new(unsafe { core::mem::zeroed() });
    let m = Box::into_raw(m);
    // SAFETY: just allocated.
    unsafe {
        (*m).c = c;
        let l = if extern_lock {
            lk::init(ptr::addr_of_mut!((*m).lock));
            ptr::addr_of_mut!((*m).lock)
        } else {
            ptr::null_mut()
        };
        init(m as *mut f::dma_fence, ops, l, 9, 1, 0, d);
    }
    (m as *mut f::dma_fence, c)
}

/// A test callback: counts its runs, records whether the lock was held
/// and its position in a shared order.
#[repr(C)]
struct TCb {
    cb: f::dma_fence_cb,
    id: u32,
    ran: AtomicU32,
    lock_held: AtomicBool,
    error_seen: AtomicI32,
    order: *const Mutex<Vec<u32>>,
    /// Drops a reference to this fence from inside the function.
    put_fence: AtomicBool,
    /// Drops a reference to another fence from inside the function.
    put_other: core::sync::atomic::AtomicPtr<f::dma_fence>,
}

// SAFETY: the raw pointers are the test's, used under its rules.
unsafe impl Send for TCb {}

impl TCb {
    fn new(id: u32, order: *const Mutex<Vec<u32>>) -> Box<TCb> {
        Box::new(TCb {
            // SAFETY: plain data; zero is valid.
            cb: unsafe { core::mem::zeroed() },
            id,
            ran: AtomicU32::new(0),
            lock_held: AtomicBool::new(false),
            error_seen: AtomicI32::new(0),
            order,
            put_fence: AtomicBool::new(false),
            put_other: core::sync::atomic::AtomicPtr::new(ptr::null_mut()),
        })
    }
    fn raw(&mut self) -> *mut f::dma_fence_cb {
        ptr::addr_of_mut!(self.cb)
    }
    fn empty(&self) -> bool {
        // SAFETY: an initialised node.
        unsafe { list_empty(ptr::addr_of!(self.cb.node)) }
    }
}

static TEST_DOMAIN: std::sync::OnceLock<usize> = std::sync::OnceLock::new();

unsafe extern "C" fn tcb_func(fence: *mut f::dma_fence, cb: *mut f::dma_fence_cb) {
    // SAFETY: cb is the first field of a TCb the test keeps.
    let t = unsafe { &*(cb as *const TCb) };
    // SAFETY: the fence is live while its callbacks run.
    unsafe {
        t.lock_held.store(lk::owned(lock_of(fence)), Ordering::SeqCst);
    }
    t.error_seen.store(error_of(fence).load(Ordering::SeqCst), Ordering::SeqCst);
    if !t.order.is_null() {
        // SAFETY: the test's vector outlives the fence.
        unsafe { &*t.order }.lock().unwrap().push(t.id);
    }
    let other = t.put_other.swap(ptr::null_mut(), Ordering::SeqCst);
    if !other.is_null() {
        let d = *TEST_DOMAIN.get().unwrap() as *const FenceDomain;
        // SAFETY: the test handed this reference to the callback.
        unsafe { put(other, Ctx::Sleep(&sleep()), &*d) };
    }
    if t.put_fence.load(Ordering::SeqCst) {
        let d = *TEST_DOMAIN.get().unwrap() as *const FenceDomain;
        // SAFETY: the test handed this reference to the callback; the
        // domain is leaked for the test's life.
        unsafe { put(fence, Ctx::Sleep(&sleep()), &*d) };
    }
    t.ran.fetch_add(1, Ordering::SeqCst);
}

fn fl(fence: *mut f::dma_fence) -> u64 {
    flags(fence).load(Ordering::SeqCst)
}

#[test]
fn signal_runs_callbacks_in_order_under_the_lock() {
    let d = domain();
    for extern_lock in [true, false] {
        let (fence, c) = mock(&PLAIN, extern_lock, &d);
        assert_eq!(fl(fence) & INLINE_LOCK != 0, !extern_lock);
        let order = Mutex::new(Vec::new());
        let mut cbs: Vec<Box<TCb>> = (1..=3).map(|i| TCb::new(i, &order)).collect();
        // SAFETY: a live fence and callbacks the test keeps.
        unsafe {
            for t in cbs.iter_mut() {
                assert_eq!(add_callback(fence, t.raw(), Some(tcb_func)), 0);
                assert!(!t.empty());
            }
            assert!(!is_signaled(fence));
            set_error(fence, -5);
            assert!(signal(fence, Some(1234)));
            assert!(!signal(fence, None), "signalling twice does nothing");
            assert_eq!(fl(fence) & (SIGNALED | TIMESTAMP), SIGNALED | TIMESTAMP);
            assert_eq!((*fence).__bindgen_anon_2.timestamp, 1234);
            assert_eq!(get_status(fence), -5);
        }
        assert_eq!(*order.lock().unwrap(), [1, 2, 3]);
        for t in &cbs {
            assert_eq!(t.ran.load(Ordering::SeqCst), 1);
            assert!(t.lock_held.load(Ordering::SeqCst), "callbacks run with the fence's lock held");
            assert_eq!(t.error_seen.load(Ordering::SeqCst), -5);
            assert!(t.empty(), "a run callback's node is empty");
        }
        let mut late = TCb::new(4, ptr::null());
        // SAFETY: as above.
        unsafe {
            assert_eq!(add_callback(fence, late.raw(), Some(tcb_func)), -Errno::ENOENT.get());
            assert!(late.empty());
            assert!(!remove_callback(fence, late.raw()));
            put(fence, Ctx::Sleep(&sleep()), &d);
        }
        assert_eq!(c.released.load(Ordering::SeqCst), 1);
    }
}

#[test]
fn enable_signaling_runs_once_and_false_means_signalled() {
    let d = domain();
    let (fence, c) = mock(&FULL, true, &d);
    let mut a = TCb::new(1, ptr::null());
    let mut b = TCb::new(2, ptr::null());
    // SAFETY: a live fence and callbacks the test keeps.
    unsafe {
        assert_eq!(c.enable.load(Ordering::SeqCst), 0, "not enabled at init");
        assert_eq!(add_callback(fence, a.raw(), Some(tcb_func)), 0);
        assert_eq!(add_callback(fence, b.raw(), Some(tcb_func)), 0);
        enable_sw_signaling(fence);
        assert_eq!(c.enable.load(Ordering::SeqCst), 1);
        assert_ne!(fl(fence) & ENABLE_SIGNAL, 0);
        assert!(remove_callback(fence, b.raw()));
        assert!(b.empty());
        assert!(!remove_callback(fence, b.raw()));
        signal(fence, None);
        put(fence, Ctx::Sleep(&sleep()), &d);
    }
    assert_eq!(a.ran.load(Ordering::SeqCst), 1);
    assert_eq!(b.ran.load(Ordering::SeqCst), 0, "a removed callback never runs");

    // enable_signaling returning false: the fence is signalled at once.
    let (fence, c) = mock(&FULL, false, &d);
    c.enable_false.store(true, Ordering::SeqCst);
    let mut a = TCb::new(1, ptr::null());
    // SAFETY: as above.
    unsafe {
        assert_eq!(add_callback(fence, a.raw(), Some(tcb_func)), -Errno::ENOENT.get());
        assert!(a.empty());
        assert_ne!(fl(fence) & SIGNALED, 0);
        assert_eq!(get_status(fence), 1);
        put(fence, Ctx::Sleep(&sleep()), &d);
    }
    assert_eq!(a.ran.load(Ordering::SeqCst), 0);
    assert_eq!(c.released.load(Ordering::SeqCst), 1);
}

#[test]
fn signaled_op_is_polled_and_signals() {
    let d = domain();
    let (fence, c) = mock(&FULL, true, &d);
    let mut a = TCb::new(1, ptr::null());
    // SAFETY: a live fence and a callback the test keeps.
    unsafe {
        assert_eq!(add_callback(fence, a.raw(), Some(tcb_func)), 0);
        assert!(!is_signaled(fence));
        assert_eq!(get_status(fence), 0);
        assert!(c.signaled.load(Ordering::SeqCst) >= 2);
        c.hw_done.store(true, Ordering::SeqCst);
        assert!(is_signaled(fence), "ops->signaled true signals the fence");
        assert_ne!(fl(fence) & SIGNALED, 0);
        assert_eq!(a.ran.load(Ordering::SeqCst), 1);
        let polls = c.signaled.load(Ordering::SeqCst);
        assert!(is_signaled(fence));
        assert_eq!(c.signaled.load(Ordering::SeqCst), polls, "the bit answers once set");
        put(fence, Ctx::Sleep(&sleep()), &d);
    }
}

#[test]
fn deadline_and_names() {
    let d = domain();
    let (fence, c) = mock(&FULL, true, &d);
    // SAFETY: a live fence.
    unsafe {
        set_deadline(fence, 42);
        assert_eq!(c.deadline.load(Ordering::SeqCst), 42);
        assert_eq!(CStr::from_ptr(name(fence, false)), c"mock");
        assert_eq!(CStr::from_ptr(name(fence, true)), c"mock-timeline");
        signal(fence, None);
        set_deadline(fence, 7);
        assert_eq!(c.deadline.load(Ordering::SeqCst), 42, "no deadline after signalling");
        put(fence, Ctx::Sleep(&sleep()), &d);
    }
    // No ops: named "unknown", with a warning; 64-bit sequence numbers.
    let before = WARNINGS.load(Ordering::SeqCst);
    // SAFETY: plain data; zero is valid.
    let mut raw: Box<f::dma_fence> = Box::new(unsafe { core::mem::zeroed() });
    let p = ptr::addr_of_mut!(*raw);
    // SAFETY: the test's structure; no release callback frees it.
    unsafe {
        init(p, ptr::null(), ptr::null_mut(), 1, 2, SEQNO64, &d);
        assert!(WARNINGS.load(Ordering::SeqCst) > before);
        assert_ne!(fl(p) & SEQNO64, 0);
        assert_eq!(CStr::from_ptr(name(p, false)), c"unknown");
        signal(p, None);
        put(p, Ctx::Sleep(&sleep()), &d);
        assert_eq!(fl(p) & INITIALIZED, INITIALIZED);
    }
}

#[test]
fn wait_uses_the_wait_op_or_the_core() {
    let d = domain();
    let (fence, c) = mock(&WAITING, true, &d);
    // SAFETY: a live fence.
    unsafe {
        assert_eq!(wait_timeout(fence, false, 10, &Ms, &sleep()), 77);
        assert_eq!(c.wait.load(Ordering::SeqCst), 1);
        assert_eq!(wait_timeout(fence, false, -1, &Ms, &sleep()), -i64::from(Errno::EINVAL.get()));
        signal(fence, None);
        put(fence, Ctx::Sleep(&sleep()), &d);
    }

    // The default wait: a timeout, a poll, then a signal from another
    // thread.
    let (fence, c) = mock(&FULL, true, &d);
    // SAFETY: a live fence.
    unsafe {
        assert_eq!(wait_timeout(fence, false, 0, &Ms, &sleep()), 0);
        assert_eq!(wait_timeout(fence, false, 5, &Ms, &sleep()), 0, "times out");
        assert_eq!(c.enable.load(Ordering::SeqCst), 1);
        get(fence);
    }
    let p = P(fence);
    let dd = d.clone();
    let t = thread::spawn(move || {
        let p = p;
        thread::sleep(std::time::Duration::from_millis(20));
        // SAFETY: the reference taken for this thread.
        unsafe {
            set_error(p.0, -125);
            signal(p.0, None);
            put(p.0, Ctx::Atomic(&atomic()), &dd);
        }
    });
    // SAFETY: a live fence.
    let r = unsafe { wait_timeout(fence, false, i64::MAX, &Ms, &sleep()) };
    assert!(r >= 1, "woken by the signal: {r}");
    t.join().unwrap();
    // SAFETY: as above.
    unsafe {
        assert_eq!(get_status(fence), -125);
        put(fence, Ctx::Sleep(&sleep()), &d);
    }
    d.drain(&sleep());
}

#[test]
fn last_put_defers_in_interrupt_context_and_under_the_lock() {
    let d = domain();
    let (fence, c) = mock(&PLAIN, true, &d);
    // SAFETY: a live fence.
    unsafe {
        signal(fence, None);
        IN_IRQ.with(|i| i.set(true));
        put(fence, Ctx::Atomic(&atomic()), &d);
        IN_IRQ.with(|i| i.set(false));
    }
    assert_eq!(c.released.load(Ordering::SeqCst), 0, "parked, not released, in interrupt context");
    d.drain(&sleep());
    assert_eq!(c.released.load(Ordering::SeqCst), 1);
    assert!(!c.released_in_irq.load(Ordering::SeqCst));

    // A callback that drops the last reference runs with the lock held:
    // the release is parked, and the callbacks after it still run.
    let leaked: &'static KArc<FenceDomain> = Box::leak(Box::new(domain()));
    let dp = KArc::as_ptr(leaked).as_ptr() as usize;
    let _ = TEST_DOMAIN.set(dp);
    let dd = *TEST_DOMAIN.get().unwrap() as *const FenceDomain;
    let (fence, c) = mock(&PLAIN, false, leaked);
    let mut a = TCb::new(1, ptr::null());
    let mut b = TCb::new(2, ptr::null());
    a.put_fence.store(true, Ordering::SeqCst);
    // SAFETY: a live fence; the reference is handed to callback `a`.
    unsafe {
        assert_eq!(add_callback(fence, a.raw(), Some(tcb_func)), 0);
        assert_eq!(add_callback(fence, b.raw(), Some(tcb_func)), 0);
        get(fence);
        signal(fence, None);
        assert_eq!(c.released.load(Ordering::SeqCst), 0);
        put(fence, Ctx::Sleep(&sleep()), &*dd);
    }
    assert_eq!(a.ran.load(Ordering::SeqCst), 1);
    assert_eq!(b.ran.load(Ordering::SeqCst), 1, "a callback queued behind the last put still runs");
    // SAFETY: the leaked domain.
    unsafe { &*dd }.drain(&sleep());
    assert_eq!(c.released.load(Ordering::SeqCst), 1);
}

#[test]
fn extra_put_warns_and_does_not_release_twice() {
    let d = domain();
    // SAFETY: plain data; zero is valid.
    let mut raw: Box<f::dma_fence> = Box::new(unsafe { core::mem::zeroed() });
    let p = ptr::addr_of_mut!(*raw);
    let before = WARNINGS.load(Ordering::SeqCst);
    // SAFETY: the test's structure; no release callback frees it.
    unsafe {
        init(p, &PROXY_OPS_NO_RELEASE, ptr::null_mut(), 1, 1, 0, &d);
        signal(p, None);
        put(p, Ctx::Sleep(&sleep()), &d);
        put(p, Ctx::Sleep(&sleep()), &d);
    }
    assert!(WARNINGS.load(Ordering::SeqCst) > before);
}

static PROXY_OPS_NO_RELEASE: f::dma_fence_ops = f::dma_fence_ops {
    get_driver_name: Some(m_drv),
    get_timeline_name: Some(m_tl),
    enable_signaling: None,
    signaled: None,
    wait: None,
    release: None,
    set_deadline: None,
};

#[test]
fn released_unsignalled_with_callbacks_signals_edeadlk() {
    let d = domain();
    let (fence, c) = mock(&PLAIN, true, &d);
    let mut a = TCb::new(1, ptr::null());
    let before = WARNINGS.load(Ordering::SeqCst);
    // SAFETY: a live fence.
    unsafe {
        assert_eq!(add_callback(fence, a.raw(), Some(tcb_func)), 0);
        put(fence, Ctx::Sleep(&sleep()), &d);
    }
    assert_eq!(a.ran.load(Ordering::SeqCst), 1);
    assert_eq!(a.error_seen.load(Ordering::SeqCst), -EDEADLK);
    assert!(WARNINGS.load(Ordering::SeqCst) > before);
    assert_eq!(c.released.load(Ordering::SeqCst), 1);
}

#[test]
fn the_bridge_signals_the_shadow_and_keeps_the_fence() {
    let d = domain();
    let (fence, c) = mock(&FULL, true, &d);
    // SAFETY: a live fence.
    let shadow = unsafe { core_fence(fence) }.unwrap();
    assert_eq!(c.enable.load(Ordering::SeqCst), 1, "taking the fence enables signalling");
    // SAFETY: as above.
    let again = unsafe { core_fence(fence) }.unwrap();
    assert!(Fence::ptr_eq(&shadow, &again));
    assert_eq!(c.enable.load(Ordering::SeqCst), 1);
    again.put(Ctx::Sleep(&sleep()));
    // The driver drops its reference: the bridge keeps the structure.
    // SAFETY: the driver's reference.
    unsafe {
        get(fence);
        put(fence, Ctx::Sleep(&sleep()), &d);
    }
    assert_eq!(c.released.load(Ordering::SeqCst), 0);
    assert!(!shadow.is_signaled());
    // SAFETY: the reference the test kept.
    unsafe {
        set_error(fence, -125);
        signal(fence, None);
    }
    assert!(shadow.is_signaled());
    assert_eq!(shadow.error(), Errno::new(125));
    // SAFETY: as above.
    unsafe { put(fence, Ctx::Sleep(&sleep()), &d) };
    // The bridge's reference went under the lock: parked.
    d.drain(&sleep());
    assert_eq!(c.released.load(Ordering::SeqCst), 1);
    shadow.put(Ctx::Sleep(&sleep()));

    // A fence signalled before the core takes it: the shadow follows.
    let (fence, _c) = mock(&PLAIN, false, &d);
    // SAFETY: a live fence.
    unsafe {
        signal(fence, None);
        let s = core_fence(fence).unwrap();
        assert!(s.is_signaled());
        s.put(Ctx::Sleep(&sleep()));
        put(fence, Ctx::Sleep(&sleep()), &d);
    }
}

#[test]
fn a_driver_signalling_by_hand_reaches_the_core() {
    // i915's breadcrumbs: set the bit, take the list, set the time, run
    // the callbacks, all under the fence's lock.
    let d = domain();
    let (fence, c) = mock(&FULL, true, &d);
    // SAFETY: a live fence.
    let shadow = unsafe { core_fence(fence) }.unwrap();
    // SAFETY: as above; this is the driver's code.
    unsafe {
        let l = lock_of(fence);
        lk::enter(l);
        assert_eq!(flags(fence).fetch_or(SIGNALED, Ordering::SeqCst) & SIGNALED, 0);
        let mut local = f::list_head { next: ptr::null_mut(), prev: ptr::null_mut() };
        let lp = ptr::addr_of_mut!(local);
        list_replace(cb_list(fence), lp);
        (*fence).__bindgen_anon_2.timestamp = 99;
        flags(fence).fetch_or(TIMESTAMP, Ordering::SeqCst);
        let mut cur = (*lp).next;
        while !ptr::eq(cur, lp) {
            let next = (*cur).next;
            list_init(cur);
            let cb = cur as *mut f::dma_fence_cb;
            ((*cb).func.unwrap())(fence, cb);
            cur = next;
        }
        lk::exit(l);
    }
    assert!(shadow.is_signaled());
    shadow.put(Ctx::Sleep(&sleep()));
    // SAFETY: the driver's reference.
    unsafe { put(fence, Ctx::Sleep(&sleep()), &d) };
    d.drain(&sleep());
    assert_eq!(c.released.load(Ordering::SeqCst), 1);
}

#[test]
fn a_proxy_follows_its_core_fence() {
    let d = domain();
    let core = Fence::new(&d, d.alloc_contexts(1), 3, &sleep()).unwrap();
    let p = proxy(core.clone(), &d);
    assert!(!p.is_null());
    let q = proxy(core.clone(), &d);
    let mut a = TCb::new(1, ptr::null());
    let mut b = TCb::new(2, ptr::null());
    // SAFETY: live proxies.
    unsafe {
        assert_eq!((*p).context, core.context());
        assert_eq!((*p).seqno, 3);
        assert!(!is_signaled(p));
        assert_eq!(add_callback(p, a.raw(), Some(tcb_func)), 0);
        assert_eq!(add_callback(q, b.raw(), Some(tcb_func)), 0);
    }
    // Signalled elsewhere, with an error: both structures follow.
    core.signal_with_error(Errno::EIO, Ctx::Atomic(&atomic()));
    assert_eq!(a.ran.load(Ordering::SeqCst), 1);
    assert_eq!(b.ran.load(Ordering::SeqCst), 1);
    // SAFETY: live proxies.
    unsafe {
        assert_eq!(get_status(p), -Errno::EIO.get());
        assert_eq!(a.error_seen.load(Ordering::SeqCst), -Errno::EIO.get());
        put(p, Ctx::Sleep(&sleep()), &d);
        put(q, Ctx::Sleep(&sleep()), &d);
    }
    d.drain(&sleep());

    // Signalling a proxy signals its core fence; a proxy of a signalled
    // fence starts signalled.
    let core2 = Fence::new(&d, d.alloc_contexts(1), 1, &sleep()).unwrap();
    let r = proxy(core2.clone(), &d);
    // SAFETY: a live proxy.
    unsafe {
        set_error(r, -Errno::EINVAL.get());
        signal(r, None);
    }
    assert!(core2.is_signaled());
    assert_eq!(core2.error(), Some(Errno::EINVAL));
    let s = proxy(core2.clone(), &d);
    // SAFETY: live proxies.
    unsafe {
        assert_ne!(fl(s) & SIGNALED, 0);
        put(r, Ctx::Sleep(&sleep()), &d);
        put(s, Ctx::Sleep(&sleep()), &d);
    }
    core.put(Ctx::Sleep(&sleep()));
    core2.put(Ctx::Sleep(&sleep()));
    d.drain(&sleep());
    assert_eq!(d.stats.live()[0], 0, "no core fence left");
}

struct P(*mut f::dma_fence);
// SAFETY: the fences are thread-safe.
unsafe impl Send for P {}
// SAFETY: as above.
unsafe impl Sync for P {}

#[test]
fn proxy_release_races_the_relay() {
    // The core fence signals on one thread while the last reference to a
    // proxy whose relay is armed goes on another. Three kinds of round:
    //  0  the relay has started but not taken its reference when the
    //     release runs: the release waits for it, the relay finds the
    //     count at zero, and the callback runs once with -EDEADLK (from
    //     the release);
    //  1  the relay holds its reference when the owner drops the other:
    //     the relay's put is the last (parked), and the callback runs once
    //     from the relay without an error;
    //  2  both start together with small spins of either side: the
    //     callback runs once, with one of the two errors.
    // The hooks count the paths; each forced one must be seen.
    use super::hooks as h;
    let d = domain();
    let (zero0, found0, last0) = (
        h::RELAY_SAW_ZERO.load(Ordering::SeqCst),
        h::RELEASE_FOUND_RUNNING.load(Ordering::SeqCst),
        h::RELAY_LAST.load(Ordering::SeqCst),
    );
    let mut errors = [0u32; 2];
    for i in 0..600u32 {
        let mode = i % 3;
        let core = Fence::new(&d, d.alloc_contexts(1), u64::from(i), &sleep()).unwrap();
        let p = proxy(core.clone(), &d);
        let mut a = TCb::new(1, ptr::null());
        // SAFETY: a live proxy; arms its relay.
        unsafe { assert_eq!(add_callback(p, a.raw(), Some(tcb_func)), 0) };
        let entered = h::RELAY_ENTERED.load(Ordering::SeqCst);
        let got = h::RELAY_GOT.load(Ordering::SeqCst);
        let go = std::sync::Arc::new(AtomicBool::new(false));
        match mode {
            0 => {
                h::SPIN_BEFORE_GET.store(2_000_000, Ordering::SeqCst);
                h::SPIN_AFTER_GET.store(0, Ordering::SeqCst);
            }
            1 => {
                h::SPIN_BEFORE_GET.store(0, Ordering::SeqCst);
                h::SPIN_AFTER_GET.store(2_000_000, Ordering::SeqCst);
            }
            _ => {
                h::SPIN_BEFORE_GET.store(i % 7 * 50, Ordering::SeqCst);
                h::SPIN_AFTER_GET.store(i % 5 * 50, Ordering::SeqCst);
            }
        }
        let (c2, go2) = (core.clone(), go.clone());
        let t = thread::spawn(move || {
            while !go2.load(Ordering::SeqCst) {
                core::hint::spin_loop();
            }
            c2.signal(Ctx::Atomic(&atomic()));
            c2.put(Ctx::Atomic(&atomic()));
        });
        go.store(true, Ordering::SeqCst);
        match mode {
            0 => {
                // The relay runs and spins before its get.
                while h::RELAY_ENTERED.load(Ordering::SeqCst) == entered {
                    core::hint::spin_loop();
                }
            }
            1 => {
                // The relay holds its reference and spins.
                while h::RELAY_GOT.load(Ordering::SeqCst) == got {
                    core::hint::spin_loop();
                }
            }
            _ => {
                for _ in 0..(i % 11) * 30 {
                    core::hint::spin_loop();
                }
            }
        }
        // SAFETY: the test's reference, the last unless the relay holds one.
        unsafe { put(p, Ctx::Sleep(&sleep()), &d) };
        // The release of a proxy with an armed relay is parked: run it now,
        // on this thread, while the relay may still run on the other.
        d.drain(&sleep());
        t.join().unwrap();
        d.drain(&sleep());
        assert_eq!(a.ran.load(Ordering::SeqCst), 1, "round {i}: the callback runs exactly once");
        let e = a.error_seen.load(Ordering::SeqCst);
        match (mode, e) {
            (0, x) => assert_eq!(x, -EDEADLK, "round {i}: released before the relay signalled"),
            (1, x) => assert_eq!(x, 0, "round {i}: signalled by the relay"),
            (_, 0) => errors[0] += 1,
            (_, x) => {
                assert_eq!(x, -EDEADLK);
                errors[1] += 1;
            }
        }
        core.put(Ctx::Sleep(&sleep()));
        drop(a);
    }
    h::SPIN_BEFORE_GET.store(0, Ordering::SeqCst);
    h::SPIN_AFTER_GET.store(0, Ordering::SeqCst);
    d.drain(&sleep());
    assert!(h::RELAY_SAW_ZERO.load(Ordering::SeqCst) > zero0, "a relay found the count at zero");
    assert!(h::RELEASE_FOUND_RUNNING.load(Ordering::SeqCst) > found0, "a release waited for a running relay");
    assert!(h::RELAY_LAST.load(Ordering::SeqCst) > last0, "a relay dropped the last reference");
    std::println!("free race rounds: {} signalled by the relay, {} released first", errors[0], errors[1]);
    assert_eq!(d.stats.live()[0], 0);
}

#[test]
fn signal_races_add_remove_and_put() {
    // One thread signals, one adds and removes callbacks, one takes and
    // drops references; every callback runs at most once, a removed one
    // never, and the fence is released exactly once, after all of them.
    let d = domain();
    for round in 0..200 {
        let (fence, c) = mock(&FULL, round % 2 == 0, &d);
        // SAFETY: references for the two helper threads.
        unsafe {
            get(fence);
            get(fence);
        }
        let (p1, p2, p3) = (P(fence), P(fence), P(fence));
        let dd = d.clone();
        let adder = thread::spawn(move || {
            let p = p1;
            let mut kept = Vec::new();
            for i in 0..20 {
                let mut t = TCb::new(i, ptr::null());
                // SAFETY: the thread's reference; callbacks kept below.
                let r = unsafe { add_callback(p.0, t.raw(), Some(tcb_func)) };
                if r == 0 && i % 3 == 0 {
                    // SAFETY: as above.
                    let removed = unsafe { remove_callback(p.0, t.raw()) };
                    kept.push((t, r, removed));
                } else {
                    kept.push((t, r, false));
                }
            }
            // SAFETY: the thread's reference.
            unsafe { put(p.0, Ctx::Sleep(&sleep()), &dd) };
            kept
        });
        let dd = d.clone();
        let getter = thread::spawn(move || {
            let p = p2;
            for _ in 0..50 {
                // SAFETY: the thread holds a reference throughout.
                unsafe {
                    get(p.0);
                    put(p.0, Ctx::Atomic(&atomic()), &dd);
                }
            }
            // SAFETY: the thread's reference.
            unsafe { put(p.0, Ctx::Sleep(&sleep()), &dd) };
        });
        for _ in 0..(round % 13) {
            thread::yield_now();
        }
        // SAFETY: the test's reference.
        unsafe { signal(p3.0, None) };
        let kept = adder.join().unwrap();
        getter.join().unwrap();
        for (t, r, removed) in &kept {
            let ran = t.ran.load(Ordering::SeqCst);
            match (*r, *removed) {
                (0, true) => assert_eq!(ran, 0, "removed before it ran"),
                (0, false) => assert_eq!(ran, 1, "added and kept: ran once"),
                _ => assert_eq!(ran, 0, "refused: never runs"),
            }
        }
        assert_eq!(c.released.load(Ordering::SeqCst), 0);
        // SAFETY: the test's reference, the last.
        unsafe { put(p3.0, Ctx::Sleep(&sleep()), &d) };
        d.drain(&sleep());
        assert_eq!(c.released.load(Ordering::SeqCst), 1);
    }
}

#[test]
fn a_sibling_proxys_callback_drops_the_last_reference() {
    // Two proxies of one core fence, relays armed in the order p2, p1. The
    // core runs p2's relay first; a callback on p2 drops p1's last
    // reference while p1's relay is still queued on the same thread. The
    // release is parked (it would otherwise wait for that relay for ever);
    // p1's relay then finds the count at zero, and p1's callback runs from
    // the release with -EDEADLK.
    let leaked: &'static KArc<FenceDomain> = Box::leak(Box::new(domain()));
    let _ = TEST_DOMAIN.set(KArc::as_ptr(leaked).as_ptr() as usize);
    let d = unsafe { &*(*TEST_DOMAIN.get().unwrap() as *const FenceDomain) };
    let core = Fence::new(leaked, leaked.alloc_contexts(1), 1, &sleep()).unwrap();
    let p1 = proxy(core.clone(), leaked);
    let p2 = proxy(core.clone(), leaked);
    let mut c1 = TCb::new(1, ptr::null());
    let mut c2 = TCb::new(2, ptr::null());
    c2.put_other.store(p1, Ordering::SeqCst);
    // SAFETY: live proxies; p1's reference goes to c2.
    unsafe {
        assert_eq!(add_callback(p2, c2.raw(), Some(tcb_func)), 0);
        assert_eq!(add_callback(p1, c1.raw(), Some(tcb_func)), 0);
    }
    let c = core.clone();
    let t = thread::spawn(move || c.signal(Ctx::Sleep(&sleep())));
    assert!(t.join().unwrap(), "the signal returns");
    assert_eq!(c2.ran.load(Ordering::SeqCst), 1);
    d.drain(&sleep());
    assert_eq!(c1.ran.load(Ordering::SeqCst), 1, "p1's callback ran once");
    assert_eq!(c1.error_seen.load(Ordering::SeqCst), -EDEADLK, "from p1's release");
    // SAFETY: the test's reference to p2.
    unsafe { put(p2, Ctx::Sleep(&sleep()), d) };
    d.drain(&sleep());
    core.put(Ctx::Sleep(&sleep()));
}
