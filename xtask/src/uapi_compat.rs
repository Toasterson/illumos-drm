// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The Rust half of the layout check of the hand-written older virtio-gpu
//! layouts (`crates/drm-uapi/src/virtgpu_compat.rs`); the C half is
//! `tools/uapi/virtgpu_compat_layout.c`.

use core::mem::{offset_of, size_of};

use drm_uapi::*;

/// One line per fact, in the format of `virtgpu_compat_layout.c`.
pub fn lines() -> Vec<String> {
    let mut v = Vec::new();
    macro_rules! size {
        ($t:ident) => {
            v.push(format!("size {} {}", stringify!($t), size_of::<$t>()))
        };
    }
    macro_rules! off {
        ($t:ident, $f:ident, $c:literal) => {
            v.push(format!("off {} {} {}", stringify!($t), $c, offset_of!($t, $f)))
        };
    }
    macro_rules! ioctl {
        ($n:ident) => {
            v.push(format!("ioctl {} {:#x}", stringify!($n), $n))
        };
    }
    size!(drm_virtgpu_execbuffer_v1);
    off!(drm_virtgpu_execbuffer_v1, flags, "flags");
    off!(drm_virtgpu_execbuffer_v1, size, "size");
    off!(drm_virtgpu_execbuffer_v1, command, "command");
    off!(drm_virtgpu_execbuffer_v1, bo_handles, "bo_handles");
    off!(drm_virtgpu_execbuffer_v1, num_bo_handles, "num_bo_handles");
    off!(drm_virtgpu_execbuffer_v1, fence_fd, "fence_fd");
    ioctl!(DRM_IOCTL_VIRTGPU_EXECBUFFER_V1);
    size!(drm_virtgpu_3d_transfer_to_host_v1);
    off!(drm_virtgpu_3d_transfer_to_host_v1, bo_handle, "bo_handle");
    off!(drm_virtgpu_3d_transfer_to_host_v1, box_, "box_");
    off!(drm_virtgpu_3d_transfer_to_host_v1, level, "level");
    off!(drm_virtgpu_3d_transfer_to_host_v1, offset, "offset");
    ioctl!(DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST_V1);
    size!(drm_virtgpu_3d_transfer_from_host_v1);
    off!(drm_virtgpu_3d_transfer_from_host_v1, bo_handle, "bo_handle");
    off!(drm_virtgpu_3d_transfer_from_host_v1, box_, "box_");
    off!(drm_virtgpu_3d_transfer_from_host_v1, level, "level");
    off!(drm_virtgpu_3d_transfer_from_host_v1, offset, "offset");
    ioctl!(DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST_V1);
    size!(drm_virtgpu_execbuffer_v2);
    off!(drm_virtgpu_execbuffer_v2, flags, "flags");
    off!(drm_virtgpu_execbuffer_v2, size, "size");
    off!(drm_virtgpu_execbuffer_v2, command, "command");
    off!(drm_virtgpu_execbuffer_v2, bo_handles, "bo_handles");
    off!(drm_virtgpu_execbuffer_v2, num_bo_handles, "num_bo_handles");
    off!(drm_virtgpu_execbuffer_v2, fence_fd, "fence_fd");
    off!(drm_virtgpu_execbuffer_v2, ring_idx, "ring_idx");
    off!(drm_virtgpu_execbuffer_v2, pad, "pad");
    ioctl!(DRM_IOCTL_VIRTGPU_EXECBUFFER_V2);
    v
}
