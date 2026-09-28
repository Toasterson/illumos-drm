// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Device capabilities (`DRM_IOCTL_GET_CAP`) and client capabilities
//! (`DRM_IOCTL_SET_CLIENT_CAP`).
//!
//! The rules follow Linux's `drm_getcap` and `drm_setclientcap` of the
//! libdrm 2.4.109 era, with one difference: a capability reports what this
//! core implements today, not what Linux always implements. PRIME, syncobj,
//! dumb buffers, vblank and page-flip events arrive in later milestones and
//! report 0 until then.

use ddi::Errno;
use drm_uapi as uapi;

use crate::driver::{Features, ModeConfig};

/// `EOPNOTSUPP`, which Linux returns for capabilities of a feature the
/// driver lacks. `ddi::Errno` has no named constant for it.
pub const EOPNOTSUPP: Errno = match Errno::new(ddi::sys::errno::EOPNOTSUPP) {
    Some(e) => e,
    // Unreachable: EOPNOTSUPP is 122. EINVAL keeps the constant total.
    None => Errno::EINVAL,
};

/// Answers `DRM_IOCTL_GET_CAP`. Unknown capabilities fail with `EINVAL`;
/// mode-setting capabilities of a driver without [`Features::MODESET`] fail
/// with `EOPNOTSUPP`.
pub fn get_cap(features: Features, mode: &ModeConfig, cap: u64) -> Result<u64, Errno> {
    let Ok(cap) = u32::try_from(cap) else { return Err(Errno::EINVAL) };
    // Capabilities that make sense for every driver, including render-only
    // ones.
    match cap {
        uapi::DRM_CAP_TIMESTAMP_MONOTONIC => return Ok(1),
        // PRIME import/export and sync objects are milestone R2.
        uapi::DRM_CAP_PRIME | uapi::DRM_CAP_SYNCOBJ | uapi::DRM_CAP_SYNCOBJ_TIMELINE => return Ok(0),
        _ => {}
    }
    let known = matches!(
        cap,
        uapi::DRM_CAP_DUMB_BUFFER
            | uapi::DRM_CAP_VBLANK_HIGH_CRTC
            | uapi::DRM_CAP_DUMB_PREFERRED_DEPTH
            | uapi::DRM_CAP_DUMB_PREFER_SHADOW
            | uapi::DRM_CAP_ASYNC_PAGE_FLIP
            | uapi::DRM_CAP_CURSOR_WIDTH
            | uapi::DRM_CAP_CURSOR_HEIGHT
            | uapi::DRM_CAP_ADDFB2_MODIFIERS
            | uapi::DRM_CAP_PAGE_FLIP_TARGET
            | uapi::DRM_CAP_CRTC_IN_VBLANK_EVENT
    );
    if !known {
        return Err(Errno::EINVAL);
    }
    if !features.contains(Features::MODESET) {
        return Err(EOPNOTSUPP);
    }
    Ok(match cap {
        uapi::DRM_CAP_CURSOR_WIDTH => nonzero_or(mode.cursor_width, 64),
        uapi::DRM_CAP_CURSOR_HEIGHT => nonzero_or(mode.cursor_height, 64),
        uapi::DRM_CAP_DUMB_PREFERRED_DEPTH => mode.preferred_depth,
        uapi::DRM_CAP_DUMB_PREFER_SHADOW => u64::from(mode.prefer_shadow),
        uapi::DRM_CAP_ADDFB2_MODIFIERS => u64::from(mode.fb_modifiers),
        // Dumb buffers are milestone R1; the vblank ioctl, page flips and
        // their events are R3. Until then the core has none of them.
        uapi::DRM_CAP_DUMB_BUFFER
        | uapi::DRM_CAP_VBLANK_HIGH_CRTC
        | uapi::DRM_CAP_ASYNC_PAGE_FLIP
        | uapi::DRM_CAP_PAGE_FLIP_TARGET
        | uapi::DRM_CAP_CRTC_IN_VBLANK_EVENT => 0,
        _ => return Err(Errno::EINVAL),
    })
}

fn nonzero_or(v: u64, default: u64) -> u64 {
    if v == 0 { default } else { v }
}

/// The client capabilities of one open file.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ClientCaps {
    /// `DRM_CLIENT_CAP_STEREO_3D`.
    pub stereo_3d: bool,
    /// `DRM_CLIENT_CAP_UNIVERSAL_PLANES`.
    pub universal_planes: bool,
    /// `DRM_CLIENT_CAP_ATOMIC`.
    pub atomic: bool,
    /// `DRM_CLIENT_CAP_ASPECT_RATIO`.
    pub aspect_ratio: bool,
    /// `DRM_CLIENT_CAP_WRITEBACK_CONNECTORS`.
    pub writeback_connectors: bool,
}

impl ClientCaps {
    /// Applies `DRM_IOCTL_SET_CLIENT_CAP`. Every client capability is a
    /// mode-setting one, so a driver without [`Features::MODESET`] refuses
    /// them all with `EOPNOTSUPP`. Values other than 0 and 1 and unknown
    /// capabilities fail with `EINVAL`. `ATOMIC` needs
    /// [`Features::ATOMIC`] and, as on Linux, sets `UNIVERSAL_PLANES` and
    /// `ASPECT_RATIO` to the same value; `WRITEBACK_CONNECTORS` needs
    /// `ATOMIC` to be set first. A failed call changes nothing.
    pub fn set(&mut self, features: Features, cap: u64, value: u64) -> Result<(), Errno> {
        if !features.contains(Features::MODESET) {
            return Err(EOPNOTSUPP);
        }
        let Ok(cap) = u32::try_from(cap) else { return Err(Errno::EINVAL) };
        let on = match value {
            0 => false,
            1 => true,
            _ => return Err(Errno::EINVAL),
        };
        match cap {
            uapi::DRM_CLIENT_CAP_STEREO_3D => self.stereo_3d = on,
            uapi::DRM_CLIENT_CAP_UNIVERSAL_PLANES => self.universal_planes = on,
            uapi::DRM_CLIENT_CAP_ATOMIC => {
                if !features.contains(Features::ATOMIC) {
                    return Err(EOPNOTSUPP);
                }
                self.atomic = on;
                self.universal_planes = on;
                self.aspect_ratio = on;
            }
            uapi::DRM_CLIENT_CAP_ASPECT_RATIO => self.aspect_ratio = on,
            uapi::DRM_CLIENT_CAP_WRITEBACK_CONNECTORS => {
                if !self.atomic {
                    return Err(Errno::EINVAL);
                }
                self.writeback_connectors = on;
            }
            _ => return Err(Errno::EINVAL),
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const KMS: Features = Features::MODESET.union(Features::ATOMIC);

    #[test]
    fn device_caps() {
        let m = ModeConfig::DEFAULT;
        assert_eq!(get_cap(Features::NONE, &m, uapi::DRM_CAP_TIMESTAMP_MONOTONIC as u64), Ok(1));
        assert_eq!(get_cap(Features::NONE, &m, uapi::DRM_CAP_PRIME as u64), Ok(0));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_SYNCOBJ as u64), Ok(0));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_SYNCOBJ_TIMELINE as u64), Ok(0));
        assert_eq!(get_cap(Features::NONE, &m, uapi::DRM_CAP_CURSOR_WIDTH as u64), Err(EOPNOTSUPP));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_CURSOR_WIDTH as u64), Ok(64));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_CURSOR_HEIGHT as u64), Ok(64));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_ADDFB2_MODIFIERS as u64), Ok(0));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_DUMB_BUFFER as u64), Ok(0));
        let big = ModeConfig { cursor_width: 256, cursor_height: 128, fb_modifiers: true, ..m };
        assert_eq!(get_cap(KMS, &big, uapi::DRM_CAP_CURSOR_WIDTH as u64), Ok(256));
        assert_eq!(get_cap(KMS, &big, uapi::DRM_CAP_CURSOR_HEIGHT as u64), Ok(128));
        assert_eq!(get_cap(KMS, &big, uapi::DRM_CAP_ADDFB2_MODIFIERS as u64), Ok(1));
        // Unknown capabilities, including ones that only differ in the high
        // bits, are EINVAL whatever the features.
        for f in [Features::NONE, KMS] {
            assert_eq!(get_cap(f, &m, 0), Err(Errno::EINVAL));
            assert_eq!(get_cap(f, &m, 0x15), Err(Errno::EINVAL));
            assert_eq!(get_cap(f, &m, (1 << 32) | uapi::DRM_CAP_TIMESTAMP_MONOTONIC as u64), Err(Errno::EINVAL));
        }
    }

    #[test]
    fn client_caps() {
        let mut c = ClientCaps::default();
        assert_eq!(c.set(Features::NONE, uapi::DRM_CLIENT_CAP_UNIVERSAL_PLANES as u64, 1), Err(EOPNOTSUPP));
        assert_eq!(c.set(Features::MODESET, uapi::DRM_CLIENT_CAP_ATOMIC as u64, 1), Err(EOPNOTSUPP));
        assert_eq!(c.set(KMS, uapi::DRM_CLIENT_CAP_WRITEBACK_CONNECTORS as u64, 1), Err(Errno::EINVAL));
        assert_eq!(c, ClientCaps::default());
        assert_eq!(c.set(KMS, uapi::DRM_CLIENT_CAP_ATOMIC as u64, 2), Err(Errno::EINVAL));
        assert_eq!(c.set(KMS, uapi::DRM_CLIENT_CAP_ATOMIC as u64, 1), Ok(()));
        assert!(c.atomic && c.universal_planes && c.aspect_ratio);
        assert_eq!(c.set(KMS, uapi::DRM_CLIENT_CAP_WRITEBACK_CONNECTORS as u64, 1), Ok(()));
        assert_eq!(c.set(KMS, uapi::DRM_CLIENT_CAP_STEREO_3D as u64, 1), Ok(()));
        assert_eq!(c.set(KMS, uapi::DRM_CLIENT_CAP_ASPECT_RATIO as u64, 0), Ok(()));
        assert!(c.writeback_connectors && c.stereo_3d && !c.aspect_ratio);
        assert_eq!(c.set(KMS, 6, 1), Err(Errno::EINVAL));
        assert_eq!(c.set(KMS, 0, 1), Err(Errno::EINVAL));
        assert_eq!(c.set(KMS, (1 << 32) | 3, 1), Err(Errno::EINVAL));
        assert_eq!(c.set(KMS, uapi::DRM_CLIENT_CAP_ATOMIC as u64, 0), Ok(()));
        assert!(!c.atomic && !c.universal_planes);
    }
}
