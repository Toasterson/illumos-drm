// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! What a device driver supplies to the core.

use core::ffi::CStr;
use core::fmt;

use ddi::{DevInfo, Errno};

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
/// The cursor size, `preferred_depth`, `prefer_shadow` and `fb_modifiers`
/// are reported as given.
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
    /// (`DRM_CAP_ADDFB2_MODIFIERS`); the planes' `IN_FORMATS` blobs list
    /// them.
    pub fb_modifiers: bool,
    /// Whether page flips may be asynchronous (`DRM_CAP_ASYNC_PAGE_FLIP`,
    /// Linux's `mode_config.async_page_flip`). Facade 2.0, for Linux-KMS
    /// devices; the core's own mode setting (R3) has none.
    pub async_page_flip: bool,
}

impl ModeConfig {
    /// The defaults: 64 by 64 cursors, no preferred depth, no shadow, no
    /// modifiers, no asynchronous flips.
    pub const DEFAULT: ModeConfig = ModeConfig {
        cursor_width: 0,
        cursor_height: 0,
        preferred_depth: 0,
        prefer_shadow: false,
        fb_modifiers: false,
        async_page_flip: false,
    };
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

/// The identity and limits of a driver: what `DRM_IOCTL_VERSION` and
/// `DRM_IOCTL_GET_CAP` report, its feature flags and its memory limits.
/// Read when a device attaches and by those ioctls; fixed for the device's
/// life.
#[derive(Clone, Copy, Debug)]
pub struct DriverInfo {
    /// The driver name, reported by `DRM_IOCTL_VERSION` and matched by
    /// libdrm's `drmOpen(name, NULL)`.
    pub name: &'static CStr,
    /// The driver date, `YYYYMMDD` by convention.
    pub date: &'static CStr,
    /// A one-line description.
    pub desc: &'static CStr,
    /// The driver interface major version.
    pub major: i32,
    /// The driver interface minor version.
    pub minor: i32,
    /// The driver patch level.
    pub patchlevel: i32,
    /// The driver's feature flags.
    pub features: Features,
    /// The mode-setting properties reported by `GET_CAP`.
    pub mode_config: ModeConfig,
    /// The largest GEM object a client may create; larger requests fail
    /// with `ENOMEM`.
    pub max_object_size: usize,
    /// The most memory all GEM objects of the device may hold together
    /// ([`GemObject::memory_bytes`](crate::gem::GemObject::memory_bytes));
    /// an object that would pass it is refused with `ENOMEM`.
    pub max_memory: usize,
}

/// What `MODE_CREATE_DUMB` returns: the new handle, the pitch and the
/// object's size.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DumbCreated {
    /// The handle in the calling file.
    pub handle: u32,
    /// Bytes per row.
    pub pitch: u32,
    /// The object's size in bytes.
    pub size: u64,
}

/// What a DRM device driver supplies: its identity and limits
/// ([`DriverInfo`]), its buffer-object type and dumb-buffer rules, its
/// private ioctls and the hooks the core calls.
///
/// Since milestone R5a the core is the kernel module `misc/drm_core`, and the
/// only implementation is the module's facade driver, which forwards every
/// hook to a driver's C table (`struct drm_driver` of `include/drm`): the
/// core is monomorphised once, inside the module. A driver module, in C or
/// in Rust, talks to the module through the facade's C functions only.
pub trait Driver: Send + Sync + Sized + 'static {
    /// The driver's identity and limits.
    fn info(&self) -> &DriverInfo;

    /// The driver's private ioctls, numbered from `DRM_COMMAND_BASE`
    /// (0x40) to below `DRM_COMMAND_END` (0xa0). A command matches the
    /// entry with its whole value first, otherwise the first entry with
    /// its number and direction ([`crate::ioctl::lookup_driver`]);
    /// [`Driver::ioctl`] handles them.
    fn ioctls(&self) -> &[DriverIoctl] {
        &[]
    }

    /// The driver's buffer objects.
    #[cfg(not(test))]
    type Object: crate::gem::GemObject;

    /// Says how GEM object memory of the device `dev` is bound for DMA,
    /// once, when the device attaches: against the device's own node (the
    /// `ddi::OwnNode` that [`Device::attach`](crate::Device::attach)
    /// received), which gives every object a scatter/gather table
    /// ([`SysMem::sg_iter`](crate::gem::SysMem::sg_iter)). `None`, the
    /// default, for a device that does no DMA; pseudo devices cannot have
    /// DMA handles.
    #[cfg(not(test))]
    fn gem_dma(&self, dev: &DevInfo) -> Option<crate::gem::GemDma> {
        let _ = dev;
        None
    }

    /// Writes the bus ID of the device. The default, for pseudo devices, is
    /// `pseudo:<driver>@<instance>`; PCI drivers write
    /// `pci:<domain>:<bus>:<device>.<function>` as libdrm expects.
    fn bus_id(&self, dev: &DevInfo, out: &mut BusId) -> fmt::Result {
        let name = dev.driver_name().to_str().map_err(|_| fmt::Error)?;
        fmt::Write::write_fmt(out, format_args!("pseudo:{}@{}", name, dev.instance().0))
    }

    /// Checks a dumb-buffer request and chooses its pitch and size, for
    /// [`Device::dumb_create_default`](crate::Device::dumb_create_default).
    /// The default accepts 8, 16, 24 and 32 bits per pixel and pads rows to
    /// 64 bytes ([`dumb::standard_layout`]).
    fn dumb_layout(&self, req: &DumbRequest) -> Result<DumbLayout, Errno> {
        dumb::standard_layout(req)
    }

    /// Makes the object of a new dumb buffer from its memory, for
    /// [`Device::dumb_create_default`](crate::Device::dumb_create_default).
    /// The default refuses (`ENOSYS`).
    #[cfg(not(test))]
    fn dumb_object(
        &self,
        dev: &crate::Device<Self>,
        mem: crate::gem::SysMem,
        req: &DumbRequest,
        layout: &DumbLayout,
        ctx: &ddi::Sleepable,
    ) -> Result<Self::Object, Errno> {
        let _ = (dev, mem, req, layout, ctx);
        Err(ENOSYS)
    }

    /// `MODE_CREATE_DUMB`, after the core has checked the caller's
    /// permission and that the driver has [`Features::DUMB`]. The default is
    /// [`Device::dumb_create_default`](crate::Device::dumb_create_default):
    /// [`Driver::dumb_layout`], the size limits, memory, and
    /// [`Driver::dumb_object`].
    #[cfg(not(test))]
    fn dumb_create(
        &self,
        dev: &crate::Device<Self>,
        file: crate::minor::CloneId,
        req: &DumbRequest,
        ctx: &ddi::Sleepable,
    ) -> Result<DumbCreated, Errno> {
        dev.dumb_create_default(file, req, ctx)
    }

    /// `MODE_MAP_DUMB`, after the core has checked the caller's permission:
    /// the fake offset of handle `handle` of `file`. The default is
    /// [`Device::dumb_map_offset_default`](crate::Device::dumb_map_offset_default).
    #[cfg(not(test))]
    fn dumb_map_offset(
        &self,
        dev: &crate::Device<Self>,
        file: crate::minor::CloneId,
        handle: u32,
        ctx: &ddi::Sleepable,
    ) -> Result<u64, Errno> {
        dev.dumb_map_offset_default(file, handle, ctx)
    }

    /// Handles one of [`Driver::ioctls`], after the core has checked its
    /// permission flags. `entry` is the matched table entry, `cmd` the
    /// caller's command value, `file` the calling file and `arg` the ioctl
    /// argument.
    #[cfg(not(test))]
    fn ioctl(
        &self,
        _dev: &crate::Device<Self>,
        _file: crate::minor::CloneId,
        _entry: &DriverIoctl,
        _cmd: u32,
        _arg: &ddi::IoctlArg<'_>,
        _ctx: &ddi::Sleepable,
    ) -> Result<(), Errno> {
        Err(Errno::ENOTTY)
    }

    /// The display side of the driver (milestone R3): for a driver with
    /// [`Features::MODESET`] and display hardware, its CRTCs, planes,
    /// encoders and connectors and the hooks that check and apply their
    /// state ([`crate::kms::KmsDriver`]). The core asks once, when the
    /// device attaches, and needs [`Features::GEM`] for framebuffers. The
    /// default, `None`, is a device without display (render-only), for which
    /// every mode-setting ioctl fails with `EOPNOTSUPP`.
    #[cfg(not(test))]
    fn kms(&self) -> Option<&dyn crate::kms::KmsDriver<crate::gem::GemRef<Self::Object>>> {
        None
    }

    /// Facade 2.0: the hooks of the driver's Linux KMS code
    /// ([`crate::lkms`]), for a device whose mode setting that code does.
    /// The core asks once, when the device attaches, and treats the device
    /// as a Linux-KMS device if the answer is `Some`: its mode-setting
    /// ioctls go to [`Driver::lkms_ioctl`], its files get event queues
    /// that `read(2)` and `chpoll(9E)` serve, and its opens and closes run
    /// the hooks. Never together with [`Driver::kms`]. The default, `None`,
    /// is every other device.
    #[cfg(not(test))]
    fn lkms(&self) -> Option<&dyn crate::lkms::LkmsHooks> {
        None
    }

    /// Facade 2.0: a mode-setting command of a Linux-KMS device, after the
    /// core's permission check: the KMS code's handler of command number
    /// `nr` with `data`, the kernel copy of the argument. `cmds` are the
    /// table's and the caller's command values and `arg` the ioctl, for
    /// the facade's copy functions during the handler. Returns 0 or a
    /// negative errno (`-ENOTTY`, the default, for an unknown command).
    #[cfg(not(test))]
    #[allow(clippy::too_many_arguments, reason = "the parts of an ioctl")]
    fn lkms_ioctl(
        &self,
        _dev: &crate::Device<Self>,
        _file: crate::minor::CloneId,
        _cmds: (u32, u32),
        _nr: u32,
        _data: &mut [u8],
        _arg: &ddi::IoctlArg<'_>,
        _ctx: &ddi::Sleepable,
    ) -> i32 {
        -Errno::ENOTTY.get()
    }

    /// Facade 2.0: the client capabilities and master status of open file
    /// `file` changed (or the file was just opened): the driver mirrors
    /// them into its own structure (`misc/drm_core`: the facade's
    /// `drm_file`, [`crate::state::FileFlags::store`]). Called with the
    /// device's state lock held: it must not block or call back into the
    /// core.
    #[cfg(not(test))]
    fn file_flags(&self, _file: crate::minor::CloneId, _flags: &crate::state::FileFlags) {}

    /// The highest interrupt priority at which the driver signals fences
    /// (a soft interrupt's, say), with which the core creates every lock
    /// that signalling takes (see [`crate::fence`]). `None`, the default,
    /// allows thread context and callouts (`timeout(9F)`) only. Called
    /// once, in [`Device::attach`](crate::Device::attach).
    #[cfg(not(test))]
    fn fence_priority(&self) -> Option<ddi::IntrPriority> {
        None
    }

    /// Called in `open(9E)` once the core has registered file `file`
    /// (Linux's `drm_driver.open`); an error fails the open, and the core
    /// then forgets the file without calling [`Driver::file_close`].
    #[cfg(not(test))]
    fn file_open(
        &self,
        _dev: &crate::Device<Self>,
        _file: crate::minor::CloneId,
        _ctx: &ddi::Sleepable,
    ) -> Result<(), Errno> {
        Ok(())
    }

    /// Called in `close(9E)` of file `file` after the core released the
    /// file's mode-setting state, GEM handles and sync objects, before its
    /// clone id is freed (Linux's `postclose`): a driver drops its
    /// per-file state here (virtio-gpu destroys the file's rendering
    /// context).
    #[cfg(not(test))]
    fn file_close(&self, _dev: &crate::Device<Self>, _file: crate::minor::CloneId, _ctx: &ddi::Sleepable) {}

    /// Makes an object of this device for `exporter`, an object of another
    /// device that a client imports with `PRIME_FD_TO_HANDLE` (Linux's
    /// `gem_prime_import`). The default refuses with `ENOTSUP`.
    #[cfg(not(test))]
    fn prime_import(
        &self,
        _dev: &crate::Device<Self>,
        _exporter: &crate::gem::GemRef<Self::Object>,
        _ctx: &ddi::Sleepable,
    ) -> Result<crate::gem::GemRef<Self::Object>, Errno> {
        Err(Errno::ENOTSUP)
    }

    /// Called in `detach(9E)` once no file is open, before the core checks
    /// whether the detach can proceed: the driver drops the references it
    /// holds on its own that would keep GEM objects alive (timers, work in
    /// flight). A check can still refuse the detach afterwards, and the
    /// device then stays attached and in use, so this must not stop
    /// anything the device needs to work (vblank sources, interrupts): that
    /// belongs in [`Driver::before_teardown`].
    #[cfg(not(test))]
    fn before_detach(&self, _ctx: &ddi::Sleepable) {}

    /// Called in `detach(9E)` once every check has passed, when the detach
    /// will complete: the driver stops what still reaches the core (vblank
    /// sources, interrupts) and drops its framebuffer references, before
    /// the core tears down the mode-setting and GEM state.
    #[cfg(not(test))]
    fn before_teardown(&self, _ctx: &ddi::Sleepable) {}
}

/// `ENOSYS`, which `ddi::Errno` has no named constant for.
pub const ENOSYS: Errno = match Errno::new(ddi::sys::errno::ENOSYS) {
    Some(e) => e,
    // Unreachable: ENOSYS is 89.
    None => Errno::EINVAL,
};

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
