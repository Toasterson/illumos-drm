// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! What a device driver supplies to the core.

use core::ffi::CStr;
use core::fmt;

use ddi::{DevInfo, DmaAttr, Errno};

use crate::dumb::{self, DumbLayout, DumbRequest};
use crate::ioctl::DriverIoctl;

/// Driver feature flags, the counterpart of Linux's `DRIVER_*` bits. Only
/// the features the core acts on exist. PRIME has no flag: as in current
/// Linux, every driver with [`Features::GEM`] exports and imports dma-bufs.
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
    /// The driver has GEM buffer objects (`DRIVER_GEM`): handles,
    /// `GEM_CLOSE`, `GEM_FLINK`, `GEM_OPEN` and mmap of objects. Without it
    /// those ioctls fail with `EOPNOTSUPP`, as on Linux.
    pub const GEM: Features = Features(1 << 3);
    /// The driver supports dumb buffers (Linux: a `dumb_create` callback),
    /// which needs [`Features::GEM`]. Without it the dumb-buffer ioctls
    /// fail with `ENOSYS` and `DRM_CAP_DUMB_BUFFER` reports 0, as on Linux.
    pub const DUMB: Features = Features(1 << 4);

    /// The driver supports sync objects (`DRIVER_SYNCOBJ`): the
    /// `SYNCOBJ_*` ioctls and `DRM_CAP_SYNCOBJ`. Without it they fail with
    /// `EOPNOTSUPP`, as on Linux.
    pub const SYNCOBJ: Features = Features(1 << 5);
    /// The driver supports timeline sync objects
    /// (`DRIVER_SYNCOBJ_TIMELINE`): `SYNCOBJ_TIMELINE_WAIT`, `QUERY`,
    /// `TRANSFER`, `TIMELINE_SIGNAL` and `DRM_CAP_SYNCOBJ_TIMELINE`; needs
    /// [`Features::SYNCOBJ`].
    pub const SYNCOBJ_TIMELINE: Features = Features(1 << 6);

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
/// The cursor size, `preferred_depth` and `prefer_shadow` are reported as
/// given. `fb_modifiers` takes effect when the core implements `ADDFB2`
/// (R3); until then `GET_CAP` reports 0 for it, because the ioctl it
/// describes does not exist.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ModeConfig {
    /// A valid cursor width (`DRM_CAP_CURSOR_WIDTH`); 0 reports the Linux
    /// default of 64.
    pub cursor_width: u64,
    /// A valid cursor height (`DRM_CAP_CURSOR_HEIGHT`); 0 reports 64.
    pub cursor_height: u64,
    /// The preferred depth of dumb buffers (`DRM_CAP_DUMB_PREFERRED_DEPTH`).
    pub preferred_depth: u64,
    /// Whether clients should render to a shadow buffer rather than to a
    /// dumb buffer directly (`DRM_CAP_DUMB_PREFER_SHADOW`).
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

/// The default limit on the size of one GEM object: 256 MiB, an 8192 by
/// 8192 buffer of 32-bit pixels. Object memory is locked kernel memory, so
/// a client must not be able to ask for arbitrary amounts of it.
pub const DEFAULT_MAX_OBJECT_SIZE: usize = 256 << 20;

/// The default limit on the memory of all GEM objects of one device: 1 GiB.
/// Linux backs GEM objects with pageable shmem; here they are locked
/// kernel memory, which a client with access to a DRM node must not be
/// able to exhaust.
pub const DEFAULT_MAX_MEMORY: usize = 1 << 30;

/// What a DRM device driver supplies: its identity for
/// `DRM_IOCTL_VERSION`, its feature flags, the mode-setting properties that
/// `DRM_IOCTL_GET_CAP` reports, its buffer-object type and dumb-buffer
/// rules, and its private ioctls.
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
    /// The DMA attributes GEM object memory is bound with, against the
    /// device's own node (the `ddi::OwnNode` that
    /// [`Device::attach`](crate::Device::attach) received), which gives
    /// every object a scatter/gather table
    /// ([`SysMem::sg_iter`](crate::gem::SysMem::sg_iter)). `None` for a
    /// device that does no DMA; pseudo devices cannot have DMA handles.
    /// [`Driver::gem_dma`] can choose at attach time instead.
    const DMA_ATTR: Option<DmaAttr> = None;
    /// The largest GEM object a client may create; larger requests fail
    /// with `ENOMEM`.
    const MAX_OBJECT_SIZE: usize = DEFAULT_MAX_OBJECT_SIZE;
    /// The most memory all GEM objects of the device may hold together
    /// ([`GemObject::memory_bytes`](crate::gem::GemObject::memory_bytes));
    /// an object that would pass it is refused with `ENOMEM`.
    const MAX_MEMORY: usize = DEFAULT_MAX_MEMORY;
    /// The driver's private ioctls, numbered from `DRM_COMMAND_BASE`
    /// (0x40) to below `DRM_COMMAND_END` (0xa0) and matched on the whole
    /// command value; [`Driver::ioctl`] handles them.
    const IOCTLS: &'static [DriverIoctl] = &[];

    /// The driver's buffer objects. [`crate::gem::SysMem`] for a driver
    /// whose objects are plain system memory. Dumb buffers are system
    /// memory converted with `From`.
    #[cfg(not(test))]
    type Object: crate::gem::GemObject + From<crate::gem::SysMem>;

    /// Says how GEM object memory of the device `dev` is bound for DMA,
    /// once, when the device attaches. The default is
    /// [`DMA_ATTR`](Driver::DMA_ATTR) against the device's own node; a
    /// driver whose
    /// attributes depend on the hardware or on its `.conf` file returns them
    /// here.
    #[cfg(not(test))]
    fn gem_dma(&self, dev: &DevInfo) -> Option<crate::gem::GemDma> {
        let _ = dev;
        Self::DMA_ATTR.map(crate::gem::GemDma::new)
    }

    /// Writes the bus ID of the device. The default, for pseudo devices, is
    /// `pseudo:<driver>@<instance>`; PCI drivers write
    /// `pci:<domain>:<bus>:<device>.<function>` as libdrm expects.
    fn bus_id(&self, dev: &DevInfo, out: &mut BusId) -> fmt::Result {
        let name = dev.driver_name().to_str().map_err(|_| fmt::Error)?;
        fmt::Write::write_fmt(out, format_args!("pseudo:{}@{}", name, dev.instance().0))
    }

    /// Checks a dumb-buffer request and chooses its pitch and size. The
    /// default accepts 8, 16, 24 and 32 bits per pixel and pads rows to 64
    /// bytes ([`dumb::standard_layout`]); a driver with other scanout
    /// constraints replaces it, typically with [`dumb::layout`].
    fn dumb_layout(&self, req: &DumbRequest) -> Result<DumbLayout, Errno> {
        dumb::standard_layout(req)
    }

    /// Handles one of [`Driver::IOCTLS`], after the core has checked its
    /// permission flags. `file` is the calling file; `arg` is the ioctl
    /// argument, copied with [`crate::uaccess`].
    #[cfg(not(test))]
    fn ioctl(
        &self,
        _dev: &crate::Device<Self>,
        _file: crate::minor::CloneId,
        _cmd: u32,
        _arg: &ddi::IoctlArg<'_>,
        _ctx: &ddi::Sleepable,
    ) -> Result<(), Errno> {
        Err(Errno::ENOTTY)
    }

    /// The highest interrupt priority at which the driver signals fences
    /// (a soft interrupt's, say), with which the core creates every lock
    /// that signalling takes (see [`crate::fence`]). `None`, the default,
    /// allows thread context and callouts (`timeout(9F)`) only. Called
    /// once, in [`Device::attach`](crate::Device::attach).
    #[cfg(not(test))]
    fn fence_priority(&self) -> Option<ddi::IntrPriority> {
        None
    }

    /// Called in `detach(9E)` once no file is open, before the core checks
    /// that no GEM object is left: the driver releases whatever references
    /// it holds on its own (timers, work in flight). The detach can still
    /// fail afterwards.
    #[cfg(not(test))]
    fn before_detach(&self, _ctx: &ddi::Sleepable) {}
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use core::fmt::Write;

    #[test]
    fn features() {
        let f = Features::MODESET.union(Features::RENDER).union(Features::GEM);
        assert!(f.contains(Features::MODESET));
        assert!(f.contains(Features::GEM));
        assert!(!f.contains(Features::ATOMIC));
        assert!(!f.contains(Features::DUMB));
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
