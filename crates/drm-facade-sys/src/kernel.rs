// SPDX-License-Identifier: MIT
// Copyright 2026 The illumos-drm authors

//! The illumos kernel types the facade headers use, for the amd64 kernel
//! (LP64). Scalars are the gate's typedefs; structures that the facade only
//! passes by pointer are opaque here (cast them to the `ddi-sys` types).
//! `kmutex_t` is embedded by value (in `spinlock_t` and `lx_mutex`), so it
//! has the kernel's size: one pointer (`sys/mutex.h`).

#![allow(non_camel_case_types)]

use core::ffi::{c_char, c_int, c_long, c_longlong, c_uint, c_ulong, c_void};

/// `caddr_t`.
pub type caddr_t = *mut c_char;
/// `dev_t`, 64 bits in the LP64 kernel.
pub type dev_t = c_ulong;
/// `minor_t`.
pub type minor_t = c_uint;
/// `off_t`.
pub type off_t = c_long;
/// `offset_t`.
pub type offset_t = c_longlong;
/// `pid_t`.
pub type pid_t = c_int;
/// `uint_t`.
pub type uint_t = c_uint;
/// `ddi_umem_cookie_t`, an opaque pointer.
pub type ddi_umem_cookie_t = *mut c_void;
/// `devmap_cookie_t`, an opaque pointer.
pub type devmap_cookie_t = *mut c_void;
/// `ddi_info_cmd_t` (`DDI_INFO_DEVT2DEVINFO` 0, `DDI_INFO_DEVT2INSTANCE` 1).
pub type ddi_info_cmd_t = c_uint;

/// `va_list` of the amd64 ABI: `__va_list_tag[1]`, 24 bytes (two 32-bit
/// offsets and two pointers). The facade only passes it by pointer
/// (`struct va_format`); a pointer to it is the pointer to its first tag,
/// which is what `vsnprintf(9F)` and `vcmn_err(9F)` take.
pub type va_list = [u64; 3];

/// `kmutex_t`: `struct mutex { void *_opaque[1]; }`.
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct kmutex_t {
    /// The kernel's opaque word.
    pub _opaque: [u64; 1],
}

macro_rules! opaque {
    ($($(#[$m:meta])* $name:ident),* $(,)?) => {$(
        $(#[$m])*
        #[repr(C)]
        pub struct $name {
            _private: [u8; 0],
        }
    )*};
}

opaque! {
    /// `dev_info_t`.
    dev_info_t,
    /// `cred_t`.
    cred_t,
    /// `struct uio`.
    uio,
    /// `struct pollhead`.
    pollhead,
    /// `struct as`.
    as_,
    /// `ddi_dma_attr_t`.
    ddi_dma_attr_t,
    /// `ddi_dma_cookie_t`.
    ddi_dma_cookie_t,
}
