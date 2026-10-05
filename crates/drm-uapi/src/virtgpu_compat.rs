//! The older layouts of `struct drm_virtgpu_execbuffer` and of the two
//! transfer structures.
//!
//! These structures grew, and each size is a different command number,
//! because the ioctl encoding carries the argument size. Linux's `drm_ioctl`
//! accepts every size and zero-extends a shorter argument; the virtio-gpu
//! driver does the same by accepting every number. `EXECBUFFER`:
//!
//! | Layout | Size | Userland |
//! | --- | --- | --- |
//! | [`drm_virtgpu_execbuffer_v1`] | 32 bytes | libdrm 2.4.109 (the installed `virtgpu_drm.h`), Linux 5.15 and earlier |
//! | [`drm_virtgpu_execbuffer_v2`] | 40 bytes | Mesa 21.3's own copy (`include/drm-uapi/virtgpu_drm.h`), Linux 5.16 to 6.5: `ring_idx` |
//! | [`crate::drm_virtgpu_execbuffer`] | 64 bytes | Linux 6.6 and later (the header in `tools/uapi/include`): sync object arrays |
//!
//! `TRANSFER_TO_HOST` and `TRANSFER_FROM_HOST` gained `stride` and
//! `layer_stride` (44 bytes, Mesa 21.3 and Linux 5.x); libdrm 2.4.109 has
//! the 36-byte layouts [`drm_virtgpu_3d_transfer_to_host_v1`] and
//! [`drm_virtgpu_3d_transfer_from_host_v1`].
//!
//! These are written by hand from those headers (MIT, like the generated
//! bindings); every field is naturally aligned, and the assertions pin the
//! layouts. `tools/uapi/virtgpu_compat_layout.c` prints the same facts from
//! the original headers for `cargo xtask uapi-check`.

use crate::bindings::{__s32, __u32, __u64, drm_virtgpu_3d_box};
use crate::ioc::drm_iowr;

/// `struct drm_virtgpu_execbuffer` of libdrm 2.4.109 (32 bytes).
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct drm_virtgpu_execbuffer_v1 {
    /// `VIRTGPU_EXECBUF_*` flags.
    pub flags: __u32,
    /// The size of the command stream in bytes.
    pub size: __u32,
    /// The command stream (a user pointer).
    pub command: __u64,
    /// The array of buffer handles (a user pointer).
    pub bo_handles: __u64,
    /// The number of buffer handles.
    pub num_bo_handles: __u32,
    /// The in or out fence descriptor.
    pub fence_fd: __s32,
}

/// `struct drm_virtgpu_execbuffer` of Linux 5.16 to 6.5 and of Mesa 21.3
/// (40 bytes): `ring_idx` added.
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct drm_virtgpu_execbuffer_v2 {
    /// `VIRTGPU_EXECBUF_*` flags.
    pub flags: __u32,
    /// The size of the command stream in bytes.
    pub size: __u32,
    /// The command stream (a user pointer).
    pub command: __u64,
    /// The array of buffer handles (a user pointer).
    pub bo_handles: __u64,
    /// The number of buffer handles.
    pub num_bo_handles: __u32,
    /// The in or out fence descriptor.
    pub fence_fd: __s32,
    /// The ring of the context (with `VIRTGPU_EXECBUF_RING_IDX`).
    pub ring_idx: __u32,
    /// Padding.
    pub pad: __u32,
}

/// `DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_EXECBUFFER, struct drm_virtgpu_execbuffer)`
/// with the 32-byte layout.
pub const DRM_IOCTL_VIRTGPU_EXECBUFFER_V1: u32 = drm_iowr::<drm_virtgpu_execbuffer_v1>(0x42);
/// The same with the 40-byte layout.
pub const DRM_IOCTL_VIRTGPU_EXECBUFFER_V2: u32 = drm_iowr::<drm_virtgpu_execbuffer_v2>(0x42);

/// `struct drm_virtgpu_3d_transfer_to_host` of libdrm 2.4.109 (36 bytes).
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct drm_virtgpu_3d_transfer_to_host_v1 {
    /// The buffer handle.
    pub bo_handle: __u32,
    /// The box to transfer.
    pub box_: drm_virtgpu_3d_box,
    /// The mipmap level.
    pub level: __u32,
    /// The offset in the buffer.
    pub offset: __u32,
}

/// `struct drm_virtgpu_3d_transfer_from_host` of libdrm 2.4.109 (36 bytes).
#[repr(C)]
#[derive(Debug, Default, Copy, Clone)]
pub struct drm_virtgpu_3d_transfer_from_host_v1 {
    /// The buffer handle.
    pub bo_handle: __u32,
    /// The box to transfer.
    pub box_: drm_virtgpu_3d_box,
    /// The mipmap level.
    pub level: __u32,
    /// The offset in the buffer.
    pub offset: __u32,
}

/// `DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST` with the 36-byte layout.
pub const DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST_V1: u32 = drm_iowr::<drm_virtgpu_3d_transfer_from_host_v1>(0x46);
/// `DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST` with the 36-byte layout.
pub const DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST_V1: u32 = drm_iowr::<drm_virtgpu_3d_transfer_to_host_v1>(0x47);

const _: () = {
    use core::mem::{offset_of, size_of};
    assert!(size_of::<drm_virtgpu_execbuffer_v1>() == 32);
    assert!(offset_of!(drm_virtgpu_execbuffer_v1, command) == 8);
    assert!(offset_of!(drm_virtgpu_execbuffer_v1, bo_handles) == 16);
    assert!(offset_of!(drm_virtgpu_execbuffer_v1, num_bo_handles) == 24);
    assert!(offset_of!(drm_virtgpu_execbuffer_v1, fence_fd) == 28);
    assert!(size_of::<drm_virtgpu_execbuffer_v2>() == 40);
    assert!(offset_of!(drm_virtgpu_execbuffer_v2, ring_idx) == 32);
    assert!(offset_of!(drm_virtgpu_execbuffer_v2, pad) == 36);
    // Each is a prefix of the next.
    assert!(size_of::<crate::drm_virtgpu_execbuffer>() == 64);
    assert!(offset_of!(crate::drm_virtgpu_execbuffer, fence_fd) == 28);
    assert!(offset_of!(crate::drm_virtgpu_execbuffer, ring_idx) == 32);
    assert!(offset_of!(crate::drm_virtgpu_execbuffer, syncobj_stride) == 36);
    assert!(size_of::<drm_virtgpu_3d_transfer_to_host_v1>() == 36);
    assert!(size_of::<drm_virtgpu_3d_transfer_from_host_v1>() == 36);
    assert!(offset_of!(drm_virtgpu_3d_transfer_to_host_v1, offset) == 32);
    assert!(size_of::<crate::drm_virtgpu_3d_transfer_to_host>() == 44);
    assert!(offset_of!(crate::drm_virtgpu_3d_transfer_to_host, offset) == 32);
    assert!(size_of::<crate::drm_virtgpu_3d_transfer_from_host>() == 44);
    assert!(DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST_V1 != crate::DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST);
    assert!(DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST_V1 != crate::DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST);
    // Three different command numbers.
    assert!(DRM_IOCTL_VIRTGPU_EXECBUFFER_V1 != DRM_IOCTL_VIRTGPU_EXECBUFFER_V2);
    assert!(DRM_IOCTL_VIRTGPU_EXECBUFFER_V2 != crate::DRM_IOCTL_VIRTGPU_EXECBUFFER);
    assert!(DRM_IOCTL_VIRTGPU_EXECBUFFER_V1 != crate::DRM_IOCTL_VIRTGPU_EXECBUFFER);
};
