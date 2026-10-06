//! The DRM userland interface as Rust types.
//!
//! This crate holds the `#[repr(C)]` argument structures and the constants
//! of the DRM ioctl interface, generated with bindgen from the libdrm 2.4.109
//! headers with the illumos patch that gfx-drm applies (`drm_handle_t` is
//! 64 bits wide on illumos), and the `DRM_IOCTL_*` command numbers in the
//! illumos `_IOWR` encoding. It covers `drm.h`, `drm_mode.h` and
//! `virtgpu_drm.h`; the last comes from Linux v6.12 rather than libdrm
//! (`tools/uapi/include/README`), with the two older layouts of
//! `drm_virtgpu_execbuffer` in [`virtgpu_compat`](crate::drm_virtgpu_execbuffer_v1).
//! A few later Linux additions the Linux KMS code needs are written by hand
//! ([`DRM_IOCTL_MODE_CLOSEFB`], [`DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP`]).
//! Only the 64-bit ABI exists: the kernel refuses 32-bit callers.
//!
//! The generated files carry the MIT licence of the headers they are
//! derived from (see the notice below); everything else is MPL-2.0.
//! `tools/uapi/generate.sh` regenerates them, and `cargo xtask uapi-check`
//! compares every size, offset and command number with what gcc computes
//! on illumos from the same headers.
//!
//! The crate has no dependencies and uses only `core`, so userland test
//! programs can use it as well as the kernel.
//!
//! # Licence of the generated bindings
//!
//! Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
//! Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
//! Copyright (c) 2007 Dave Airlie, Copyright (c) 2007 Jakob Bornecrantz,
//! Copyright (c) 2008 Red Hat Inc., Copyright (c) 2007-2008 Tungsten
//! Graphics, Inc., Copyright (c) 2007-2008 Intel Corporation,
//! Copyright 2013 Red Hat. Copyright (c) 2006, 2015, Oracle and/or its
//! affiliates (illumos patch). All rights reserved.
//!
//! Permission is hereby granted, free of charge, to any person obtaining a
//! copy of this software and associated documentation files (the
//! "Software"), to deal in the Software without restriction, including
//! without limitation the rights to use, copy, modify, merge, publish,
//! distribute, sublicense, and/or sell copies of the Software, and to permit
//! persons to whom the Software is furnished to do so, subject to the
//! following conditions: The above copyright notice and this permission
//! notice shall be included in all copies or substantial portions of the
//! Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
//! EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//! MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//! IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//! CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT
//! OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
//! THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#![no_std]

mod abi;
#[allow(non_camel_case_types, non_upper_case_globals, non_snake_case, missing_docs, dead_code)]
#[allow(clippy::all)]
mod bindings;
pub mod ioc;
mod ioctl;
#[allow(non_camel_case_types)]
mod linux_newer;
#[allow(non_camel_case_types)]
mod virtgpu_compat;

pub use bindings::*;
pub use ioctl::*;
pub use linux_newer::*;
pub use virtgpu_compat::*;
