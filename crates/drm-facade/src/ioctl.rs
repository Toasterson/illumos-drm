// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Driver ioctls: the table entries, the argument the core copies in and
//! out, and copies from user addresses named in it.

use alloc::vec::Vec;
use core::ffi::{c_int, c_void};
use core::mem::{MaybeUninit, size_of};

use ddi::Errno;
use drm_facade_sys as sys;

use crate::device::{Dev, Driver, File};
use crate::{check, neg};

/// The most private ioctls a Rust driver can have.
pub const MAX_IOCTLS: usize = 32;

/// One private ioctl of a driver: its complete command value, its
/// permission flags (`DRM_AUTH`, `DRM_RENDER_ALLOW`, ... of
/// `drm_facade_sys`) and its handler. Several entries may share a number
/// with different structure sizes (the layouts of older userland): a
/// command matches the entry with its whole value first, else the first
/// entry with its number and direction, with Linux's size rule.
pub struct Ioctl<D: Driver> {
    /// The command value.
    pub cmd: u32,
    /// The permission flags.
    pub flags: u32,
    /// The handler.
    pub func: fn(&Dev<D>, &mut IoctlData<'_>, &File) -> Result<(), Errno>,
}

/// A `#[repr(C)]` structure every bit pattern of which is a valid value:
/// integer fields only.
///
/// # Safety
/// The implementing type has integer fields only and no invariants.
pub unsafe trait Plain: Copy + 'static {}

// SAFETY: integers.
unsafe impl Plain for u8 {}
// SAFETY: integers.
unsafe impl Plain for u32 {}
// SAFETY: integers.
unsafe impl Plain for u64 {}

/// The argument of an ioctl, as the core copied it in: the table entry's
/// size, zero-extended. What the handler leaves in it is copied out when
/// it returns `Ok` (for commands that copy out).
pub struct IoctlData<'a> {
    buf: &'a mut [u8],
}

impl IoctlData<'_> {
    /// The structure, if it has the size of `T`.
    pub fn read<T: Plain>(&self) -> Result<T, Errno> {
        if self.buf.len() < size_of::<T>() {
            return Err(Errno::EINVAL);
        }
        // SAFETY: at least size_of::<T>() bytes; T is Plain (any bits);
        // read_unaligned because the buffer has no alignment of T's.
        Ok(unsafe { (self.buf.as_ptr() as *const T).read_unaligned() })
    }

    /// Writes the structure back for the copy-out.
    pub fn write<T: Plain>(&mut self, v: &T) -> Result<(), Errno> {
        if self.buf.len() < size_of::<T>() {
            return Err(Errno::EINVAL);
        }
        // SAFETY: as in read.
        unsafe { (self.buf.as_mut_ptr() as *mut T).write_unaligned(*v) };
        Ok(())
    }

    /// The size of the argument (the table entry's).
    pub fn len(&self) -> usize {
        self.buf.len()
    }

    /// Whether the argument is empty (an `_IO` command).
    pub fn is_empty(&self) -> bool {
        self.buf.is_empty()
    }

    /// The command value of the matched table entry.
    pub fn cmd(&self) -> u32 {
        // SAFETY: inside a handler.
        unsafe { sys::drm_illumos_ioctl_cmd() }
    }

    /// The caller's command value.
    pub fn user_cmd(&self) -> u32 {
        // SAFETY: inside a handler.
        unsafe { sys::drm_illumos_ioctl_user_cmd() }
    }
}

/// Copies `dst.len()` bytes from user address `uaddr` of the ioctl being
/// served.
pub fn copy_in(uaddr: u64, dst: &mut [u8]) -> Result<(), Errno> {
    // SAFETY: a buffer of the length given; the facade checks the source.
    check(unsafe { sys::drm_copy_from_user(dst.as_mut_ptr().cast(), uaddr, dst.len()) })
}

/// Copies `src` to user address `uaddr` of the ioctl being served.
pub fn copy_out(uaddr: u64, src: &[u8]) -> Result<(), Errno> {
    // SAFETY: as above.
    check(unsafe { sys::drm_copy_to_user(uaddr, src.as_ptr().cast(), src.len()) })
}

/// Copies `n` structures from user address `uaddr` (`EINVAL` on overflow).
pub fn copy_in_array<T: Plain>(uaddr: u64, n: usize) -> Result<Vec<T>, Errno> {
    let bytes = n.checked_mul(size_of::<T>()).ok_or(Errno::EINVAL)?;
    let mut v: Vec<T> = Vec::new();
    v.try_reserve_exact(n).map_err(|_| Errno::ENOMEM)?;
    // SAFETY: n elements of capacity; T is Plain, so the copied bytes are
    // valid values once written.
    unsafe {
        let p = v.as_mut_ptr() as *mut MaybeUninit<u8>;
        let dst = core::slice::from_raw_parts_mut(p as *mut u8, bytes);
        copy_in(uaddr, dst)?;
        v.set_len(n);
    }
    Ok(v)
}

/// Copies structures to user address `uaddr`.
pub fn copy_out_array<T: Plain>(uaddr: u64, src: &[T]) -> Result<(), Errno> {
    // SAFETY: the slice's bytes; T has no padding requirements beyond Plain.
    let b = unsafe { core::slice::from_raw_parts(src.as_ptr() as *const u8, size_of_val(src)) };
    copy_out(uaddr, b)
}

/// The number of entries of a driver's table.
pub(crate) const fn count<D: Driver>() -> usize {
    let n = D::IOCTLS.len();
    assert!(n <= MAX_IOCTLS, "too many driver ioctls");
    n
}

/// The facade entries of a driver's table: every entry calls the same
/// thunk, which finds the handler by the matched command.
pub(crate) const fn descs<D: Driver>() -> [sys::drm_ioctl_desc; MAX_IOCTLS] {
    let mut out = [sys::drm_ioctl_desc { cmd: 0, flags: 0, func: None, name: core::ptr::null() }; MAX_IOCTLS];
    let n = count::<D>();
    let mut i = 0;
    while i < n {
        out[i] = sys::drm_ioctl_desc {
            cmd: D::IOCTLS[i].cmd,
            flags: D::IOCTLS[i].flags,
            func: Some(thunk::<D>),
            name: c"driver ioctl".as_ptr(),
        };
        i += 1;
    }
    out
}

unsafe extern "C" fn thunk<D: Driver>(dev: *mut sys::drm_device, data: *mut c_void, file: *mut sys::drm_file) -> c_int {
    // SAFETY: inside a handler.
    let cmd = unsafe { sys::drm_illumos_ioctl_cmd() };
    let Some(e) = D::IOCTLS.iter().find(|e| e.cmd == cmd) else { return neg(Errno::ENOTTY) };
    // SAFETY: the facade passes this driver's device, the kernel copy of
    // the argument with the entry's size, and the calling file.
    let (d, f) = unsafe { (Dev::<D>::from_raw(dev), File::from_raw(file)) };
    let Some(f) = f else { return neg(Errno::EINVAL) };
    let len = drm_uapi::ioc::ioc_size(cmd);
    let buf: &mut [u8] = if len == 0 || data.is_null() {
        &mut []
    } else {
        // SAFETY: the core's buffer holds at least the entry's size.
        unsafe { core::slice::from_raw_parts_mut(data as *mut u8, len) }
    };
    let mut a = IoctlData { buf };
    match (e.func)(d, &mut a, &f) {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}
