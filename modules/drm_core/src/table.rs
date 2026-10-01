// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The device table and the character-device entry points.
//!
//! Every DRM device of every driver has a number `N` (0 to 63) and serves
//! the minors of `card<N>`, `renderD<128+N>`, their clone opens, and its
//! dma-buf and sync-file files (`drm_core::minor`, `drm_core::anon_fd`).
//! The driver module attaches the device, so a device number carries the
//! driver's major; the entry points below, which the driver's `cb_ops`
//! point at, find the device by the number in the minor and check the
//! major, then run illumos-ddi's character glue (`table_glue`) on the
//! core's `Device`, exactly as a driver module with its own copy of the
//! core used to.

use core::ffi::{c_int, c_short, c_void};
use core::ptr;
use core::sync::atomic::{AtomicPtr, AtomicU32, Ordering};

use ddi::module::{CharTable, table_glue};
use ddi::sys;
use ddi::{Instance, Minor};
use drm_core::Device;
use drm_core::minor::{self, MAX_DEVICES};

use crate::cdriver::CDriver;

static DEVICES: [AtomicPtr<Device<CDriver>>; MAX_DEVICES as usize] =
    [const { AtomicPtr::new(ptr::null_mut()) }; MAX_DEVICES as usize];
static MAJORS: [AtomicU32; MAX_DEVICES as usize] = [const { AtomicU32::new(u32::MAX) }; MAX_DEVICES as usize];
/// The node of each published device, for getinfo(9E), which the DDI does
/// not serialise with detach(9E) and which therefore never touches the
/// device itself.
static DIPS: [AtomicPtr<c_void>; MAX_DEVICES as usize] =
    [const { AtomicPtr::new(ptr::null_mut()) }; MAX_DEVICES as usize];

/// Publishes device `d` under its number, after it is fully registered.
pub(crate) fn publish(d: &Device<CDriver>) {
    let i = d.index() as usize;
    // SAFETY: the device's node is live while it registers.
    let major = unsafe { sys::ddi_driver_major(d.dev_info().as_raw()) };
    if let (Some(p), Some(m), Some(n)) = (DEVICES.get(i), MAJORS.get(i), DIPS.get(i)) {
        n.store(d.dev_info().as_raw() as *mut c_void, Ordering::Release);
        m.store(major, Ordering::Release);
        p.store(d as *const Device<CDriver> as *mut Device<CDriver>, Ordering::Release);
    }
}

/// Withdraws device number `index` (before it unregisters).
pub(crate) fn withdraw(index: u32) {
    let i = index as usize;
    if let (Some(p), Some(m), Some(n)) = (DEVICES.get(i), MAJORS.get(i), DIPS.get(i)) {
        p.store(ptr::null_mut(), Ordering::Release);
        m.store(u32::MAX, Ordering::Release);
        n.store(ptr::null_mut(), Ordering::Release);
    }
}

/// The device number of a minor: a node or clone minor, or an anonymous
/// (dma-buf, sync-file) one.
fn index_of(m: u32) -> Option<u32> {
    minor::decode(m).map(|d| d.index).or_else(|| drm_core::anon_fd::decode(m).map(|(i, _)| i))
}

/// The device that serves device number `dev`, if its major is that
/// device's driver's.
fn device_of(dev: sys::dev_t) -> Option<(Minor, &'static Device<CDriver>)> {
    // SAFETY: plain arithmetic on a device number.
    let (maj, min) = unsafe { (sys::getmajor(dev), sys::getminor(dev)) };
    let i = index_of(min)? as usize;
    if MAJORS.get(i)?.load(Ordering::Acquire) != maj {
        return None;
    }
    let p = DEVICES.get(i)?.load(Ordering::Acquire);
    // SAFETY: published from a live registered device, which the DDI keeps
    // from unregistering while an entry point of its driver runs (see
    // CharTable).
    if p.is_null() { None } else { Some((Minor(min), unsafe { &*p })) }
}

/// The lookup behind the entry points.
pub(crate) struct Table;

// SAFETY: a device is published once registered and withdrawn by its
// driver's detach(9E) before it goes; detach refuses while a file of the
// device is open (Device::detach), and the DDI does not detach during
// open(9E). Clone minors decode to their device's number (same_device).
unsafe impl CharTable for Table {
    type Device = Device<CDriver>;

    fn lookup(dev: sys::dev_t) -> Option<(Minor, &'static Device<CDriver>)> {
        device_of(dev)
    }

    fn same_device(a: Minor, b: Minor) -> bool {
        matches!((index_of(a.0), index_of(b.0)), (Some(x), Some(y)) if x == y)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_open(devp: *mut sys::dev_t, flag: c_int, otyp: c_int, credp: *mut sys::cred_t) -> c_int {
    // SAFETY: the kernel's open(9E) arguments.
    unsafe { table_glue::open::<Table>(devp, flag, otyp, credp) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_close(dev: sys::dev_t, flag: c_int, otyp: c_int, credp: *mut sys::cred_t) -> c_int {
    // SAFETY: the kernel's close(9E) arguments.
    unsafe { table_glue::close::<Table>(dev, flag, otyp, credp) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_read(dev: sys::dev_t, uiop: *mut sys::uio_t, credp: *mut sys::cred_t) -> c_int {
    // SAFETY: the kernel's read(9E) arguments.
    unsafe { table_glue::read::<Table>(dev, uiop, credp) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_ioctl(
    dev: sys::dev_t,
    cmd: c_int,
    arg: sys::intptr_t,
    mode: c_int,
    credp: *mut sys::cred_t,
    rvalp: *mut c_int,
) -> c_int {
    // SAFETY: the kernel's ioctl(9E) arguments.
    unsafe { table_glue::ioctl::<Table>(dev, cmd, arg, mode, credp, rvalp) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_chpoll(
    dev: sys::dev_t,
    events: c_short,
    anyyet: c_int,
    reventsp: *mut c_short,
    phpp: *mut *mut sys::pollhead,
) -> c_int {
    // SAFETY: the kernel's chpoll(9E) arguments.
    unsafe { table_glue::chpoll::<Table>(dev, events, anyyet, reventsp, phpp) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_devmap(
    dev: sys::dev_t,
    dhp: sys::devmap_cookie_t,
    off: sys::offset_t,
    len: sys::size_t,
    maplen: *mut sys::size_t,
    model: sys::uint_t,
) -> c_int {
    // SAFETY: the kernel's devmap(9E) arguments.
    unsafe { table_glue::devmap::<Table>(dev, dhp, off, len, maplen, model) }
}

#[allow(clippy::too_many_arguments, reason = "the shape of segmap(9E)")]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_segmap(
    dev: sys::dev_t,
    off: sys::off_t,
    asp: *mut sys::addrspace,
    addrp: *mut sys::caddr_t,
    len: sys::off_t,
    prot: sys::c_uint,
    maxprot: sys::c_uint,
    flags: sys::c_uint,
    credp: *mut sys::cred_t,
) -> c_int {
    // SAFETY: the kernel's segmap(9E) arguments.
    unsafe { table_glue::segmap::<Table>(dev, off, asp, addrp, len, prot, maxprot, flags, credp) }
}

/// `getinfo(9E)` for a DRM driver.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_getinfo(
    _dip: sys::dev_info_t,
    cmd: sys::ddi_info_cmd_t,
    arg: *mut c_void,
    resultp: *mut *mut c_void,
) -> c_int {
    let dev = arg as usize as sys::dev_t;
    // SAFETY: plain arithmetic on a device number.
    let (maj, min) = unsafe { (sys::getmajor(dev), sys::getminor(dev)) };
    let Some(i) = index_of(min).map(|i| i as usize) else { return sys::DDI_FAILURE };
    if MAJORS.get(i).map(|m| m.load(Ordering::Acquire)) != Some(maj) {
        return sys::DDI_FAILURE;
    }
    let r = match cmd {
        sys::ddi_info_cmd_t::DDI_INFO_DEVT2INSTANCE => match minor::instance_of_minor(Minor(min)) {
            Some(Instance(i)) => i as usize as *mut c_void,
            None => return sys::DDI_FAILURE,
        },
        sys::ddi_info_cmd_t::DDI_INFO_DEVT2DEVINFO => {
            let n = DIPS.get(i).map_or(ptr::null_mut(), |n| n.load(Ordering::Acquire));
            if n.is_null() {
                return sys::DDI_FAILURE;
            }
            n
        }
        _ => return sys::DDI_FAILURE,
    };
    if !resultp.is_null() {
        // SAFETY: the kernel's result pointer.
        unsafe { *resultp = r };
    }
    sys::DDI_SUCCESS
}

/// The instance of the driver that owns a DRM minor number, or -1.
#[unsafe(no_mangle)]
pub extern "C" fn drm_minor_instance(m: sys::minor_t) -> c_int {
    minor::instance_of_minor(Minor(m)).map(|i| i.0).unwrap_or(-1)
}
