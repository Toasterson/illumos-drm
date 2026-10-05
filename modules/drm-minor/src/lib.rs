// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `misc/drm_minor`: the DRM device numbers of the whole system.
//!
//! Each illumos-drm driver module links its own static copy of the DRM
//! core, so before this module every copy named its first device `card0`
//! and `renderD128`, and two drivers (`rdrmnull` and `virtio_gpu`) claimed
//! the same `/dev/dri` links. This module owns one table of 64 numbers for
//! all of them. A device takes the lowest free number when it attaches and
//! gives it back when it detaches, as Linux's `drm_minor_alloc` takes the
//! lowest free index of its global `drm_minors_xa`; number `N` names the
//! nodes `card<N>` and `renderD<128+N>`.
//!
//! The interface is two C functions, declared for Rust by the crate
//! `drm-minor-sys`:
//!
//! * `int drm_minor_alloc(const void *owner)`: the lowest free number, or
//!   -1 if all 64 are taken or `owner` is NULL;
//! * `int drm_minor_free(int id, const void *owner)`: 0, or `EINVAL` if
//!   `id` is not held by `owner` (the number is then left alone).
//!
//! `owner` is an opaque token, in practice the device's `dev_info_t`. The
//! table is 64 atomic words, one per number, holding the owner or 0, so
//! both calls are lock-free, never block and never allocate, and a free
//! that does not match the owner (a wrong number, a repeated free, another
//! device's number) changes nothing. An `id_space(9F)` would have served
//! for allocation, but `id_free` of a number that is not allocated is a
//! programming error the kernel does not survive, and the owner check needs
//! the table anyway.
//!
//! The kernel keeps this module loaded while any module that depends on it
//! (`-N misc/drm_minor`) is loaded. `_fini` also refuses (`EBUSY`) while a
//! number is still taken, which can only happen if a driver unloaded
//! without giving its numbers back; they then stay reserved rather than
//! being handed out twice.
//!
//! The module is linked with a mapfile (`exports.mapfile`) that leaves only
//! `_init`, `_fini`, `_info` and the two functions global.
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_op_in_unsafe_fn)]

pub mod slots;

#[cfg(not(test))]
mod kernel {
    use core::cell::UnsafeCell;
    use core::ffi::{c_int, c_void};
    use core::ptr;

    use ddi_sys as sys;

    use crate::slots::Slots;

    static SLOTS: Slots = Slots::new();

    /// A kernel structure the module system reads and writes through a
    /// pointer; only `mod_install`, `mod_remove` and `mod_info` touch it.
    struct Linkage<T>(UnsafeCell<T>);
    // SAFETY: the module system serialises _init, _fini and _info.
    unsafe impl<T> Sync for Linkage<T> {}

    static MODLMISC: Linkage<sys::modlmisc> = Linkage(UnsafeCell::new(sys::modlmisc {
        misc_modops: &raw const sys::mod_miscops as *mut sys::mod_ops,
        misc_linkinfo: c"DRM device numbers (illumos-drm)".as_ptr(),
    }));

    static MODLINKAGE: Linkage<sys::modlinkage> = Linkage(UnsafeCell::new(sys::modlinkage {
        ml_rev: sys::MODREV_1,
        ml_linkage: [
            MODLMISC.0.get() as *mut c_void,
            ptr::null_mut(),
            ptr::null_mut(),
            ptr::null_mut(),
            ptr::null_mut(),
            ptr::null_mut(),
            ptr::null_mut(),
        ],
    }));

    /// `_init(9E)`.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn _init() -> c_int {
        // SAFETY: the linkage is a valid static; _init runs once.
        unsafe { sys::mod_install(MODLINKAGE.0.get()) }
    }

    /// `_fini(9E)`: refused while a number is taken.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn _fini() -> c_int {
        let taken = SLOTS.taken();
        if taken != 0 {
            // SAFETY: a constant format with one int argument.
            unsafe { sys::cmn_err(sys::CE_WARN, c"drm_minor: %d DRM device numbers still taken".as_ptr(), taken) };
            return sys::errno::EBUSY;
        }
        // SAFETY: the linkage is a valid static.
        unsafe { sys::mod_remove(MODLINKAGE.0.get()) }
    }

    /// `_info(9E)`.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn _info(modinfop: *mut sys::modinfo) -> c_int {
        // SAFETY: the linkage is a valid static; modinfop comes from the
        // module system.
        unsafe { sys::mod_info(MODLINKAGE.0.get(), modinfop) }
    }

    /// Takes the lowest free device number for `owner`; -1 if none is free
    /// or `owner` is NULL. See the crate documentation.
    #[unsafe(no_mangle)]
    pub extern "C" fn drm_minor_alloc(owner: *const c_void) -> c_int {
        match SLOTS.alloc(owner as usize) {
            // At most 63.
            Some(id) => id as c_int,
            None => -1,
        }
    }

    /// Gives device number `id` of `owner` back; `EINVAL` if `owner` does
    /// not hold it. See the crate documentation.
    #[unsafe(no_mangle)]
    pub extern "C" fn drm_minor_free(id: c_int, owner: *const c_void) -> c_int {
        if u32::try_from(id).is_ok_and(|i| SLOTS.free(i, owner as usize)) {
            return 0;
        }
        // SAFETY: a constant format with one int and one pointer argument.
        unsafe {
            sys::cmn_err(
                sys::CE_WARN,
                c"drm_minor: device number %d is not held by %p; left as it was".as_ptr(),
                id,
                owner,
            )
        };
        sys::errno::EINVAL
    }

    #[panic_handler]
    fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
        // Nothing in this module panics (no indexing without bounds, no
        // arithmetic that can overflow); the handler exists because a
        // no_std staticlib needs one.
        // SAFETY: a constant format without arguments.
        unsafe { sys::panic(c"drm_minor: rust panic".as_ptr()) }
    }
}
