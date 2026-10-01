// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The facade's C functions, one module per header of `include/drm`. The
//! headers document every function; the comments here only say how each
//! maps onto the core. Every pointer a caller passes is checked against
//! the marks the facade put into the structures it set up (`__core`), so a
//! wrong or stale pointer is refused (`-EINVAL`, `NULL`) instead of being
//! followed, as far as such a check can tell.
//!
//! The context of a call: functions documented for thread context assume
//! it (their callers are ioctl handlers, attach and detach); the few that
//! a driver may call from an interrupt handler (`dma_fence_signal`,
//! `dma_fence_put`, `drm_gem_object_put`, ...) ask illumos-ddi which
//! context they run in.

pub(crate) mod device;
pub(crate) mod fence;
pub(crate) mod gem;
pub(crate) mod illumos;
pub(crate) mod prime;
pub(crate) mod resv;
pub(crate) mod rkms;
pub(crate) mod syncobj;
pub(crate) mod uaccess;
pub(crate) mod version;

use ddi::Sleepable;
use ddi::private::{Context, current_context};
use drm_core::defer::Ctx;

/// Runs `f` with the caller's context as the core's `Ctx`: atomic on an
/// interrupt thread, sleepable otherwise.
///
/// # Safety
/// The caller is an interrupt handler or a thread that may block: not a
/// `timeout(9F)` handler, `quiesce(9E)` or `chpoll(9E)`, not at raised
/// priority and not holding a spin lock (`ddi::private::current_context`).
/// The functions that use it say so in their headers and offer a variant
/// for those contexts (`drm_illumos_gem_object_put_deferred`).
pub(crate) unsafe fn with_ctx<R>(f: impl FnOnce(Ctx<'_>) -> R) -> R {
    // SAFETY: the caller's contract.
    match unsafe { current_context() } {
        Context::Sleep(s) => f(Ctx::Sleep(&s)),
        Context::Atomic(a) => f(Ctx::Atomic(&a)),
    }
}

/// The token of a function documented for thread context.
///
/// # Safety
/// The caller runs in user or kernel context, as the function's contract
/// in the header requires.
pub(crate) unsafe fn sleepable() -> Sleepable {
    // SAFETY: the caller's contract.
    unsafe { Sleepable::assume() }
}
