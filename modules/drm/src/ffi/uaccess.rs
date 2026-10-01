// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `drm_copy_from_user` and `drm_copy_to_user` of `drm_ioctl.h`: copies
//! with the data model and `FKIOCTL` flag of the calling thread's ioctl.

use core::ffi::c_void;

use ddi::Errno;
use ddi::sys;

use crate::devcore::neg;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_copy_from_user(dst: *mut c_void, uaddr: u64, len: usize) -> i32 {
    if len == 0 {
        return 0;
    }
    if dst.is_null() {
        return neg(Errno::EFAULT);
    }
    let r = crate::ctxt::with_current(|_, arg| {
        // SAFETY: the caller's buffer of len bytes; ddi_copyin checks the
        // source against the caller's address space and mode.
        unsafe { sys::ddi_copyin(uaddr as usize as *const c_void, dst, len, arg.mode()) }
    });
    if r == Some(0) { 0 } else { neg(Errno::EFAULT) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_copy_to_user(uaddr: u64, src: *const c_void, len: usize) -> i32 {
    if len == 0 {
        return 0;
    }
    if src.is_null() {
        return neg(Errno::EFAULT);
    }
    let r = crate::ctxt::with_current(|_, arg| {
        // SAFETY: as above, the other way.
        unsafe { sys::ddi_copyout(src, uaddr as usize as *mut c_void, len, arg.mode()) }
    });
    if r == Some(0) { 0 } else { neg(Errno::EFAULT) }
}
