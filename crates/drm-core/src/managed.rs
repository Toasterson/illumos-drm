// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Managed resources of a device: Linux's `drm_managed` (`drmm_*`) on the
//! public `drm_device.managed` (`include/drm/drm_managed.h`).
//!
//! A driver registers release actions and allocations that live as long
//! as its device. Each is a node on `managed.resources`, a Linux list
//! under `managed.lock`, added at the head; when the device's last
//! reference goes ([`release_all`], after the driver's `release`
//! callback, before the device structure is freed) the nodes are taken
//! from the head, so they are released in the reverse order of their
//! registration, each action called and each allocation freed with no
//! lock held. [`release_action`] runs one action early, [`kfree`] frees
//! one allocation early.
//!
//! The data of an allocation follows its node's header, 8-byte aligned
//! (Linux's kmalloc alignment on x86-64). The logic is tested on the host
//! (`tests` below).

use core::ffi::{CStr, c_char, c_void};
use core::mem::size_of;
use core::ptr;

use drm_facade_sys as f;

use crate::kmem::{self, Flag};
use crate::spinlock as lk;

/// A release action: `fn(dev, data)`.
pub type Action = unsafe extern "C" fn(*mut f::drm_device, *mut c_void);

#[repr(C)]
struct Node {
    entry: f::list_head,
    release: Option<Action>,
    /// The action's data, or the allocation's data area.
    data: *mut c_void,
    name: *const c_char,
    /// The node's allocation size, header included.
    total: usize,
}

/// The header before an allocation's data.
const HDR: usize = size_of::<Node>().next_multiple_of(16);

#[cfg(not(test))]
fn warn(msg: &CStr) {
    // SAFETY: "%s" with a NUL-terminated string.
    unsafe { ddi::sys::cmn_err(ddi::sys::CE_WARN, c"drm_core: %s".as_ptr(), msg.as_ptr()) };
}

#[cfg(test)]
static WARNINGS: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
#[cfg(test)]
fn warn(_msg: &CStr) {
    WARNINGS.fetch_add(1, core::sync::atomic::Ordering::SeqCst);
}

#[cfg(test)]
std::thread_local! {
    static FAIL_ALLOC: core::cell::Cell<bool> = const { core::cell::Cell::new(false) };
}
#[cfg(test)]
extern crate std;

fn resources(dev: *mut f::drm_device) -> *mut f::list_head {
    // SAFETY: only computes the field's address.
    unsafe { ptr::addr_of_mut!((*dev).managed.resources) }
}

fn lock(dev: *mut f::drm_device) -> *mut f::spinlock_t {
    // SAFETY: only computes the field's address.
    unsafe { ptr::addr_of_mut!((*dev).managed.lock) }
}

/// Sets up `managed` of a new device structure: an empty list, its lock,
/// and `final_kfree`, the address of the driver's structure (informative:
/// the core frees it after the resources).
///
/// # Safety
/// `dev` is a device structure no other thread uses yet. Thread context.
pub unsafe fn init(dev: *mut f::drm_device, final_kfree: *mut c_void) {
    // SAFETY: the caller's contract.
    unsafe {
        let r = resources(dev);
        (*r).next = r;
        (*r).prev = r;
        lk::init(lock(dev));
        (*dev).managed.final_kfree = final_kfree;
    }
}

/// Destroys `managed.lock` once [`release_all`] has run.
///
/// # Safety
/// No other thread uses the device. Thread context.
pub unsafe fn fini(dev: *mut f::drm_device) {
    // SAFETY: the caller's contract.
    unsafe { lk::destroy(lock(dev)) };
}

fn alloc_node(data_size: usize, sleep: bool) -> *mut Node {
    let Some(total) = HDR.checked_add(data_size) else { return ptr::null_mut() };
    #[cfg(test)]
    if FAIL_ALLOC.with(|c| c.get()) {
        return ptr::null_mut();
    }
    let p = kmem::alloc(total, if sleep { Flag::Sleep } else { Flag::NoSleep }) as *mut Node;
    if !p.is_null() {
        // SAFETY: a new allocation of at least HDR bytes.
        unsafe {
            ptr::write(
                p,
                Node {
                    entry: f::list_head { next: ptr::null_mut(), prev: ptr::null_mut() },
                    release: None,
                    data: ptr::null_mut(),
                    name: ptr::null(),
                    total,
                },
            )
        };
    }
    p
}

/// # Safety
/// `n` came from [`alloc_node`] and is on no list.
unsafe fn free_node(n: *mut Node) {
    // SAFETY: the caller's contract.
    unsafe { kmem::free(n as *mut u8, (*n).total) };
}

/// Links `n` at the head of the list (Linux's `list_add`).
///
/// # Safety
/// `dev` was set up with [`init`]; `n` is a new node.
unsafe fn link(dev: *mut f::drm_device, n: *mut Node) {
    let head = resources(dev);
    // SAFETY: the caller's contract; the list changes under the lock.
    unsafe {
        lk::enter(lock(dev));
        let e = ptr::addr_of_mut!((*n).entry);
        let first = (*head).next;
        (*e).next = first;
        (*e).prev = head;
        (*first).prev = e;
        (*head).next = e;
        lk::exit(lock(dev));
    }
}

/// Unlinks the first node from the head (`newest`) or the tail for which
/// `pred` holds. Lock taken here.
///
/// # Safety
/// As [`link`].
unsafe fn unlink_where(dev: *mut f::drm_device, newest: bool, pred: impl Fn(&Node) -> bool) -> *mut Node {
    let head = resources(dev);
    // SAFETY: the caller's contract; nodes on the list are live.
    unsafe {
        lk::enter(lock(dev));
        let mut cur = if newest { (*head).next } else { (*head).prev };
        let mut found = ptr::null_mut();
        while !ptr::eq(cur, head) {
            let n = cur as *mut Node;
            if pred(&*n) {
                (*(*cur).prev).next = (*cur).next;
                (*(*cur).next).prev = (*cur).prev;
                found = n;
                break;
            }
            cur = if newest { (*cur).next } else { (*cur).prev };
        }
        lk::exit(lock(dev));
        found
    }
}

/// `__drmm_add_action`: 0, `-EINVAL` (no action), `-ENOMEM`.
///
/// # Safety
/// `dev` was set up with [`init`] and holds a reference of the caller's;
/// thread context if `sleep`.
pub unsafe fn add_action(
    dev: *mut f::drm_device,
    action: Option<Action>,
    data: *mut c_void,
    name: *const c_char,
) -> i32 {
    if action.is_none() {
        return -ddi::Errno::EINVAL.get();
    }
    let n = alloc_node(0, true);
    if n.is_null() {
        return -ddi::Errno::ENOMEM.get();
    }
    // SAFETY: the new node; the caller's device.
    unsafe {
        (*n).release = action;
        (*n).data = data;
        (*n).name = name;
        link(dev, n);
    }
    0
}

/// `__drmm_add_action_or_reset`: as [`add_action`], but a failure calls
/// the action at once.
///
/// # Safety
/// As [`add_action`].
pub unsafe fn add_action_or_reset(
    dev: *mut f::drm_device,
    action: Option<Action>,
    data: *mut c_void,
    name: *const c_char,
) -> i32 {
    // SAFETY: the caller's contract.
    let r = unsafe { add_action(dev, action, data, name) };
    if r != 0
        && let Some(a) = action
    {
        // SAFETY: the driver's action with its own data.
        unsafe { a(dev, data) };
    }
    r
}

/// `drmm_release_action`: calls the oldest registration of `action` with
/// `data` now and removes it; `false` (with a warning) if there is none.
///
/// # Safety
/// As [`add_action`].
pub unsafe fn release_action(dev: *mut f::drm_device, action: Option<Action>, data: *mut c_void) -> bool {
    let Some(a) = action else { return false };
    // SAFETY: the caller's contract.
    let n = unsafe { unlink_where(dev, false, |n| n.release.is_some_and(|r| ptr::fn_addr_eq(r, a)) && n.data == data) };
    if n.is_null() {
        warn(c"drmm_release_action: no such action");
        return false;
    }
    // SAFETY: the node is ours now; the driver's action.
    unsafe {
        a(dev, data);
        free_node(n);
    }
    true
}

/// `drmm_kmalloc`: `size` bytes that live until the device's release (or
/// [`kfree`]), zeroed with `zero`; NULL for 0 bytes or without memory
/// (never with `sleep`).
///
/// # Safety
/// As [`add_action`]; thread context if `sleep`.
pub unsafe fn kmalloc(
    dev: *mut f::drm_device,
    size: usize,
    zero: bool,
    sleep: bool,
    name: *const c_char,
) -> *mut c_void {
    if size == 0 {
        return ptr::null_mut();
    }
    let n = alloc_node(size, sleep);
    if n.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: the new node with `size` bytes after the header.
    unsafe {
        let d = (n as *mut u8).add(HDR);
        if zero {
            ptr::write_bytes(d, 0, size);
        }
        (*n).data = d as *mut c_void;
        (*n).name = name;
        link(dev, n);
        d as *mut c_void
    }
}

/// `drmm_kstrdup`: a managed copy of `s`; NULL for NULL or without memory.
///
/// # Safety
/// As [`kmalloc`]; `s` is NULL or a NUL-terminated string.
pub unsafe fn kstrdup(dev: *mut f::drm_device, s: *const c_char, sleep: bool) -> *mut c_char {
    if s.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: the caller's string.
    let b = unsafe { CStr::from_ptr(s) }.to_bytes_with_nul();
    // SAFETY: the caller's device.
    let p = unsafe { kmalloc(dev, b.len(), false, sleep, c"drmm_kstrdup".as_ptr()) } as *mut u8;
    if !p.is_null() {
        // SAFETY: a new area of b.len() bytes.
        unsafe { ptr::copy_nonoverlapping(b.as_ptr(), p, b.len()) };
    }
    p as *mut c_char
}

/// `drmm_kfree`: frees a managed allocation early; NULL is allowed, an
/// address that is no managed allocation of `dev` is left alone with a
/// warning.
///
/// # Safety
/// As [`add_action`].
pub unsafe fn kfree(dev: *mut f::drm_device, data: *mut c_void) {
    if data.is_null() {
        return;
    }
    // SAFETY: the caller's contract.
    let n = unsafe { unlink_where(dev, true, |n| n.release.is_none() && n.data == data) };
    if n.is_null() {
        warn(c"drmm_kfree: not a managed allocation of this device");
        return;
    }
    // SAFETY: the node is ours now.
    unsafe { free_node(n) };
}

/// Releases every resource, newest first, and returns how many there were.
/// Called once when the device's last reference goes.
///
/// # Safety
/// `dev` was set up with [`init`]; nothing else uses it but the actions.
/// Thread context.
pub unsafe fn release_all(dev: *mut f::drm_device) -> usize {
    let mut count = 0;
    loop {
        // SAFETY: the caller's contract.
        let n = unsafe { unlink_where(dev, true, |_| true) };
        if n.is_null() {
            return count;
        }
        // SAFETY: the node is ours; the driver's action with its data.
        unsafe {
            if let Some(a) = (*n).release {
                a(dev, (*n).data);
            }
            free_node(n);
        }
        count += 1;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::sync::atomic::Ordering;
    use std::boxed::Box;
    use std::sync::Mutex;
    use std::thread;
    use std::vec::Vec;

    static LOG: Mutex<Vec<(usize, usize)>> = Mutex::new(Vec::new());

    /// Records (device, data) in the log.
    unsafe extern "C" fn record(dev: *mut f::drm_device, data: *mut c_void) {
        LOG.lock().unwrap().push((dev as usize, data as usize));
    }

    /// A device for one test. Never freed: the log is keyed by the
    /// device's address, which must stay unique while tests run in
    /// parallel.
    fn device() -> *mut f::drm_device {
        // SAFETY: plain data; zero is valid.
        let d: Box<f::drm_device> = Box::new(unsafe { core::mem::zeroed() });
        let p = Box::into_raw(d);
        // SAFETY: a new structure.
        unsafe { init(p, p as *mut c_void) };
        p
    }

    fn log_of(dev: *mut f::drm_device) -> Vec<usize> {
        LOG.lock().unwrap().iter().filter(|(d, _)| *d == dev as usize).map(|(_, x)| *x).collect()
    }

    #[test]
    fn released_in_reverse_order_once() {
        let dev = device();
        // SAFETY: the test's device.
        unsafe {
            assert_eq!(add_action(dev, Some(record), 1 as *mut c_void, c"one".as_ptr()), 0);
            let a = kmalloc(dev, 24, true, true, ptr::null()) as *mut u8;
            assert!(!a.is_null());
            assert_eq!(a as usize % 8, 0, "8-byte aligned");
            assert!((0..24).all(|i| *a.add(i) == 0), "zeroed");
            assert_eq!(add_action(dev, Some(record), 2 as *mut c_void, c"two".as_ptr()), 0);
            let s = kstrdup(dev, c"name".as_ptr(), true);
            assert_eq!(CStr::from_ptr(s), c"name");
            assert_eq!(add_action(dev, Some(record), 3 as *mut c_void, c"three".as_ptr()), 0);
            assert_eq!(add_action(dev, None, ptr::null_mut(), ptr::null()), -ddi::Errno::EINVAL.get());
            // Early: the second action runs now, the allocation goes now.
            assert!(release_action(dev, Some(record), 2 as *mut c_void));
            assert_eq!(log_of(dev), [2]);
            kfree(dev, a as *mut c_void);
            kfree(dev, ptr::null_mut());
            assert_eq!(release_all(dev), 3, "two actions and the string");
            assert_eq!(log_of(dev), [2, 3, 1], "reverse order, each once");
            assert_eq!(release_all(dev), 0);
            fini(dev);
        }
    }

    #[test]
    fn misuse_is_refused_with_a_warning() {
        let dev = device();
        let before = WARNINGS.load(Ordering::SeqCst);
        let mut stack = 0u64;
        // SAFETY: the test's device.
        unsafe {
            assert!(!release_action(dev, Some(record), 9 as *mut c_void));
            kfree(dev, ptr::addr_of_mut!(stack) as *mut c_void);
            assert_eq!(WARNINGS.load(Ordering::SeqCst), before + 2);
            assert!(kmalloc(dev, 0, false, true, ptr::null()).is_null());
            assert_eq!(release_all(dev), 0);
            fini(dev);
        }
    }

    #[test]
    fn add_or_reset_calls_the_action_on_failure() {
        let dev = device();
        // SAFETY: the test's device.
        unsafe {
            FAIL_ALLOC.with(|c| c.set(true));
            let r = add_action_or_reset(dev, Some(record), 7 as *mut c_void, c"seven".as_ptr());
            assert!(kmalloc(dev, 8, false, false, ptr::null()).is_null());
            FAIL_ALLOC.with(|c| c.set(false));
            assert_eq!(r, -ddi::Errno::ENOMEM.get());
            assert_eq!(log_of(dev), [7], "called at once");
            assert_eq!(release_all(dev), 0, "and not registered");
            fini(dev);
        }
    }

    struct D(*mut f::drm_device);
    // SAFETY: the device's functions are thread-safe.
    unsafe impl Send for D {}
    // SAFETY: as above.
    unsafe impl Sync for D {}

    /// Four threads register and free at once; the release then runs every
    /// remaining action once, each thread's newest first.
    #[test]
    fn concurrent_registration() {
        let dev = D(device());
        thread::scope(|s| {
            for t in 0..4usize {
                let dev = &dev;
                s.spawn(move || {
                    for i in 0..100usize {
                        // SAFETY: the test's device.
                        unsafe {
                            let v = (1000 + t * 1000 + i) as *mut c_void;
                            assert_eq!(add_action(dev.0, Some(record), v, ptr::null()), 0);
                            let p = kmalloc(dev.0, 16 + i, i % 2 == 0, true, ptr::null());
                            if i % 3 == 0 {
                                kfree(dev.0, p);
                            }
                        }
                    }
                });
            }
        });
        // SAFETY: the test's device, no other user now.
        unsafe {
            assert_eq!(release_all(dev.0), 400 + 400 - 136);
            let l = log_of(dev.0);
            assert_eq!(l.len(), 400);
            for t in 0..4usize {
                let mine: Vec<usize> = l.iter().copied().filter(|v| (v / 1000) == t + 1).collect();
                assert!(mine.windows(2).all(|w| w[0] > w[1]), "thread {t}: newest first");
            }
            fini(dev.0);
        }
    }
}
