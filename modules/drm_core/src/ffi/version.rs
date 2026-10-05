// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! `drm_facade_version.h`, and `drm_debug` of `drm_print.h`.

use core::mem::{offset_of, size_of};
use core::sync::atomic::AtomicU32;

use ddi::Errno;
use drm_facade_sys as f;

use crate::devcore::neg;

/// The enabled debug categories (`drm_print.h`).
#[unsafe(no_mangle)]
pub static drm_debug: AtomicU32 = AtomicU32::new(0);

/// The sizes of the facade structures, in the order of
/// `DRM_FACADE_STRUCTS` (`include/drm/drm_facade_version.h`).
const SIZES: [usize; f::DRM_FACADE_NSIZES as usize] = [
    size_of::<f::drm_device>(),
    size_of::<f::drm_minor>(),
    size_of::<f::drm_file>(),
    size_of::<f::drm_driver>(),
    size_of::<f::drm_illumos_mode_config>(),
    size_of::<f::drm_ioctl_desc>(),
    size_of::<f::drm_gem_object>(),
    size_of::<f::drm_gem_object_funcs>(),
    size_of::<f::drm_vma_offset_node>(),
    size_of::<f::dma_fence>(),
    size_of::<f::dma_fence_ops>(),
    size_of::<f::dma_fence_cb>(),
    size_of::<f::dma_resv>(),
    size_of::<f::dma_buf>(),
    size_of::<f::dma_buf_attachment>(),
    size_of::<f::drm_rkms_funcs>(),
    size_of::<f::drm_rkms_layout>(),
    size_of::<f::drm_rkms_plane_desc>(),
    size_of::<f::drm_rkms_crtc_desc>(),
    size_of::<f::drm_rkms_encoder_desc>(),
    size_of::<f::drm_rkms_connector_desc>(),
    size_of::<f::drm_rkms_mode_limits>(),
    size_of::<f::drm_rkms_mode>(),
    size_of::<f::drm_rkms_crtc_state>(),
    size_of::<f::drm_rkms_plane_state>(),
    size_of::<f::drm_rkms_connector_state>(),
    size_of::<f::drm_rkms_fb_info>(),
    size_of::<f::drm_rkms_rect>(),
    size_of::<f::drm_printer>(),
    // The private tails (DRM_FACADE_TAILS), each its structure's last field.
    size_of::<f::drm_device>() - offset_of!(f::drm_device, __core),
    size_of::<f::drm_file>() - offset_of!(f::drm_file, __core),
    size_of::<f::drm_gem_object>() - offset_of!(f::drm_gem_object, __core),
    size_of::<f::dma_fence>() - offset_of!(f::dma_fence, __core),
    size_of::<f::dma_resv>() - offset_of!(f::dma_resv, __core),
    size_of::<f::dma_buf>() - offset_of!(f::dma_buf, __core),
];

const _: () = assert!(SIZES.len() <= f::DRM_FACADE_MAX_SIZES as usize);

#[unsafe(no_mangle)]
pub unsafe extern "C" fn drm_facade_version(out: *mut f::drm_facade_version) -> i32 {
    if out.is_null() {
        return neg(Errno::EINVAL);
    }
    let mut v = f::drm_facade_version {
        major: f::DRM_FACADE_MAJOR,
        minor: f::DRM_FACADE_MINOR,
        nsizes: SIZES.len() as u32,
        reserved: 0,
        sizes: [0; f::DRM_FACADE_MAX_SIZES as usize],
    };
    for (d, s) in v.sizes.iter_mut().zip(SIZES.iter()) {
        *d = *s as u64;
    }
    // SAFETY: the caller's structure.
    unsafe { *out = v };
    0
}
