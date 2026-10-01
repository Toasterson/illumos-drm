// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The safe Rust interface of the DRM facade, for Rust DRM drivers.
//!
//! Since milestone R5a the DRM core is the kernel module `misc/drm`, and
//! every driver, in C or in Rust, uses it through the same C functions
//! (`include/drm`, bound by `drm-facade-sys`). This crate wraps those
//! functions in safe types; it calls nothing else in the core, holds no
//! state of the core, and instantiates none of the core's generics: the
//! types here are handles to `#[repr(C)]` structures of the facade.
//!
//! A driver module:
//!
//! * implements [`Driver`] on its per-device state, with its identity,
//!   features, private ioctls ([`Ioctl`]) and hooks;
//! * uses [`Device<D>`](Device) as the instance of its `ddi::Driver`,
//!   created with [`Device::register`] in `attach(9E)` and given back with
//!   [`Device::unregister`] in `detach(9E)`;
//! * points its `cb_ops` at the core: `ddi::module! { driver: X, cb_ops:
//!   drm_facade::CB_OPS.get() }`, and maps minors with
//!   [`instance_of_minor`];
//! * makes its buffer objects [`GemRef<T>`](GemRef) of a [`GemObject`]
//!   type, its fences [`Fence`]s, and describes its display with
//!   [`kms::KmsDriver`].
//!
//! The value helpers of the core (display modes and formats, EDID parsing,
//! dumb-buffer layouts, `KArc`, `KVec`, the software vblank) are
//! re-exported from `drm-core` built without its `kernel` feature: plain
//! functions and containers, which a driver may compile in.
#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]
#![warn(missing_docs)]

extern crate alloc;

pub mod device;
pub mod fence;
pub mod gem;
pub mod ioctl;
pub mod kms;
pub mod sync;
mod version;

pub use drm_facade_sys as sys;

pub use drm_core::{defer, dumb, karc, ksync, kvec, offsets};

pub use device::{CB_OPS, Dev, DevHandle, Device, Driver, File, FileId, Settings, instance_of_minor};
pub use fence::Fence;
pub use gem::{Gem, GemObject, GemRef, Usage};
pub use ioctl::{Ioctl, IoctlData, Plain};
pub use sync::Syncobj;
pub use version::check_facade;

use ddi::Errno;

/// `ENOSYS`, which `ddi::Errno` has no constant for.
pub const ENOSYS: Errno = match Errno::new(ddi::sys::errno::ENOSYS) {
    Some(e) => e,
    None => Errno::EINVAL,
};

/// The facade's error convention: 0 or a negative errno.
pub(crate) fn check(r: i32) -> Result<(), Errno> {
    if r >= 0 { Ok(()) } else { Err(Errno::new(-r).unwrap_or(Errno::EINVAL)) }
}

/// A pointer result of the facade: the pointer, or the errno of an
/// `ERR_PTR` (or `ENOMEM` for NULL).
pub(crate) fn check_ptr<T>(p: *mut T) -> Result<core::ptr::NonNull<T>, Errno> {
    let v = p as isize;
    if (-4095..0).contains(&v) {
        return Err(Errno::new(-v as i32).unwrap_or(Errno::EINVAL));
    }
    core::ptr::NonNull::new(p).ok_or(Errno::ENOMEM)
}

/// A negative errno for the facade's callbacks.
pub(crate) fn neg(e: Errno) -> i32 {
    -e.get()
}
