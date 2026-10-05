// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `misc/drm_core`: the DRM core of illumos-drm as one kernel module, and the C
//! facade every DRM driver links against.
//!
//! Until milestone R5a each driver module linked its own copy of the core
//! (`drm-core`). Now the core lives here, monomorphised once for the
//! facade driver (`cdriver::CDriver`), which forwards every hook of the
//! core's `Driver` trait to a driver's C table (`struct drm_driver` of
//! `include/drm`). A driver module, in C (`cdrmnull`) or in Rust
//! (`rdrmnull`, `virtio_gpu` through the `drm-facade` crate), registers its
//! devices with `drm_dev_register` and
//! points its `cb_ops` at the entry points of `table`, which find the
//! device of a device number in the module's device table and run
//! illumos-ddi's character glue (`table_glue`) on it. Every other call is
//! one of the facade's C functions (`ffi`), whose names, types and
//! semantics the headers in `include/drm` define.
//!
//! Generic code stays inside: the facade is plain functions over
//! `#[repr(C)]` structures, so no driver instantiates a generic of the
//! core, and the core's Rust types never cross the module boundary. The
//! link reduces every symbol but `_init`, `_fini`, `_info` and the facade
//! to local scope (`drm_core.mapfile`, generated from the headers).
//!
//! What the module itself owns: the device numbers (formerly
//! `misc/drm_minor`), the device table, the fence domain of fences that
//! belong to no device (`dma_fence_init` on a driver's structure, kstat
//! `drm:0:sync`), and the table of ioctl calls in progress that lets the
//! facade's copy and file-descriptor functions find the ioctl argument of
//! the calling thread. `_fini` refuses while a device number is taken or a
//! fence of the module's domain is alive.
#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]

extern crate alloc;

#[cfg(not(test))]
mod cdriver;
#[cfg(not(test))]
mod ckms;
#[cfg(not(test))]
mod ctxt;
#[cfg(not(test))]
mod devcore;
#[cfg(not(test))]
mod ffi;
#[cfg(not(test))]
mod fobj;
#[cfg(not(test))]
mod table;

/// Marks the `__core` tail of a structure the facade set up, so that a
/// function handed a structure it did not initialise refuses it rather
/// than follow a stray pointer. XORed with the structure's address.
pub(crate) const MAGIC_DEVICE: u64 = 0x6472_6d5f_6465_7630; // "drm_dev0"
/// As [`MAGIC_DEVICE`], for `struct drm_gem_object`.
pub(crate) const MAGIC_GEM: u64 = 0x6472_6d5f_6765_6d30; // "drm_gem0"
/// As [`MAGIC_DEVICE`], for `struct drm_file`.
pub(crate) const MAGIC_FILE: u64 = 0x6472_6d5f_6669_6c30; // "drm_fil0"
/// As [`MAGIC_DEVICE`], for `struct dma_buf`.
pub(crate) const MAGIC_DMABUF: u64 = 0x646d_615f_6275_6630; // "dma_buf0"

#[cfg(not(test))]
mod kernel {
    use core::cell::UnsafeCell;
    use core::ffi::{c_int, c_void};
    use core::ptr;

    use ddi::Sleepable;
    use ddi::sys;
    use drm_core::fence::FenceDomain;
    use drm_core::karc::KArc;

    struct Linkage<T>(UnsafeCell<T>);
    // SAFETY: the kernel serialises module installation and removal; the
    // structures are only read by it afterwards.
    unsafe impl<T> Sync for Linkage<T> {}

    static MODLMISC: Linkage<sys::modlmisc> = Linkage(UnsafeCell::new(sys::modlmisc {
        misc_modops: &raw const sys::mod_miscops as *mut sys::mod_ops,
        misc_linkinfo: c"DRM core and facade (illumos-drm)".as_ptr(),
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

    /// The fence domain of fences that belong to no device. Set in
    /// `_init` before the module is installed and cleared in `_fini` after
    /// it is removed, so every facade call sees it set.
    static FENCES: Linkage<Option<KArc<FenceDomain>>> = Linkage(UnsafeCell::new(None));

    /// A signalled fence of the module's domain, for `dma_fence_get_stub`
    /// and for structures without a core fence.
    static STUB: Linkage<Option<drm_core::fence::Fence>> = Linkage(UnsafeCell::new(None));

    /// A new reference to the module's signalled stub fence.
    pub(crate) fn kernel_stub() -> Option<drm_core::fence::Fence> {
        // SAFETY: as FENCES.
        unsafe { (*STUB.0.get()).clone() }
    }

    /// The module's fence domain.
    pub(crate) fn module_fences() -> &'static KArc<FenceDomain> {
        // SAFETY: written only in _init before mod_install and in _fini
        // after mod_remove, when no facade call can run (every caller is a
        // dependent module, which the kernel loads after and unloads before
        // this one).
        match unsafe { &*FENCES.0.get() } {
            Some(d) => d,
            // Unreachable while the module is installed.
            None => unsafe { sys::panic(c"drm_core: facade called while the module is not installed".as_ptr()) },
        }
    }

    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn _init() -> c_int {
        // SAFETY: _init(9E) runs in kernel context and may block.
        let ctx = unsafe { Sleepable::assume() };
        // Never beside the legacy core: OpenIndiana's gfx-drm delivers its
        // C core as misc/drm (and its drivers, i915 among them, depend on
        // it). Both packages install together, but the two stacks must
        // never be loaded at once.
        if ddi::private::module_loaded(c"drm", &ctx) {
            // SAFETY: a constant message.
            unsafe {
                sys::cmn_err(
                    sys::CE_WARN,
                    c"drm_core: the legacy DRM core misc/drm (gfx-drm) is loaded; misc/drm_core does not load beside it (unload gfx-drm's drivers and misc/drm first)".as_ptr(),
                )
            };
            return sys::errno::EBUSY;
        }
        let Ok(d) = FenceDomain::for_module(&ctx) else { return sys::errno::ENOMEM };
        if crate::ctxt::init(&ctx).is_err() {
            return sys::errno::ENOMEM;
        }
        let Ok(stub) = drm_core::fence::Fence::signaled_stub(&d, &ctx) else {
            crate::ctxt::fini();
            return sys::errno::ENOMEM;
        };
        // SAFETY: nothing else runs yet (see module_fences).
        unsafe {
            *FENCES.0.get() = Some(d);
            *STUB.0.get() = Some(stub);
        }
        // SAFETY: the linkage is a valid static.
        let r = unsafe { sys::mod_install(MODLINKAGE.0.get()) };
        if r != 0 {
            // SAFETY: the module was not installed.
            unsafe {
                *STUB.0.get() = None;
                *FENCES.0.get() = None;
            }
            crate::ctxt::fini();
        }
        r
    }

    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn _fini() -> c_int {
        // SAFETY: _fini(9E) runs in kernel context and may block.
        let ctx = unsafe { Sleepable::assume() };
        let taken = drm_core::numbers_taken();
        if taken != 0 {
            // SAFETY: a constant format with one int argument.
            unsafe { sys::cmn_err(sys::CE_WARN, c"drm_core: %d DRM devices still registered".as_ptr(), taken) };
            return sys::errno::EBUSY;
        }
        // The stub is the module's own fence; it goes first and comes back
        // if the module stays.
        // SAFETY: drivers are unloaded (they depend on this module), so no
        // facade call runs.
        let stub = unsafe { (*STUB.0.get()).take() };
        drop(stub);
        if let Err(live) = FenceDomain::quiesce(module_fences(), &ctx) {
            if let Ok(s) = drm_core::fence::Fence::signaled_stub(module_fences(), &ctx) {
                // SAFETY: as above.
                unsafe { *STUB.0.get() = Some(s) };
            }
            let fences = live.first().copied().unwrap_or(0);
            // SAFETY: a constant format with one long argument.
            unsafe {
                sys::cmn_err(sys::CE_WARN, c"drm_core: %ld fences of drivers' structures still alive".as_ptr(), fences)
            };
            return sys::errno::EBUSY;
        }
        // SAFETY: the linkage is a valid static.
        let r = unsafe { sys::mod_remove(MODLINKAGE.0.get()) };
        if r == 0 {
            // SAFETY: the module is removed; no facade call can run.
            let d = unsafe { (*FENCES.0.get()).take() };
            drop(d);
            crate::ctxt::fini();
        } else if let Ok(s) = drm_core::fence::Fence::signaled_stub(module_fences(), &ctx) {
            // The module stays: so does its stub fence.
            // SAFETY: as above.
            unsafe { *STUB.0.get() = Some(s) };
        }
        r
    }

    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn _info(modinfop: *mut sys::modinfo) -> c_int {
        // SAFETY: the kernel passes a valid modinfo; the linkage is static.
        unsafe { sys::mod_info(MODLINKAGE.0.get(), modinfop) }
    }
}

#[cfg(not(test))]
pub(crate) use kernel::{kernel_stub, module_fences};
