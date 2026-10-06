// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Device capabilities (`DRM_IOCTL_GET_CAP`) and client capabilities
//! (`DRM_IOCTL_SET_CLIENT_CAP`).
//!
//! The rules follow Linux's `drm_getcap` and `drm_setclientcap` of the
//! libdrm 2.4.109 era, with one difference: a capability reports what this
//! core implements today, not what Linux always implements. Dumb buffers
//! exist since R1, PRIME and sync objects since R2, `ADDFB2` modifiers and
//! vblank events since R3; targeted page flips report 0, and asynchronous
//! ones 0 unless a Linux-KMS device's mode configuration (facade 2.0) has
//! them (in atomic commits also only with atomic mode setting, as Linux's
//! `DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP`).

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

/// Answers `DRM_IOCTL_GET_CAP`. Apart from the capabilities every driver
/// answers, a driver without [`Features::MODESET`] refuses all with
/// `EOPNOTSUPP`, as on Linux; for a mode-setting driver, unknown
/// capabilities fail with `EINVAL`.
pub fn get_cap(features: Features, mode: &ModeConfig, cap: u64) -> Result<u64, Errno> {
    let Ok(cap) = u32::try_from(cap) else { return Err(Errno::EINVAL) };
    // Capabilities that make sense for every driver, including render-only
    // ones.
    match cap {
        uapi::DRM_CAP_TIMESTAMP_MONOTONIC => return Ok(1),
        // R2: PRIME for every GEM driver, as in current Linux; sync
        // objects and timelines per driver feature.
        uapi::DRM_CAP_PRIME => {
            let both = u64::from(uapi::DRM_PRIME_CAP_IMPORT | uapi::DRM_PRIME_CAP_EXPORT);
            return Ok(if features.contains(Features::GEM) { both } else { 0 });
        }
        uapi::DRM_CAP_SYNCOBJ => return Ok(u64::from(features.contains(Features::SYNCOBJ))),
        uapi::DRM_CAP_SYNCOBJ_TIMELINE => {
            return Ok(u64::from(features.contains(Features::SYNCOBJ.union(Features::SYNCOBJ_TIMELINE))));
        }
        _ => {}
    }
    // Everything else is about mode setting; Linux answers EOPNOTSUPP for
    // any other capability of a driver without it, known or not.
    if !features.contains(Features::MODESET) {
        return Err(EOPNOTSUPP);
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
            | uapi::DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP
    );
    if !known {
        return Err(Errno::EINVAL);
    }
    Ok(match cap {
        uapi::DRM_CAP_CURSOR_WIDTH => nonzero_or(mode.cursor_width, 64),
        uapi::DRM_CAP_CURSOR_HEIGHT => nonzero_or(mode.cursor_height, 64),
        // As Linux: 1 if the driver creates dumb buffers, and the driver's
        // preferences for them.
        uapi::DRM_CAP_DUMB_BUFFER => u64::from(features.contains(Features::GEM.union(Features::DUMB))),
        uapi::DRM_CAP_DUMB_PREFERRED_DEPTH => mode.preferred_depth,
        uapi::DRM_CAP_DUMB_PREFER_SHADOW => u64::from(mode.prefer_shadow),
        // Since R3: ADDFB2 takes modifiers when the driver says so (and
        // IN_FORMATS lists them), WAIT_VBLANK takes the high CRTC bits, and
        // vblank and flip events carry the CRTC ID, as on Linux. Async and
        // target flips are not implemented.
        uapi::DRM_CAP_ADDFB2_MODIFIERS => u64::from(mode.fb_modifiers),
        uapi::DRM_CAP_VBLANK_HIGH_CRTC | uapi::DRM_CAP_CRTC_IN_VBLANK_EVENT => 1,
        // Facade 2.0: a Linux-KMS device's mode configuration says whether
        // it flips asynchronously; R3 never does.
        uapi::DRM_CAP_ASYNC_PAGE_FLIP => u64::from(mode.async_page_flip),
        // As Linux: asynchronous flips in atomic commits need atomic mode
        // setting as well.
        uapi::DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP => u64::from(features.contains(Features::ATOMIC) && mode.async_page_flip),
        uapi::DRM_CAP_PAGE_FLIP_TARGET => 0,
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
    /// them all with `EOPNOTSUPP`. Unknown capabilities and values other
    /// than 0 and 1 fail with `EINVAL`, except that `ATOMIC` also accepts 2,
    /// the value Linux lets an Xorg server use to ask for atomic mode
    /// setting knowingly. `ATOMIC` needs [`Features::ATOMIC`] and, as on
    /// Linux, sets `UNIVERSAL_PLANES` and `ASPECT_RATIO` along with it;
    /// `WRITEBACK_CONNECTORS` needs `ATOMIC` to be set first. A failed call
    /// changes nothing.
    ///
    /// Linux also refuses `ATOMIC` = 1 from processes whose name starts
    /// with `X`, because the modesetting driver of Xorg misuses it. That
    /// check needs the process name, which no committed DDI interface
    /// provides; it belongs with atomic mode setting in milestone R3.
    pub fn set(&mut self, features: Features, cap: u64, value: u64) -> Result<(), Errno> {
        if !features.contains(Features::MODESET) {
            return Err(EOPNOTSUPP);
        }
        let Ok(cap) = u32::try_from(cap) else { return Err(Errno::EINVAL) };
        let max = if cap == uapi::DRM_CLIENT_CAP_ATOMIC { 2 } else { 1 };
        if value > max {
            return Err(Errno::EINVAL);
        }
        let on = value != 0;
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
        assert_eq!(get_cap(Features::GEM, &m, uapi::DRM_CAP_PRIME as u64), Ok(3), "import and export");
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_SYNCOBJ as u64), Ok(0));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_SYNCOBJ_TIMELINE as u64), Ok(0));
        let so = Features::SYNCOBJ;
        let tl = Features::SYNCOBJ.union(Features::SYNCOBJ_TIMELINE);
        assert_eq!(get_cap(so, &m, uapi::DRM_CAP_SYNCOBJ as u64), Ok(1));
        assert_eq!(get_cap(so, &m, uapi::DRM_CAP_SYNCOBJ_TIMELINE as u64), Ok(0));
        assert_eq!(get_cap(tl, &m, uapi::DRM_CAP_SYNCOBJ_TIMELINE as u64), Ok(1));
        assert_eq!(
            get_cap(Features::SYNCOBJ_TIMELINE, &m, uapi::DRM_CAP_SYNCOBJ_TIMELINE as u64),
            Ok(0),
            "needs SYNCOBJ"
        );
        assert_eq!(get_cap(Features::NONE, &m, uapi::DRM_CAP_CURSOR_WIDTH as u64), Err(EOPNOTSUPP));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_CURSOR_WIDTH as u64), Ok(64));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_CURSOR_HEIGHT as u64), Ok(64));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_ADDFB2_MODIFIERS as u64), Ok(0));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_DUMB_BUFFER as u64), Ok(0), "no dumb buffers without GEM|DUMB");
        let dumb = KMS.union(Features::GEM).union(Features::DUMB);
        assert_eq!(get_cap(dumb, &m, uapi::DRM_CAP_DUMB_BUFFER as u64), Ok(1));
        assert_eq!(get_cap(KMS.union(Features::DUMB), &m, uapi::DRM_CAP_DUMB_BUFFER as u64), Ok(0), "DUMB needs GEM");
        assert_eq!(get_cap(Features::GEM.union(Features::DUMB), &m, uapi::DRM_CAP_DUMB_BUFFER as u64), Err(EOPNOTSUPP));
        let big = ModeConfig {
            cursor_width: 256,
            cursor_height: 128,
            preferred_depth: 24,
            prefer_shadow: true,
            fb_modifiers: true,
            async_page_flip: true,
        };
        assert_eq!(get_cap(KMS, &big, uapi::DRM_CAP_CURSOR_WIDTH as u64), Ok(256));
        assert_eq!(get_cap(KMS, &big, uapi::DRM_CAP_CURSOR_HEIGHT as u64), Ok(128));
        // The driver's modifier support and the dumb-buffer preferences.
        assert_eq!(get_cap(KMS, &big, uapi::DRM_CAP_ADDFB2_MODIFIERS as u64), Ok(1));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_CRTC_IN_VBLANK_EVENT as u64), Ok(1));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_VBLANK_HIGH_CRTC as u64), Ok(1));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_ASYNC_PAGE_FLIP as u64), Ok(0));
        assert_eq!(get_cap(KMS, &big, uapi::DRM_CAP_ASYNC_PAGE_FLIP as u64), Ok(1));
        assert_eq!(get_cap(Features::GEM, &big, uapi::DRM_CAP_ASYNC_PAGE_FLIP as u64), Err(EOPNOTSUPP));
        // Asynchronous flips in atomic commits: the configuration's, and
        // only with atomic mode setting (Linux's drm_getcap()).
        let cap = uapi::DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP as u64;
        assert_eq!(get_cap(KMS, &big, cap), Ok(1));
        assert_eq!(get_cap(KMS, &m, cap), Ok(0));
        assert_eq!(get_cap(Features::MODESET, &big, cap), Ok(0), "needs ATOMIC");
        assert_eq!(get_cap(Features::ATOMIC, &big, cap), Err(EOPNOTSUPP), "needs MODESET");
        assert_eq!(get_cap(KMS, &big, uapi::DRM_CAP_PAGE_FLIP_TARGET as u64), Ok(0));
        assert_eq!(get_cap(KMS, &big, uapi::DRM_CAP_DUMB_PREFERRED_DEPTH as u64), Ok(24));
        assert_eq!(get_cap(KMS, &big, uapi::DRM_CAP_DUMB_PREFER_SHADOW as u64), Ok(1));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_DUMB_PREFERRED_DEPTH as u64), Ok(0));
        assert_eq!(get_cap(KMS, &m, uapi::DRM_CAP_DUMB_PREFER_SHADOW as u64), Ok(0));
        // Unknown capabilities, including ones that only differ in the high
        // bits, are EINVAL for a mode-setting driver and EOPNOTSUPP without.
        for (f, e) in [(Features::NONE, EOPNOTSUPP), (KMS, Errno::EINVAL)] {
            assert_eq!(get_cap(f, &m, 0), Err(e));
            assert_eq!(get_cap(f, &m, 0x16), Err(e));
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
        assert_eq!(c.set(KMS, uapi::DRM_CLIENT_CAP_ATOMIC as u64, 3), Err(Errno::EINVAL));
        assert_eq!(c.set(KMS, uapi::DRM_CLIENT_CAP_UNIVERSAL_PLANES as u64, 2), Err(Errno::EINVAL));
        assert_eq!(c, ClientCaps::default());
        assert_eq!(c.set(KMS, uapi::DRM_CLIENT_CAP_ATOMIC as u64, 2), Ok(()));
        assert!(c.atomic && c.universal_planes && c.aspect_ratio);
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
