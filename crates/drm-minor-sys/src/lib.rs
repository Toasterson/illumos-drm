// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Raw bindings to `misc/drm_minor`, the kernel module that hands out DRM
//! device numbers.
//!
//! Every illumos-drm driver module links its own copy of the DRM core, so
//! nothing inside one module can know which device numbers another module
//! uses. `misc/drm_minor` (the crate `drm_minor` in `modules/drm-minor`)
//! owns the one table of numbers for the whole system; every driver module
//! depends on it (`-N misc/drm_minor`, from the `[package.metadata.kmod]`
//! table of this crate), so the kernel loads it first and cannot unload it
//! while a driver is loaded. A device number `N` names the nodes `card<N>`
//! and `renderD<128+N>`.
//!
//! The interface is a small, stable C ABI: the first cross-module piece of
//! the C interface the README plans for milestone R5. It is private to
//! illumos-drm; the functions are documented here and in the module's
//! source instead of a manual page. `cargo xtask kernel-check` checks that
//! both resolve from `drm_minor` in a running kernel.
#![no_std]

use core::ffi::{c_int, c_void};

/// The number of device numbers, 0 to 63, as Linux's primary (0 to 63) and
/// render (128 to 191) minor ranges.
pub const DRM_MINOR_COUNT: c_int = 64;

unsafe extern "C" {
    /// `drm_minor_alloc`. illumos-drm `misc/drm_minor`
    /// (`modules/drm-minor/src/lib.rs`).
    ///
    /// Takes the lowest free device number for `owner`, an opaque non-NULL
    /// token that must be passed again to free it (the core passes its
    /// `dev_info_t`). Returns the number, from 0 to
    /// [`DRM_MINOR_COUNT`]` - 1`, or -1 if every number is taken or
    /// `owner` is NULL. Never blocks and never allocates; may be called
    /// from any context.
    pub fn drm_minor_alloc(owner: *const c_void) -> c_int;
    /// `drm_minor_free`. illumos-drm `misc/drm_minor`
    /// (`modules/drm-minor/src/lib.rs`).
    ///
    /// Gives device number `id` back. Returns 0, or `EINVAL` (and logs a
    /// warning) if `id` is out of range or not held by `owner`; the number
    /// then stays as it was, so a wrong or repeated call cannot release
    /// another device's number.
    pub fn drm_minor_free(id: c_int, owner: *const c_void) -> c_int;
}
