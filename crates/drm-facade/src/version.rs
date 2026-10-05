// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The facade version check of a driver module: the Rust counterpart of
//! `drm_facade_check()` of `include/drm/drm_facade.h`.

use core::mem::size_of;

use ddi::Errno;
use drm_facade_sys as sys;

/// The number of structures whose sizes the facade version covers.
const N: usize = sys::DRM_FACADE_NSIZES as usize;

/// The size and name of every facade structure as this driver was compiled,
/// in the order of `drm_facade_version.sizes[]` (indexed by the
/// `DRM_FACADE_SZ_*` constants, so the order cannot drift).
const MINE: [(usize, &str); N] = {
    let mut a = [(0, ""); N];
    macro_rules! put {
        ($($s:ident => $i:ident),* $(,)?) => {
            $( a[sys::$i as usize] = (size_of::<sys::$s>(), stringify!($s)); )*
        };
    }
    put! {
        drm_device => DRM_FACADE_SZ_drm_device,
        drm_minor => DRM_FACADE_SZ_drm_minor,
        drm_file => DRM_FACADE_SZ_drm_file,
        drm_driver => DRM_FACADE_SZ_drm_driver,
        drm_illumos_mode_config => DRM_FACADE_SZ_drm_illumos_mode_config,
        drm_ioctl_desc => DRM_FACADE_SZ_drm_ioctl_desc,
        drm_gem_object => DRM_FACADE_SZ_drm_gem_object,
        drm_gem_object_funcs => DRM_FACADE_SZ_drm_gem_object_funcs,
        drm_vma_offset_node => DRM_FACADE_SZ_drm_vma_offset_node,
        dma_fence => DRM_FACADE_SZ_dma_fence,
        dma_fence_ops => DRM_FACADE_SZ_dma_fence_ops,
        dma_fence_cb => DRM_FACADE_SZ_dma_fence_cb,
        dma_resv => DRM_FACADE_SZ_dma_resv,
        dma_buf => DRM_FACADE_SZ_dma_buf,
        dma_buf_attachment => DRM_FACADE_SZ_dma_buf_attachment,
        drm_rkms_funcs => DRM_FACADE_SZ_drm_rkms_funcs,
        drm_rkms_layout => DRM_FACADE_SZ_drm_rkms_layout,
        drm_rkms_plane_desc => DRM_FACADE_SZ_drm_rkms_plane_desc,
        drm_rkms_crtc_desc => DRM_FACADE_SZ_drm_rkms_crtc_desc,
        drm_rkms_encoder_desc => DRM_FACADE_SZ_drm_rkms_encoder_desc,
        drm_rkms_connector_desc => DRM_FACADE_SZ_drm_rkms_connector_desc,
        drm_rkms_mode_limits => DRM_FACADE_SZ_drm_rkms_mode_limits,
        drm_rkms_mode => DRM_FACADE_SZ_drm_rkms_mode,
        drm_rkms_crtc_state => DRM_FACADE_SZ_drm_rkms_crtc_state,
        drm_rkms_plane_state => DRM_FACADE_SZ_drm_rkms_plane_state,
        drm_rkms_connector_state => DRM_FACADE_SZ_drm_rkms_connector_state,
        drm_rkms_fb_info => DRM_FACADE_SZ_drm_rkms_fb_info,
        drm_rkms_rect => DRM_FACADE_SZ_drm_rkms_rect,
        drm_printer => DRM_FACADE_SZ_drm_printer,
        drm_illumos_lkms_funcs => DRM_FACADE_SZ_drm_illumos_lkms_funcs,
    }
    // The private tails: the last field of each structure, so its size is
    // what follows its offset.
    macro_rules! tail {
        ($($s:ident => $i:ident),* $(,)?) => {
            $( a[sys::$i as usize] = (
                size_of::<sys::$s>() - core::mem::offset_of!(sys::$s, __core),
                concat!(stringify!($s), ".__core"),
            ); )*
        };
    }
    tail! {
        drm_device => DRM_FACADE_TAIL_drm_device,
        drm_file => DRM_FACADE_TAIL_drm_file,
        drm_gem_object => DRM_FACADE_TAIL_drm_gem_object,
        dma_fence => DRM_FACADE_TAIL_dma_fence,
        dma_resv => DRM_FACADE_TAIL_dma_resv,
        dma_buf => DRM_FACADE_TAIL_dma_buf,
    }
    // Every slot filled: a structure added to DRM_FACADE_STRUCTS must be
    // added here too.
    let mut i = 0;
    while i < N {
        assert!(a[i].0 != 0, "a facade structure is missing from MINE");
        i += 1;
    }
    a
};

/// Why a driver does not fit the loaded `misc/drm_core`.
#[derive(Debug, PartialEq, Eq)]
enum Mismatch {
    Version { major: u32, minor: u32 },
    Count(u32),
    Size { name: &'static str, core: u64, mine: usize },
}

// The minor version may be 0, which makes `v.minor < MINOR` always false.
#[allow(clippy::absurd_extreme_comparisons)]
fn compare(v: &sys::drm_facade_version) -> Result<(), Mismatch> {
    if v.major != sys::DRM_FACADE_MAJOR || v.minor < sys::DRM_FACADE_MINOR {
        return Err(Mismatch::Version { major: v.major, minor: v.minor });
    }
    if (v.nsizes as usize) < N {
        return Err(Mismatch::Count(v.nsizes));
    }
    for (i, &(mine, name)) in MINE.iter().enumerate() {
        if v.sizes[i] != mine as u64 {
            return Err(Mismatch::Size { name, core: v.sizes[i], mine });
        }
    }
    Ok(())
}

/// Compares the facade this driver was compiled against with the loaded
/// `misc/drm_core`: the same major version, a core minor version at least the
/// driver's, and the same size of every facade structure. Call it from
/// `ddi::Driver::load`, so that a mismatched driver refuses to load
/// (`EINVAL`, after a warning naming `driver` and the first difference)
/// before anything attaches. `drm_dev_alloc()` checks the version again.
pub fn check_facade(driver: &str) -> Result<(), Errno> {
    let mut v = sys::drm_facade_version { major: 0, minor: 0, nsizes: 0, reserved: 0, sizes: [0; 64] };
    // SAFETY: `v` is a valid output structure.
    if unsafe { sys::drm_facade_version(&mut v) } != 0 {
        ddi::kwarn!("{driver}: no DRM facade version");
        return Err(Errno::EINVAL);
    }
    compare(&v).map_err(|m| {
        match m {
            Mismatch::Version { major, minor } => ddi::kwarn!(
                "{driver}: built for DRM facade {}.{}, misc/drm_core provides {major}.{minor}",
                sys::DRM_FACADE_MAJOR,
                sys::DRM_FACADE_MINOR
            ),
            Mismatch::Count(n) => ddi::kwarn!("{driver}: misc/drm_core knows {n} facade structures, the driver {N}"),
            Mismatch::Size { name, core, mine } => {
                ddi::kwarn!("{driver}: {name} is {core} bytes in misc/drm_core, {mine} in the driver")
            }
        }
        Errno::EINVAL
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn core_like() -> sys::drm_facade_version {
        let mut v = sys::drm_facade_version {
            major: sys::DRM_FACADE_MAJOR,
            minor: sys::DRM_FACADE_MINOR,
            nsizes: N as u32,
            reserved: 0,
            sizes: [0; 64],
        };
        for (i, (s, _)) in MINE.iter().enumerate() {
            v.sizes[i] = *s as u64;
        }
        v
    }

    #[test]
    fn same_facade_fits() {
        assert_eq!(compare(&core_like()), Ok(()));
        let mut later = core_like();
        later.minor += 1;
        later.nsizes += 1;
        assert_eq!(compare(&later), Ok(()));
    }

    #[test]
    fn mismatches_refuse() {
        let mut v = core_like();
        v.major += 1;
        assert!(matches!(compare(&v), Err(Mismatch::Version { .. })));
        if sys::DRM_FACADE_MINOR > 0 {
            let mut v = core_like();
            v.minor -= 1;
            assert!(matches!(compare(&v), Err(Mismatch::Version { .. })));
        }
        let mut v = core_like();
        v.nsizes -= 1;
        assert_eq!(compare(&v), Err(Mismatch::Count(N as u32 - 1)));
        let mut v = core_like();
        v.sizes[sys::DRM_FACADE_SZ_dma_fence as usize] += 8;
        assert!(matches!(compare(&v), Err(Mismatch::Size { name: "dma_fence", .. })));
        let mut v = core_like();
        v.sizes[sys::DRM_FACADE_TAIL_drm_gem_object as usize] -= 8;
        assert!(matches!(compare(&v), Err(Mismatch::Size { name: "drm_gem_object.__core", .. })));
    }
}
