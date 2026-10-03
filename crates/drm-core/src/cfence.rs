// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Fences with Linux's public layout: Linux's `dma_fence` semantics on the
//! facade's `struct dma_fence` (`include/drm/dma_fence.h`), whether a
//! driver embedded it in its own structure or the core allocated it.
//!
//! # The public fields are the state
//!
//! The structure's fields are what Linux drivers read and write: the lock
//! (`extern_lock`, or `inline_lock` with `DMA_FENCE_FLAG_INLINE_LOCK_BIT`),
//! `cb_list` (a Linux list of `struct dma_fence_cb` under that lock),
//! `flags` (changed atomically), `timestamp` (replacing `cb_list` once
//! signalled), `error`, `refcount` and the driver's `ops`. The functions
//! here implement Linux's rules on them: [`signal_locked`] sets the
//! signalled bit, takes the callback list off the fence and runs every
//! callback with the lock held; [`add_callback`] enables signalling
//! (`ops->enable_signaling`, once) and links the callback; [`is_signaled`]
//! polls `ops->signaled`; [`wait_timeout`] calls `ops->wait` or waits
//! through the core. A driver that signals its fences itself, as i915's
//! breadcrumbs do (set the bit, replace the list, run the callbacks), is
//! seen by the core like any other signaller.
//!
//! # The core's view: one more callback
//!
//! The rest of the core (sync objects, reservations, sync files, arrays,
//! chains and waits) works on [`Fence`], its own fence object. Each
//! structure carries one in its private tail ([`FenceCore`]):
//!
//! * A driver's fence (`dma_fence_init`, a *native* fence) gets a *shadow*
//!   core fence in the module's domain. When the core first takes the
//!   structure ([`core_fence`]), it enables signalling and adds the
//!   *bridge*, a `struct dma_fence_cb` inside the tail state, to the
//!   structure's `cb_list`; the bridge signals the shadow with the
//!   structure's error. The bridge holds a reference to the structure
//!   until it has run, as a Linux sync object holds the fence it waits
//!   for, so a fence the core waits on cannot be released unsignalled.
//! * A fence the core hands out (a sync object's fence, the stub,
//!   `drm_illumos_fence_create`) gets a structure allocated by the core, a
//!   *proxy*, whose ops are the core's own ([`PROXY_OPS`]):
//!   `enable_signaling` adds the *relay*, a core callback on the core
//!   fence that signals the structure, and `signaled` asks the core fence.
//!   The relay holds no reference: it takes one with [`get_unless_zero`]
//!   and does nothing once the structure's count is zero; the structure's
//!   release removes the relay first, waiting for a relay that runs, so
//!   the memory is valid while a relay can touch it. Signalling a proxy
//!   also signals its core fence.
//!
//! Proxies and native fences therefore take the same paths; a core fence
//! may have several proxies, each with its own lock and callbacks.
//!
//! # Release
//!
//! The last [`put`] releases the structure: callbacks still on the list of
//! an unsignalled fence run with `-EDEADLK` (Linux's rule, with a
//! warning), the tail state goes (a relay is removed, which may wait), an
//! inline lock is destroyed, and `ops->release` frees the driver's memory.
//! That needs thread context and must not run under the fence's own lock.
//! So the last reference is released at once only in thread context by a
//! thread that does not hold the lock; in interrupt context, from a
//! callback, and from the bridge and relay it is parked through the tail's
//! [`RawDefer`] node on the module domain's list and released by its task
//! queue ([`FenceDomain::defer_raw`]). This also removes R5a's last spin: a
//! release never waits for a callback on the processor it interrupted.
//!
//! The logic is tested on the host with mock ops and real threads
//! (`tests` below); the kernel module's `dma_fence_*` functions are thin
//! wrappers.

use core::cell::UnsafeCell;
use core::ffi::{CStr, c_char};
use core::mem::offset_of;
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, AtomicU64, Ordering};

use ddi::{Atomic, Errno, Sleepable};
use drm_facade_sys as f;

use crate::defer::{Ctx, RawDefer};
use crate::fence::{AddError, Fence, FenceCallback, FenceCb, FenceDomain};
use crate::karc::KArc;
use crate::ksync::{self, Nanos};

/// `DMA_FENCE_FLAG_INITIALIZED_BIT`.
pub const INITIALIZED: u64 = 1 << f::DMA_FENCE_FLAG_INITIALIZED_BIT;
/// `DMA_FENCE_FLAG_INLINE_LOCK_BIT`.
pub const INLINE_LOCK: u64 = 1 << f::DMA_FENCE_FLAG_INLINE_LOCK_BIT;
/// `DMA_FENCE_FLAG_SEQNO64_BIT`.
pub const SEQNO64: u64 = 1 << f::DMA_FENCE_FLAG_SEQNO64_BIT;
/// `DMA_FENCE_FLAG_SIGNALED_BIT`.
pub const SIGNALED: u64 = 1 << f::DMA_FENCE_FLAG_SIGNALED_BIT;
/// `DMA_FENCE_FLAG_TIMESTAMP_BIT`.
pub const TIMESTAMP: u64 = 1 << f::DMA_FENCE_FLAG_TIMESTAMP_BIT;
/// `DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT`.
pub const ENABLE_SIGNAL: u64 = 1 << f::DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT;

/// Marks the tail of a structure the facade set up (XORed with the state's
/// and the structure's addresses).
const MAGIC: u64 = 0x646d_615f_6663_6531; // "dma_fce1"

/// The tail: `[0]` the [`FenceCore`], `[1]` its check, `[2..4]` the
/// deferred-release node.
const TAIL_DEFER: usize = 2;
const _: () = assert!(RawDefer::SIZE == 2 * size_of::<u64>());
const _: () = assert!(size_of::<[u64; 4]>() == 32);
const _: () = assert!(offset_of!(f::dma_fence_cb, node) == 0);

/// Linux's `-EDEADLK`, the error of a fence released with callbacks
/// pending.
const EDEADLK: i32 = 45;

// --- the lock ---------------------------------------------------------------

use crate::spinlock as lk;

/// A warning: `cmn_err(CE_WARN)` in the kernel, a counter on the host.
#[cfg(not(test))]
fn warn(msg: &CStr) {
    // SAFETY: "%s" with a NUL-terminated string.
    unsafe { ddi::sys::cmn_err(ddi::sys::CE_WARN, c"drm_core: %s".as_ptr(), msg.as_ptr()) };
}

#[cfg(test)]
pub(crate) static WARNINGS: AtomicU32 = AtomicU32::new(0);
#[cfg(test)]
fn warn(_msg: &CStr) {
    WARNINGS.fetch_add(1, Ordering::SeqCst);
}

// --- raw access ---------------------------------------------------------------

fn flags<'a>(fence: *mut f::dma_fence) -> &'a AtomicU64 {
    // SAFETY: flags is an aligned unsigned long, changed only atomically.
    unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*fence).flags).cast::<u64>()) }
}

fn refs<'a>(fence: *mut f::dma_fence) -> &'a AtomicU32 {
    // SAFETY: the kref's counter is an aligned int, changed only atomically.
    unsafe { AtomicU32::from_ptr(ptr::addr_of_mut!((*fence).refcount.refcount.refs.counter) as *mut u32) }
}

fn error_of<'a>(fence: *mut f::dma_fence) -> &'a AtomicI32 {
    // SAFETY: error is an aligned int; read and written atomically here.
    unsafe { AtomicI32::from_ptr(ptr::addr_of_mut!((*fence).error)) }
}

fn cb_list(fence: *mut f::dma_fence) -> *mut f::list_head {
    // SAFETY: only computes the field's address.
    unsafe { ptr::addr_of_mut!((*fence).__bindgen_anon_2.cb_list) }
}

fn tail(fence: *mut f::dma_fence) -> *mut u64 {
    // SAFETY: only computes the field's address.
    unsafe { ptr::addr_of_mut!((*fence).__core) as *mut u64 }
}

/// The fence's lock (Linux's `dma_fence_spinlock`).
///
/// # Safety
/// `fence` is an initialised structure.
unsafe fn lock_of(fence: *mut f::dma_fence) -> *mut f::spinlock_t {
    let u = fence as *mut u8 as *mut f::spinlock_t;
    if flags(fence).load(Ordering::Acquire) & INLINE_LOCK != 0 {
        u
    } else {
        // SAFETY: the union's first member is the external lock pointer.
        unsafe { *(fence as *mut *mut f::spinlock_t) }
    }
}

// Linux's list primitives on the C list_head.

/// # Safety
/// `h` is writable.
unsafe fn list_init(h: *mut f::list_head) {
    // SAFETY: the caller's contract.
    unsafe {
        (*h).next = h;
        (*h).prev = h;
    }
}

/// # Safety
/// `h` is a list head or an entry that was initialised.
unsafe fn list_empty(h: *const f::list_head) -> bool {
    // SAFETY: the caller's contract.
    unsafe { ptr::eq((*h).next, h) }
}

/// # Safety
/// `head` is a list, `new` an entry on no list; the list's lock is held.
unsafe fn list_add_tail(new: *mut f::list_head, head: *mut f::list_head) {
    // SAFETY: the caller's contract.
    unsafe {
        let prev = (*head).prev;
        (*new).next = head;
        (*new).prev = prev;
        (*prev).next = new;
        (*head).prev = new;
    }
}

/// # Safety
/// `e` is on a list whose lock is held.
unsafe fn list_del_init(e: *mut f::list_head) {
    // SAFETY: the caller's contract.
    unsafe {
        let (p, n) = ((*e).prev, (*e).next);
        (*p).next = n;
        (*n).prev = p;
        list_init(e);
    }
}

/// Moves the entries of `old` to `new` (Linux's `list_replace`); an empty
/// `old` leaves `new` empty. `old` is garbage afterwards.
///
/// # Safety
/// Both writable; `old`'s lock is held.
unsafe fn list_replace(old: *mut f::list_head, new: *mut f::list_head) {
    // SAFETY: the caller's contract.
    unsafe {
        (*new).next = (*old).next;
        (*(*new).next).prev = new;
        (*new).prev = (*old).prev;
        (*(*new).prev).next = new;
    }
}

// --- the tail state -------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Kind {
    /// A driver's structure with a shadow core fence.
    Native,
    /// A structure the core made for a core fence.
    Proxy,
}

/// The core's state of one structure, owned through the tail.
#[repr(C)]
pub struct FenceCore {
    /// The bridge of a native fence: on the structure's `cb_list` from
    /// [`core_fence`] until the fence signals.
    bridge: UnsafeCell<f::dma_fence_cb>,
    kind: Kind,
    /// The shadow (native) or the proxied core fence; taken at release.
    fence: UnsafeCell<Option<Fence>>,
    /// The bridge was added (under the fence's lock).
    bridged: AtomicBool,
    /// The relay of a proxy; added by `enable_signaling` (under the lock).
    relay: Option<FenceCb<Relay>>,
    relayed: AtomicBool,
    /// Where releases from atomic contexts are parked.
    domain: KArc<FenceDomain>,
}

// SAFETY: the C callback is touched only under the structure's lock; the
// core fence is read-only until the release, which owns the structure.
unsafe impl Send for FenceCore {}
// SAFETY: as above.
unsafe impl Sync for FenceCore {}

impl FenceCore {
    /// The core fence: set at creation, taken only by the release, which
    /// clears the tail first, so a caller that holds a reference always
    /// finds it. `None` only for a caller that broke that rule (a driver
    /// using a released structure), which then gets "signalled" answers
    /// instead of a panic.
    fn fence(&self) -> Option<&Fence> {
        // SAFETY: see above; the release owns the structure when it takes
        // the fence.
        unsafe { (*self.fence.get()).as_ref() }
    }
}

/// The [`FenceCore`] of a structure the facade set up.
///
/// # Safety
/// `fence` is NULL or a structure with a reference the caller holds (or
/// that the caller is releasing).
pub unsafe fn core_of<'a>(fence: *const f::dma_fence) -> Option<&'a FenceCore> {
    if fence.is_null() {
        return None;
    }
    let t = tail(fence as *mut f::dma_fence);
    // SAFETY: the caller's contract.
    let (p, check) = unsafe { (*t, *t.add(1)) };
    if p == 0 || p ^ MAGIC ^ (fence as u64) != check {
        return None;
    }
    // SAFETY: a FenceCore owned by the structure (set_core).
    Some(unsafe { &*(p as *const FenceCore) })
}

fn set_core(fence: *mut f::dma_fence, core: KArc<FenceCore>) {
    let p = KArc::into_raw(core).as_ptr() as u64;
    let t = tail(fence);
    // SAFETY: the caller's structure, not shared yet.
    unsafe {
        *t = p;
        *t.add(1) = p ^ MAGIC ^ (fence as u64);
    }
}

/// Takes the state out of the tail (the release).
///
/// # Safety
/// The caller releases the structure.
unsafe fn take_core(fence: *mut f::dma_fence) -> Option<KArc<FenceCore>> {
    // SAFETY: the caller's contract.
    unsafe { core_of(fence) }?;
    let t = tail(fence);
    // SAFETY: as above; the pointer came from set_core.
    unsafe {
        let p = *t;
        *t = 0;
        *t.add(1) = 0;
        Some(KArc::from_raw(NonNull::new_unchecked(p as *mut FenceCore)))
    }
}

// --- initialisation -----------------------------------------------------------

/// Initialises the public fields as Linux's `__dma_fence_init` does, with
/// an inline lock for a NULL `lock`. Any context.
///
/// # Safety
/// `fence` is writable and not in use; `ops` and `lock` stay valid while
/// the fence exists.
unsafe fn init_public(
    fence: *mut f::dma_fence,
    ops: *const f::dma_fence_ops,
    lock: *mut f::spinlock_t,
    context: u64,
    seqno: u64,
    extra: u64,
) {
    // SAFETY: the caller's contract.
    unsafe {
        (*fence).__core = [0; 4];
        let fl = if lock.is_null() {
            lk::init(fence as *mut u8 as *mut f::spinlock_t);
            INITIALIZED | INLINE_LOCK | extra
        } else {
            *(fence as *mut *mut f::spinlock_t) = lock;
            INITIALIZED | extra
        };
        (*fence).ops = ops;
        list_init(cb_list(fence));
        (*fence).context = context;
        (*fence).seqno = seqno;
        (*fence).error = 0;
        (*fence).refcount.refcount.refs.counter = 1;
        (*fence).flags = fl as core::ffi::c_ulong;
    }
    // SAFETY: as above.
    let named = unsafe { !ops.is_null() && (*ops).get_driver_name.is_some() && (*ops).get_timeline_name.is_some() };
    if !named {
        warn(c"dma_fence_init: ops without get_driver_name/get_timeline_name; the fence is named \"unknown\"");
    }
}

/// `dma_fence_init` and `dma_fence_init64` (`extra` = [`SEQNO64`]): the
/// public fields, and a shadow core fence in `domain` (`KM_NOSLEEP`, any
/// context). Without memory the fence starts signalled with `-ENOMEM`.
///
/// # Safety
/// As [`init_public`].
pub unsafe fn init(
    fence: *mut f::dma_fence,
    ops: *const f::dma_fence_ops,
    lock: *mut f::spinlock_t,
    context: u64,
    seqno: u64,
    extra: u64,
    domain: &KArc<FenceDomain>,
) {
    if fence.is_null() {
        return;
    }
    // SAFETY: the caller's contract.
    unsafe { init_public(fence, ops, lock, context, seqno, extra & SEQNO64) };
    let core = Fence::try_new(domain, context, seqno).ok().and_then(|shadow| {
        KArc::try_new(FenceCore {
            // SAFETY: plain data; zero is valid, the node is set below.
            bridge: UnsafeCell::new(unsafe { core::mem::zeroed() }),
            kind: Kind::Native,
            fence: UnsafeCell::new(Some(shadow)),
            bridged: AtomicBool::new(false),
            relay: None,
            relayed: AtomicBool::new(false),
            domain: domain.clone(),
        })
        .ok()
    });
    match core {
        Some(c) => {
            // SAFETY: the bridge is empty until added.
            unsafe { list_init(ptr::addr_of_mut!((*c.bridge.get()).node)) };
            set_core(fence, c);
        }
        None => {
            // Nothing may wait for it forever: signalled, with an error.
            error_of(fence).store(-(Errno::ENOMEM.get()), Ordering::Release);
            // SAFETY: not shared yet: the time is the only state to set.
            unsafe { (*fence).__bindgen_anon_2.timestamp = ksync::now() };
            flags(fence).fetch_or(SIGNALED | TIMESTAMP, Ordering::AcqRel);
            warn(c"dma_fence_init: no memory; fence signalled with ENOMEM");
        }
    }
}

// --- proxies --------------------------------------------------------------------

/// The memory of a proxy.
#[repr(C)]
struct ProxyFence {
    c: UnsafeCell<f::dma_fence>,
}

// SAFETY: the structure is only changed through this module's functions.
unsafe impl Send for ProxyFence {}
// SAFETY: as above.
unsafe impl Sync for ProxyFence {}

unsafe extern "C" fn proxy_name(_f: *mut f::dma_fence) -> *const c_char {
    c"drm".as_ptr()
}

unsafe extern "C" fn proxy_enable_signaling(fence: *mut f::dma_fence) -> bool {
    // SAFETY: called by enable_locked with the structure's lock held and a
    // reference held by the caller.
    let Some(core) = (unsafe { core_of(fence) }) else { return false };
    if core.relayed.load(Ordering::Acquire) {
        return true;
    }
    let (Some(relay), Some(cf)) = (&core.relay, core.fence()) else { return false };
    match cf.add_callback(relay) {
        Ok(()) => {
            core.relayed.store(true, Ordering::Release);
            true
        }
        Err(AddError::Signaled) => false,
        // Added already (cannot happen: enable_signaling runs once).
        Err(AddError::Busy) => true,
    }
}

unsafe extern "C" fn proxy_signaled(fence: *mut f::dma_fence) -> bool {
    // SAFETY: the caller holds a reference.
    match unsafe { core_of(fence) } {
        Some(core) => core.fence().is_none_or(Fence::is_signaled),
        None => true,
    }
}

unsafe extern "C" fn proxy_release(fence: *mut f::dma_fence) {
    // SAFETY: the structure is the first field of a ProxyFence made by
    // proxy(), whose allocation this was.
    drop(unsafe { KArc::from_raw(NonNull::new_unchecked(fence as *mut ProxyFence)) });
}

/// The ops of the structures the core makes for its own fences.
pub static PROXY_OPS: f::dma_fence_ops = f::dma_fence_ops {
    get_driver_name: Some(proxy_name),
    get_timeline_name: Some(proxy_name),
    enable_signaling: Some(proxy_enable_signaling),
    signaled: Some(proxy_signaled),
    wait: None,
    release: Some(proxy_release),
    set_deadline: None,
};

/// The relay of a proxy: signals the structure when its core fence
/// signals elsewhere.
struct Relay {
    c: *mut f::dma_fence,
}

// SAFETY: the structure outlives every run of the relay (its release
// removes the relay first, waiting for a run in progress).
unsafe impl Send for Relay {}
// SAFETY: as above.
unsafe impl Sync for Relay {}

impl FenceCallback for Relay {
    fn signaled(&self, core_fence: &Fence, _ctx: Ctx<'_>) -> Option<Fence> {
        let c = self.c;
        // A structure being released is left alone.
        if !get_unless_zero(c) {
            return None;
        }
        if flags(c).load(Ordering::Acquire) & SIGNALED == 0 {
            let ts = core_fence.timestamp().unwrap_or_else(ksync::now);
            // SAFETY: a live structure (the reference just taken).
            unsafe {
                let l = lock_of(c);
                lk::enter(l);
                if let Some(e) = core_fence.error() {
                    let _ = error_of(c).compare_exchange(0, -e.get(), Ordering::AcqRel, Ordering::Acquire);
                }
                signal_locked_c(c, ts);
                lk::exit(l);
            }
        }
        // Never released here: the release waits for this run to end.
        // SAFETY: the reference taken above.
        unsafe { put_deferred(c, None) };
        None
    }
}

/// A new structure for core fence `fence`, with one reference; NULL
/// without memory. Any context.
pub fn proxy(fence: Fence, domain: &KArc<FenceDomain>) -> *mut f::dma_fence {
    // SAFETY: plain data; zero is valid.
    let pf = ProxyFence { c: UnsafeCell::new(unsafe { core::mem::zeroed() }) };
    let Ok(arc) = KArc::try_new(pf) else { return ptr::null_mut() };
    let raw = KArc::into_raw(arc);
    let c = raw.as_ptr() as *mut f::dma_fence;
    let Ok(relay) = FenceCb::try_new(Relay { c }) else {
        // SAFETY: never shared.
        drop(unsafe { KArc::from_raw(raw) });
        return ptr::null_mut();
    };
    let (context, seqno) = (fence.context(), fence.seqno());
    let core = KArc::try_new(FenceCore {
        // SAFETY: plain data; zero is valid.
        bridge: UnsafeCell::new(unsafe { core::mem::zeroed() }),
        kind: Kind::Proxy,
        fence: UnsafeCell::new(Some(fence)),
        bridged: AtomicBool::new(false),
        relay: Some(relay),
        relayed: AtomicBool::new(false),
        domain: domain.clone(),
    });
    let Ok(core) = core else {
        // SAFETY: never shared.
        drop(unsafe { KArc::from_raw(raw) });
        return ptr::null_mut();
    };
    // SAFETY: just allocated, not shared.
    unsafe {
        init_public(c, &PROXY_OPS, ptr::null_mut(), context, seqno, 0);
        list_init(ptr::addr_of_mut!((*core.bridge.get()).node));
    }
    set_core(c, core);
    // A signalled core fence makes a signalled structure.
    // SAFETY: as above.
    if unsafe { proxy_signaled(c) } {
        // SAFETY: as above.
        unsafe { signal(c, None) };
    }
    c
}

// --- references and release ------------------------------------------------------

/// `dma_fence_get`.
///
/// # Safety
/// `fence` is NULL or holds a reference of the caller's.
pub unsafe fn get(fence: *mut f::dma_fence) -> *mut f::dma_fence {
    if !fence.is_null() {
        refs(fence).fetch_add(1, Ordering::Relaxed);
    }
    fence
}

/// Takes a reference unless the count is zero (Linux's
/// `kref_get_unless_zero`).
pub fn get_unless_zero(fence: *mut f::dma_fence) -> bool {
    let r = refs(fence);
    let mut n = r.load(Ordering::Acquire);
    loop {
        if n == 0 {
            return false;
        }
        match r.compare_exchange_weak(n, n + 1, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => return true,
            Err(m) => n = m,
        }
    }
}

/// Drops a reference; `true` if it was the last. A count that is zero
/// already is left alone, with a warning (a driver's extra put).
fn drop_ref(fence: *mut f::dma_fence) -> bool {
    let r = refs(fence);
    let mut n = r.load(Ordering::Acquire);
    loop {
        if n == 0 {
            warn(c"dma_fence_put: the fence has no reference left");
            return false;
        }
        match r.compare_exchange_weak(n, n - 1, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => return n == 1,
            Err(m) => n = m,
        }
    }
}

/// `dma_fence_put` in the caller's context: the last reference is released
/// at once in thread context by a thread that does not hold the fence's
/// lock, and parked on `domain` otherwise.
///
/// # Safety
/// `fence` is NULL or a structure whose reference the caller gives up.
pub unsafe fn put(fence: *mut f::dma_fence, ctx: Ctx<'_>, domain: &FenceDomain) {
    if fence.is_null() || !drop_ref(fence) {
        return;
    }
    match ctx {
        // SAFETY: the last reference; the lock is initialised.
        Ctx::Sleep(s) if !unsafe { lk::owned(lock_of(fence)) } => unsafe { release(fence, s) },
        // SAFETY: the last reference.
        _ => unsafe { defer(fence, domain) },
    }
}

/// Drops a reference and parks the release if it was the last; any
/// context. `domain` is where to park it, the structure's own domain for
/// `None` (a structure with tail state).
///
/// # Safety
/// As [`put`].
pub unsafe fn put_deferred(fence: *mut f::dma_fence, domain: Option<&FenceDomain>) {
    if fence.is_null() || !drop_ref(fence) {
        return;
    }
    match domain {
        // SAFETY: the last reference.
        Some(d) => unsafe { defer(fence, d) },
        // SAFETY: the last reference; the tail state is the structure's
        // until its release, which runs only after the push below.
        None => match unsafe { core_of(fence) } {
            Some(core) => {
                let d = core.domain.clone();
                // SAFETY: as above.
                unsafe { defer(fence, &d) };
            }
            // Unreachable (relay and bridge exist only with tail state);
            // leaked rather than released in an unknown context.
            None => warn(c"dma_fence_put: deferred release of a fence without core state; leaked"),
        },
    }
}

unsafe fn run_release(node: NonNull<RawDefer>, ctx: &Sleepable) {
    let off = offset_of!(f::dma_fence, __core) + TAIL_DEFER * size_of::<u64>();
    // SAFETY: the node was parked by defer() at this offset of a structure.
    let fence = unsafe { (node.as_ptr() as *mut u8).sub(off) } as *mut f::dma_fence;
    // SAFETY: the structure's last reference was handed to the drain.
    unsafe { release(fence, ctx) };
}

/// Parks the release of `fence` on `domain`'s task queue.
///
/// # Safety
/// The caller dropped the last reference.
unsafe fn defer(fence: *mut f::dma_fence, domain: &FenceDomain) {
    // SAFETY: the tail's node belongs to the release from now on.
    unsafe {
        let node = NonNull::new_unchecked(tail(fence).add(TAIL_DEFER) as *mut RawDefer);
        domain.defer_raw(node, run_release);
    }
}

/// Releases a structure whose last reference is gone. Thread context, the
/// fence's lock not held.
///
/// # Safety
/// The caller owns the structure (its count is zero).
pub unsafe fn release(fence: *mut f::dma_fence, ctx: &Sleepable) {
    // SAFETY: the caller's contract.
    unsafe {
        let l = lock_of(fence);
        lk::enter(l);
        if flags(fence).load(Ordering::Acquire) & SIGNALED == 0 && !list_empty(cb_list(fence)) {
            // Linux's rule: the callbacks run, with an error, rather than
            // wait for ever.
            warn(c"dma_fence: fence released with pending callbacks; signalled with EDEADLK");
            error_of(fence).store(-EDEADLK, Ordering::Release);
            signal_locked_c(fence, ksync::now());
        }
        lk::exit(l);
        if let Some(core) = take_core(fence) {
            if let (Some(r), Some(cf)) = (&core.relay, core.fence())
                && core.relayed.load(Ordering::Acquire)
            {
                // Waits for a relay that runs now on another thread (never
                // this one: the relay does not release).
                cf.remove_callback(r, ctx);
            }
            if let Some(f) = (*core.fence.get()).take() {
                f.put(Ctx::Sleep(ctx));
            }
            drop(core);
        }
        if flags(fence).load(Ordering::Acquire) & INLINE_LOCK != 0 {
            lk::destroy(l);
        }
        let ops = (*fence).ops;
        if !ops.is_null()
            && let Some(rel) = (*ops).release
        {
            rel(fence);
        }
    }
}

// --- signalling -----------------------------------------------------------------

/// Linux's `dma_fence_signal_timestamp_locked` on the public fields:
/// returns `false` if the fence had signalled already.
///
/// # Safety
/// `fence` is live and its lock is held by the caller.
unsafe fn signal_locked_c(fence: *mut f::dma_fence, ts: Nanos) -> bool {
    if flags(fence).fetch_or(SIGNALED, Ordering::AcqRel) & SIGNALED != 0 {
        return false;
    }
    let mut local = f::list_head { next: ptr::null_mut(), prev: ptr::null_mut() };
    let lp = ptr::addr_of_mut!(local);
    // SAFETY: the caller's contract; the list is the fence's until the
    // timestamp replaces it.
    unsafe {
        list_replace(cb_list(fence), lp);
        (*fence).__bindgen_anon_2.timestamp = ts;
    }
    flags(fence).fetch_or(TIMESTAMP, Ordering::AcqRel);
    // SAFETY: the callbacks are the drivers' and stay valid until they have
    // run; each is emptied first, and the next one read before its
    // function runs (which may free it).
    unsafe {
        let mut cur = (*lp).next;
        while !ptr::eq(cur, lp) {
            let next = (*cur).next;
            list_init(cur);
            let cb = cur as *mut f::dma_fence_cb;
            if let Some(func) = (*cb).func {
                func(fence, cb);
            }
            cur = next;
        }
    }
    true
}

/// [`signal_locked_c`], then, for a proxy, its core fence (under the
/// lock: the proxy's own relay finds the signalled bit and leaves the
/// lock alone; other structures' relays take their own locks).
///
/// # Safety
/// As [`signal_locked_c`].
unsafe fn signal_locked_full(fence: *mut f::dma_fence, ts: Nanos) -> bool {
    // SAFETY: the caller's contract.
    let core = unsafe { core_of(fence) };
    let proxied = core.filter(|c| c.kind == Kind::Proxy).and_then(FenceCore::fence);
    if let Some(e) = proxied.and_then(Fence::error) {
        let _ = error_of(fence).compare_exchange(0, -e.get(), Ordering::AcqRel, Ordering::Acquire);
    }
    // SAFETY: the caller's contract.
    let did = unsafe { signal_locked_c(fence, ts) };
    if let Some(cf) = proxied {
        let e = error_of(fence).load(Ordering::Acquire);
        if e < 0
            && let Some(e) = Errno::new(-e)
        {
            cf.set_error(e);
        }
        // SAFETY: signalling never blocks; releases it causes are parked.
        let a = unsafe { Atomic::assume() };
        cf.signal(Ctx::Atomic(&a));
    }
    did
}

/// `dma_fence_signal_timestamp_locked` (`ts` `None`: now). Returns whether
/// this call signalled it.
///
/// # Safety
/// `fence` is live (the caller holds a reference) and the caller holds its
/// lock.
pub unsafe fn signal_locked(fence: *mut f::dma_fence, ts: Option<Nanos>) -> bool {
    if !initialised(fence) {
        return false;
    }
    // SAFETY: the caller's contract.
    unsafe { signal_locked_full(fence, ts.unwrap_or_else(ksync::now)) }
}

/// `dma_fence_signal_timestamp` (`ts` `None`: now): takes the lock.
///
/// # Safety
/// `fence` is live (the caller holds a reference); the caller does not hold
/// its lock.
pub unsafe fn signal(fence: *mut f::dma_fence, ts: Option<Nanos>) -> bool {
    if !initialised(fence) {
        return false;
    }
    // SAFETY: the caller's contract.
    unsafe {
        let l = lock_of(fence);
        lk::enter(l);
        let did = signal_locked_full(fence, ts.unwrap_or_else(ksync::now));
        lk::exit(l);
        did
    }
}

/// Whether the signalled bit is set (Linux's
/// `dma_fence_test_signaled_flag`).
///
/// # Safety
/// `fence` is live.
pub unsafe fn signaled_flag(fence: *mut f::dma_fence) -> bool {
    !fence.is_null() && flags(fence).load(Ordering::Acquire) & SIGNALED != 0
}

/// Whether `fence` was initialised (`DMA_FENCE_FLAG_INITIALIZED_BIT`),
/// without a warning.
///
/// # Safety
/// `fence` is NULL or readable.
pub unsafe fn is_initialised(fence: *mut f::dma_fence) -> bool {
    !fence.is_null() && flags(fence).load(Ordering::Acquire) & INITIALIZED != 0
}

/// Whether the structure was initialised by `dma_fence_init` (warns
/// otherwise: a driver's uninitialised or freed structure).
fn initialised(fence: *mut f::dma_fence) -> bool {
    if fence.is_null() {
        return false;
    }
    if flags(fence).load(Ordering::Acquire) & INITIALIZED == 0 {
        warn(c"dma_fence: function called on a fence that was not initialised");
        return false;
    }
    true
}

/// Linux's `__dma_fence_enable_signaling`: calls `ops->enable_signaling`
/// once and signals the fence if it returns false. Returns whether the
/// fence is still unsignalled.
///
/// # Safety
/// `fence` is live and its lock held by the caller.
unsafe fn enable_locked(fence: *mut f::dma_fence) -> bool {
    let was = flags(fence).fetch_or(ENABLE_SIGNAL, Ordering::AcqRel) & ENABLE_SIGNAL != 0;
    if flags(fence).load(Ordering::Acquire) & SIGNALED != 0 {
        return false;
    }
    // SAFETY: the caller's contract.
    unsafe {
        let ops = (*fence).ops;
        if !was
            && !ops.is_null()
            && let Some(en) = (*ops).enable_signaling
            && !en(fence)
        {
            signal_locked_full(fence, ksync::now());
            return false;
        }
    }
    true
}

/// `dma_fence_enable_sw_signaling`.
///
/// # Safety
/// `fence` is live; the caller does not hold its lock.
pub unsafe fn enable_sw_signaling(fence: *mut f::dma_fence) {
    if !initialised(fence) {
        return;
    }
    // SAFETY: the caller's contract.
    unsafe {
        let l = lock_of(fence);
        lk::enter(l);
        enable_locked(fence);
        lk::exit(l);
    }
}

/// `dma_fence_is_signaled`: the bit, or `ops->signaled` (which signals the
/// fence when true).
///
/// # Safety
/// `fence` is live; the caller does not hold its lock.
pub unsafe fn is_signaled(fence: *mut f::dma_fence) -> bool {
    if !initialised(fence) {
        return fence.is_null();
    }
    if flags(fence).load(Ordering::Acquire) & SIGNALED != 0 {
        return true;
    }
    // SAFETY: the caller's contract.
    unsafe {
        let ops = (*fence).ops;
        if !ops.is_null()
            && let Some(s) = (*ops).signaled
            && s(fence)
        {
            signal(fence, None);
            return true;
        }
    }
    false
}

/// `dma_fence_get_status`: 0, 1 or the negative error.
///
/// # Safety
/// As [`is_signaled`].
pub unsafe fn get_status(fence: *mut f::dma_fence) -> i32 {
    if !initialised(fence) {
        return -Errno::EINVAL.get();
    }
    // SAFETY: the caller's contract.
    unsafe {
        let l = lock_of(fence);
        lk::enter(l);
        let mut s = flags(fence).load(Ordering::Acquire) & SIGNALED != 0;
        let ops = (*fence).ops;
        if !s
            && !ops.is_null()
            && let Some(p) = (*ops).signaled
            && p(fence)
        {
            signal_locked_full(fence, ksync::now());
            s = true;
        }
        lk::exit(l);
        if !s {
            return 0;
        }
    }
    let e = error_of(fence).load(Ordering::Acquire);
    if e < 0 { e } else { 1 }
}

/// `dma_fence_set_error`: refused (with a warning) once signalled, and for
/// values that are no negative errno.
///
/// # Safety
/// `fence` is live.
pub unsafe fn set_error(fence: *mut f::dma_fence, error: i32) {
    if !initialised(fence) {
        return;
    }
    if !(-4095..0).contains(&error) {
        warn(c"dma_fence_set_error: not a negative errno; ignored");
        return;
    }
    if flags(fence).load(Ordering::Acquire) & SIGNALED != 0 {
        warn(c"dma_fence_set_error: the fence has signalled; ignored");
        return;
    }
    error_of(fence).store(error, Ordering::Release);
    // SAFETY: the caller's contract.
    if let Some(c) = unsafe { core_of(fence) }
        && c.kind == Kind::Proxy
        && let (Some(cf), Some(e)) = (c.fence(), Errno::new(-error))
    {
        cf.set_error(e);
    }
}

/// `dma_fence_set_deadline`.
///
/// # Safety
/// `fence` is live.
pub unsafe fn set_deadline(fence: *mut f::dma_fence, deadline: Nanos) {
    if !initialised(fence) || flags(fence).load(Ordering::Acquire) & SIGNALED != 0 {
        return;
    }
    // SAFETY: the caller's contract.
    unsafe {
        let ops = (*fence).ops;
        if !ops.is_null()
            && let Some(d) = (*ops).set_deadline
        {
            d(fence, deadline);
        }
    }
}

/// `dma_fence_driver_name` and `dma_fence_timeline_name`.
///
/// # Safety
/// `fence` is live.
pub unsafe fn name(fence: *mut f::dma_fence, timeline: bool) -> *const c_char {
    let unknown = c"unknown".as_ptr();
    if fence.is_null() {
        return unknown;
    }
    // SAFETY: the caller's contract.
    unsafe {
        let ops = (*fence).ops;
        if ops.is_null() {
            return unknown;
        }
        let g = if timeline { (*ops).get_timeline_name } else { (*ops).get_driver_name };
        match g {
            Some(g) => {
                let p = g(fence);
                if p.is_null() { unknown } else { p }
            }
            None => unknown,
        }
    }
}

// --- callbacks ----------------------------------------------------------------

/// `dma_fence_add_callback`: 0, `-ENOENT` (signalled), `-EINVAL`.
///
/// # Safety
/// `fence` is live; `cb` is the caller's and stays valid until it has run
/// or been removed; the caller does not hold the fence's lock.
pub unsafe fn add_callback(fence: *mut f::dma_fence, cb: *mut f::dma_fence_cb, func: f::dma_fence_func_t) -> i32 {
    if cb.is_null() || func.is_none() || !initialised(fence) {
        return -Errno::EINVAL.get();
    }
    // SAFETY: the caller's contract.
    unsafe {
        let node = ptr::addr_of_mut!((*cb).node);
        if flags(fence).load(Ordering::Acquire) & SIGNALED != 0 {
            list_init(node);
            return -Errno::ENOENT.get();
        }
        let l = lock_of(fence);
        lk::enter(l);
        let r = if enable_locked(fence) {
            (*cb).func = func;
            list_add_tail(node, cb_list(fence));
            0
        } else {
            list_init(node);
            -Errno::ENOENT.get()
        };
        lk::exit(l);
        r
    }
}

/// `dma_fence_remove_callback`: whether it was removed before it ran.
///
/// # Safety
/// As [`add_callback`]; `cb` was added to this fence (or emptied by it).
pub unsafe fn remove_callback(fence: *mut f::dma_fence, cb: *mut f::dma_fence_cb) -> bool {
    if cb.is_null() || !initialised(fence) {
        return false;
    }
    // SAFETY: the caller's contract.
    unsafe {
        let l = lock_of(fence);
        lk::enter(l);
        let node = ptr::addr_of_mut!((*cb).node);
        let on = !list_empty(node);
        if on {
            list_del_init(node);
        }
        lk::exit(l);
        on
    }
}

// --- the core's view ---------------------------------------------------------------

unsafe extern "C" fn bridge_fn(fence: *mut f::dma_fence, cb: *mut f::dma_fence_cb) {
    // SAFETY: the bridge is the first field of a FenceCore owned by
    // `fence`, which is live: the bridge holds a reference.
    let core = unsafe { &*((cb as *mut u8).sub(offset_of!(FenceCore, bridge)) as *const FenceCore) };
    if let Some(shadow) = core.fence() {
        let e = error_of(fence).load(Ordering::Acquire);
        if e < 0
            && let Some(e) = Errno::new(-e)
        {
            shadow.set_error(e);
        }
        // SAFETY: signalling never blocks; this runs with the fence's lock
        // held in any context.
        let a = unsafe { Atomic::assume() };
        shadow.signal(Ctx::Atomic(&a));
    }
    // The bridge's reference; the release is parked (the lock is held).
    // SAFETY: that reference; the core state is not touched afterwards.
    unsafe { put_deferred(fence, None) };
}

/// The core fence of a structure, for the core's sync objects,
/// reservations, sync files and waits: a native fence's shadow (adding
/// the bridge, which enables signalling, the first time), a proxy's core
/// fence; `None` for a structure without core state (one that
/// `dma_fence_init` signalled for lack of memory, or a stranger).
///
/// # Safety
/// `fence` is live; the caller does not hold its lock.
pub unsafe fn core_fence(fence: *mut f::dma_fence) -> Option<Fence> {
    // SAFETY: the caller's contract.
    let core = unsafe { core_of(fence) }?;
    let cf = core.fence()?;
    if core.kind == Kind::Proxy {
        return Some(cf.clone());
    }
    // SAFETY: as above.
    let signaled = unsafe {
        let l = lock_of(fence);
        lk::enter(l);
        if !core.bridged.load(Ordering::Acquire) && enable_locked(fence) {
            // The bridge's reference, dropped when it has run.
            get(fence);
            let b = core.bridge.get();
            (*b).func = Some(bridge_fn);
            list_add_tail(ptr::addr_of_mut!((*b).node), cb_list(fence));
            core.bridged.store(true, Ordering::Release);
        }
        let s = flags(fence).load(Ordering::Acquire) & SIGNALED != 0;
        lk::exit(l);
        s
    };
    let shadow = cf;
    if signaled && !shadow.is_signaled() {
        // Signalled before the bridge was added (or without one).
        let e = error_of(fence).load(Ordering::Acquire);
        if e < 0
            && let Some(e) = Errno::new(-e)
        {
            shadow.set_error(e);
        }
        // SAFETY: signalling never blocks.
        let a = unsafe { Atomic::assume() };
        shadow.signal(Ctx::Atomic(&a));
    }
    Some(shadow.clone())
}

/// Clock ticks and nanoseconds, for [`wait_timeout`].
pub trait TickClock {
    /// Nanoseconds of `t` ticks.
    fn ticks_to_ns(&self, t: i64) -> i64;
    /// Ticks of `ns` nanoseconds, rounded down.
    fn ns_to_ticks(&self, ns: i64) -> i64;
}

/// The deadline of a timeout in ticks (`MAX_SCHEDULE_TIMEOUT`, `i64::MAX`,
/// for none).
pub fn deadline(timeout: i64, clock: &impl TickClock) -> Option<Nanos> {
    if timeout == i64::MAX || timeout < 0 {
        None
    } else {
        Some(ksync::now().saturating_add(clock.ticks_to_ns(timeout)))
    }
}

/// The result of a wait as Linux returns it: the remaining ticks (at
/// least 1), 0 on timeout, the negative errno.
pub fn wait_result(r: Result<(), Errno>, timeout: i64, deadline: Option<Nanos>, clock: &impl TickClock) -> i64 {
    match r {
        Ok(()) => match deadline {
            None => timeout.max(1),
            Some(d) => clock.ns_to_ticks(d.saturating_sub(ksync::now())).max(1),
        },
        Err(e) if e.get() == ksync::ETIME.get() => 0,
        Err(e) => -i64::from(e.get()),
    }
}

/// `dma_fence_default_wait`: waits through the core fence (the bridge of
/// a native fence wakes it).
///
/// # Safety
/// `fence` is live; thread context, the fence's lock not held.
pub unsafe fn default_wait(
    fence: *mut f::dma_fence,
    intr: bool,
    timeout: i64,
    clock: &impl TickClock,
    ctx: &Sleepable,
) -> i64 {
    if !initialised(fence) {
        return -i64::from(Errno::EINVAL.get());
    }
    if flags(fence).load(Ordering::Acquire) & SIGNALED != 0 {
        return timeout.max(1);
    }
    if timeout == 0 {
        return 0;
    }
    // SAFETY: the caller's contract.
    let Some(cf) = (unsafe { core_fence(fence) }) else {
        // Without core state the structure was signalled at init.
        return timeout.max(1);
    };
    let d = deadline(timeout, clock);
    let r = wait_result(cf.wait(intr, d, ctx), timeout, d, clock);
    cf.put(Ctx::Sleep(ctx));
    r
}

/// `dma_fence_wait_timeout`: enables signalling, then `ops->wait` or
/// [`default_wait`]. `-EINVAL` for a negative timeout.
///
/// # Safety
/// As [`default_wait`].
pub unsafe fn wait_timeout(
    fence: *mut f::dma_fence,
    intr: bool,
    timeout: i64,
    clock: &impl TickClock,
    ctx: &Sleepable,
) -> i64 {
    if timeout < 0 || !initialised(fence) {
        return -i64::from(Errno::EINVAL.get());
    }
    // SAFETY: the caller's contract.
    unsafe {
        enable_sw_signaling(fence);
        let ops = (*fence).ops;
        if !ops.is_null()
            && let Some(w) = (*ops).wait
        {
            return w(fence, intr, timeout);
        }
        default_wait(fence, intr, timeout, clock, ctx)
    }
}

#[cfg(test)]
mod tests;
