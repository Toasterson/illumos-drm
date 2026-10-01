// SPDX-License-Identifier: MIT
// Copyright 2026 The illumos-drm authors

//! Raw bindings to the DRM facade of illumos-drm, the C interface of the
//! kernel module `misc/drm_core`.
//!
//! The C headers in `include/drm` are the single source of truth; this
//! crate is generated from them by `tools/facade/generate.sh` (bindgen),
//! with bindgen's size and offset assertions for every structure.
//! `cargo xtask facade-check` compares the same sizes and offsets with
//! what gcc computes on illumos. The functions are implemented by
//! `misc/drm_core` (`modules/drm_core`); a driver that uses them is linked with
//! `-N misc/drm_core` (from this crate's `[package.metadata.kmod]` table).
//!
//! The safe interface for Rust drivers is the `drm-facade` crate.
#![no_std]
#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, missing_docs)]
#![allow(clippy::all)]

mod kernel;

mod bindings {
    #![allow(unsafe_op_in_unsafe_fn, unnecessary_transmutes)]
    include!("bindings.rs");
}

pub use bindings::*;
pub use kernel::*;
