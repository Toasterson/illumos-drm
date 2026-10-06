//! Linux uAPI newer than the libdrm 2.4.109 headers the bindings are
//! generated from, written by hand from Linux v7.2's `drm.h` and
//! `drm_mode.h` (MIT, like the generated bindings): what the core
//! accepts for the Linux KMS code it carries (facade 2.0).
//!
//! * `DRM_IOCTL_MODE_CLOSEFB` (Linux 6.8): removes a framebuffer like
//!   `RMFB`, but leaves the planes that show it as they are.
//! * `DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP` (Linux 6.8): whether atomic commits
//!   may flip asynchronously.
//!
//! The assertions pin the layout; every field is naturally aligned.

use crate::bindings::__u32;
use crate::ioc::drm_iowr;

/// `struct drm_mode_closefb` (8 bytes): the argument of
/// [`DRM_IOCTL_MODE_CLOSEFB`].
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct drm_mode_closefb {
    /// The framebuffer's ID.
    pub fb_id: __u32,
    /// Must be zero.
    pub pad: __u32,
}

const _: () = assert!(core::mem::size_of::<drm_mode_closefb>() == 8);
const _: () = assert!(core::mem::offset_of!(drm_mode_closefb, pad) == 4);

/// `DRM_IOWR(0xD0, struct drm_mode_closefb)` from Linux's `drm.h`.
pub const DRM_IOCTL_MODE_CLOSEFB: u32 = drm_iowr::<drm_mode_closefb>(0xd0);

/// `DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP` from Linux's `drm.h`: 1 if the driver
/// supports `DRM_MODE_PAGE_FLIP_ASYNC` in atomic commits.
pub const DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP: u32 = 0x15;
