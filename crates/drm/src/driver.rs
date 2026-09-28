// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! What a device driver supplies to the core.

use core::ffi::CStr;
use core::fmt;

use ddi::DevInfo;

/// Driver feature flags, the counterpart of Linux's `DRIVER_*` bits. Only
/// the features the core acts on exist; later milestones add GEM, PRIME and
/// syncobj together with the code that implements them.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Features(u32);

impl Features {
    /// No features: a render-only device without mode setting.
    pub const NONE: Features = Features(0);
    /// The driver does kernel mode setting (`DRIVER_MODESET`). Without it
    /// the mode-setting capabilities of `GET_CAP` and all client
    /// capabilities are refused with `EOPNOTSUPP`, as on Linux.
    pub const MODESET: Features = Features(1 << 0);
    /// The driver supports atomic mode setting (`DRIVER_ATOMIC`), which a
    /// client enables with the `ATOMIC` client capability.
    pub const ATOMIC: Features = Features(1 << 1);
    /// The device has a render node (`DRIVER_RENDER`).
    pub const RENDER: Features = Features(1 << 2);

    /// Combines two sets of features.
    pub const fn union(self, other: Features) -> Features {
        Features(self.0 | other.0)
    }
    /// Reports whether every feature in `other` is present.
    pub const fn contains(self, other: Features) -> bool {
        self.0 & other.0 == other.0
    }
}

/// Mode-setting properties that `GET_CAP` reports. Fields left at their
/// defaults report what the core itself provides.
///
/// Only the cursor size is reported in R0. `preferred_depth` and
/// `prefer_shadow` take effect when the core implements dumb buffers (R1),
/// and `fb_modifiers` when it implements `ADDFB2` (R3); until then `GET_CAP`
/// reports 0 for all three, because the ioctls they describe do not exist.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ModeConfig {
    /// A valid cursor width (`DRM_CAP_CURSOR_WIDTH`); 0 reports the Linux
    /// default of 64.
    pub cursor_width: u64,
    /// A valid cursor height (`DRM_CAP_CURSOR_HEIGHT`); 0 reports 64.
    pub cursor_height: u64,
    /// The preferred depth of dumb buffers (`DRM_CAP_DUMB_PREFERRED_DEPTH`),
    /// reported from R1 on.
    pub preferred_depth: u64,
    /// Whether clients should render to a shadow buffer rather than to a
    /// dumb buffer directly (`DRM_CAP_DUMB_PREFER_SHADOW`), reported from R1
    /// on.
    pub prefer_shadow: bool,
    /// Whether `ADDFB2` accepts format modifiers
    /// (`DRM_CAP_ADDFB2_MODIFIERS`), reported from R3 on.
    pub fb_modifiers: bool,
}

impl ModeConfig {
    /// The defaults: 64 by 64 cursors, no preferred depth, no shadow, no
    /// modifiers.
    pub const DEFAULT: ModeConfig =
        ModeConfig { cursor_width: 0, cursor_height: 0, preferred_depth: 0, prefer_shadow: false, fb_modifiers: false };
}

impl Default for ModeConfig {
    fn default() -> Self {
        Self::DEFAULT
    }
}

/// The largest bus ID, in bytes. PCI bus IDs (`pci:0000:00:02.0`) need 16.
pub const BUS_ID_MAX: usize = 64;

/// A bus ID, the string that `GET_UNIQUE` reports and `SET_UNIQUE`
/// accepts. It is written once at attach time through [`core::fmt::Write`];
/// a string that does not fit is an error, never truncated.
pub struct BusId {
    buf: [u8; BUS_ID_MAX],
    len: usize,
}

impl BusId {
    /// Creates an empty bus ID.
    pub const fn new() -> BusId {
        BusId { buf: [0; BUS_ID_MAX], len: 0 }
    }
    /// Returns the bytes of the bus ID, without a terminating NUL.
    pub fn as_bytes(&self) -> &[u8] {
        self.buf.get(..self.len).unwrap_or(&[])
    }
}

impl Default for BusId {
    fn default() -> Self {
        Self::new()
    }
}

impl fmt::Write for BusId {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let b = s.as_bytes();
        if b.contains(&0) {
            return Err(fmt::Error);
        }
        let end = self.len.checked_add(b.len()).ok_or(fmt::Error)?;
        let dst = self.buf.get_mut(self.len..end).ok_or(fmt::Error)?;
        dst.copy_from_slice(b);
        self.len = end;
        Ok(())
    }
}

/// The minimum a DRM device driver supplies: its identity for
/// `DRM_IOCTL_VERSION`, its feature flags, and the mode-setting properties
/// that `DRM_IOCTL_GET_CAP` reports.
///
/// A driver module implements this on its per-device state, creates a
/// [`crate::Device`] from it in `attach(9E)` and uses the device as the
/// instance of its `ddi::Driver`; see the `rdrmnull` example.
pub trait Driver: Send + Sync + Sized + 'static {
    /// The driver name, reported by `DRM_IOCTL_VERSION` and matched by
    /// libdrm's `drmOpen(name, NULL)`.
    const NAME: &'static CStr;
    /// The driver date, `YYYYMMDD` by convention.
    const DATE: &'static CStr;
    /// A one-line description.
    const DESC: &'static CStr;
    /// The driver interface major version.
    const MAJOR: i32;
    /// The driver interface minor version.
    const MINOR: i32;
    /// The driver patch level.
    const PATCHLEVEL: i32;
    /// The driver's feature flags.
    const FEATURES: Features;
    /// The mode-setting properties reported by `GET_CAP`.
    const MODE_CONFIG: ModeConfig = ModeConfig::DEFAULT;

    /// Writes the bus ID of the device. The default, for pseudo devices, is
    /// `pseudo:<driver>@<instance>`; PCI drivers write
    /// `pci:<domain>:<bus>:<device>.<function>` as libdrm expects.
    fn bus_id(&self, dev: &DevInfo, out: &mut BusId) -> fmt::Result {
        let name = dev.driver_name().to_str().map_err(|_| fmt::Error)?;
        fmt::Write::write_fmt(out, format_args!("pseudo:{}@{}", name, dev.instance().0))
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use core::fmt::Write;

    #[test]
    fn features() {
        let f = Features::MODESET.union(Features::RENDER);
        assert!(f.contains(Features::MODESET));
        assert!(!f.contains(Features::ATOMIC));
        assert!(f.contains(Features::NONE));
    }

    #[test]
    fn bus_id_refuses_overflow_and_nul() {
        let mut b = BusId::new();
        write!(b, "pci:{:04x}:{:02x}:{:02x}.{}", 0, 0, 2, 0).unwrap();
        assert_eq!(b.as_bytes(), b"pci:0000:00:02.0");
        let mut b = BusId::new();
        assert!(b.write_str(&"x".repeat(BUS_ID_MAX)).is_ok());
        assert!(b.write_str("y").is_err());
        assert_eq!(b.as_bytes().len(), BUS_ID_MAX);
        assert!(BusId::new().write_str("a\0b").is_err());
    }
}
