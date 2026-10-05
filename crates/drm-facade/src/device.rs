// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The driver, its devices and their files.

use core::ffi::{CStr, c_int};
use core::ptr::{self, NonNull};

use ddi::module::StaticCell;
use ddi::sys as dsys;
use ddi::{DdiError, DmaAttr, Errno, Instance, IntrPriority, Minor, OwnNode, Sleepable};
use drm_core::dumb::DumbRequest;
use drm_facade_sys as sys;

use crate::ioctl::{self, Ioctl};
use crate::kms::{self, KmsDriver};
use crate::{check, check_ptr, neg};

/// What a Rust DRM driver supplies: its identity, features, limits,
/// private ioctls and hooks. The facade's C table (`struct drm_driver`)
/// is built from these at compile time.
pub trait Driver: Send + Sync + Sized + 'static {
    /// The name `DRM_IOCTL_VERSION` reports and `drmOpen(name)` matches.
    const NAME: &'static CStr;
    /// The date, `YYYYMMDD`.
    const DATE: &'static CStr;
    /// A one-line description.
    const DESC: &'static CStr;
    /// The interface major version.
    const MAJOR: i32;
    /// The interface minor version.
    const MINOR: i32;
    /// The patch level.
    const PATCHLEVEL: i32;
    /// The `DRIVER_*` feature flags (`drm_facade_sys::DRIVER_GEM`, ...).
    const FEATURES: u32;
    /// Whether the driver has dumb buffers ([`Driver::dumb_create`]).
    const DUMB: bool = false;
    /// Whether [`Driver::kms`] returns a display (mode setting with the
    /// core's R3 implementation).
    const KMS: bool = false;
    /// The largest GEM object, in bytes; 0 for the core's default.
    const MAX_OBJECT_SIZE: u64 = 0;
    /// The most object memory of one device; 0 for the core's default.
    const MAX_MEMORY: u64 = 0;
    /// The driver's private ioctls, at most [`ioctl::MAX_IOCTLS`].
    const IOCTLS: &'static [Ioctl<Self>] = &[];

    /// A file was opened (Linux's `open`); an error fails the open.
    fn file_open(&self, dev: &Dev<Self>, file: &File) -> Result<(), Errno> {
        let _ = (dev, file);
        Ok(())
    }

    /// A file was closed, after the core released its state (`postclose`).
    fn file_close(&self, dev: &Dev<Self>, file: &File) {
        let _ = (dev, file);
    }

    /// `MODE_CREATE_DUMB` (with [`Driver::DUMB`]).
    fn dumb_create(&self, dev: &Dev<Self>, file: &File, req: &DumbRequest) -> Result<DumbCreated, Errno> {
        let _ = (dev, file, req);
        Err(crate::ENOSYS)
    }

    /// The display, with [`Driver::KMS`].
    fn kms(&self) -> Option<&dyn KmsDriver> {
        None
    }

    /// A detach that may still be refused: drop what the driver holds on
    /// its own, signal the fences it owns.
    fn before_detach(&self, dev: &Dev<Self>) {
        let _ = dev;
    }

    /// The detach will complete: stop vblank and fence sources.
    fn before_teardown(&self, dev: &Dev<Self>) {
        let _ = dev;
    }
}

/// What `MODE_CREATE_DUMB` returns.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DumbCreated {
    /// The handle in the calling file.
    pub handle: u32,
    /// Bytes per row.
    pub pitch: u32,
    /// The object's size in bytes.
    pub size: u64,
}

/// The settings of a device before it registers.
#[derive(Default)]
pub struct Settings<'a> {
    /// GEM memory is bound for DMA with these attributes.
    pub dma: Option<DmaAttr>,
    /// The highest interrupt priority that signals the device's fences.
    pub fence_priority: Option<IntrPriority>,
    /// What `GET_CAP` reports about mode setting.
    pub mode_config: Option<sys::drm_illumos_mode_config>,
    /// The bus ID (`pci:0000:00:02.0`); the default is
    /// `pseudo:<driver>@<instance>`.
    pub unique: Option<&'a CStr>,
}

/// The structure the facade allocates for a device: the C device and the
/// driver's state after it.
#[repr(C)]
pub struct Dev<D: Driver> {
    /// Changed by the core through raw pointers while references exist.
    ddev: core::cell::UnsafeCell<sys::drm_device>,
    driver: D,
}

// SAFETY: the C device is changed only by the core under its rules; the
// driver state is Send and Sync.
unsafe impl<D: Driver> Send for Dev<D> {}
// SAFETY: as above.
unsafe impl<D: Driver> Sync for Dev<D> {}

impl<D: Driver> Dev<D> {
    /// The driver's state.
    pub fn driver(&self) -> &D {
        &self.driver
    }

    /// The C device.
    pub fn raw(&self) -> *mut sys::drm_device {
        self.ddev.get()
    }

    /// The device's node.
    pub fn dev_info(&self) -> ddi::DevInfo {
        // SAFETY: the node of the driver's attach, live while the device
        // exists.
        unsafe { ddi::DevInfo::from_raw((*self.raw()).dev as dsys::dev_info_t) }
    }

    /// A handle for the driver's timers and interrupt handlers.
    pub fn handle(&self) -> DevHandle {
        DevHandle(self.raw())
    }

    /// The device of a C device the facade passes to a callback.
    ///
    /// # Safety
    /// `ddev` is a device of this driver type, allocated by
    /// [`Device::register`].
    pub(crate) unsafe fn from_raw<'a>(ddev: *mut sys::drm_device) -> &'a Dev<D> {
        // SAFETY: the C device is the first field of a Dev<D>.
        unsafe { &*(ddev as *const Dev<D>) }
    }
}

/// A device for the driver's timers and interrupt handlers: valid from
/// [`Device::register`] until [`Driver::before_teardown`] returns, which
/// is when the driver must stop them.
#[derive(Clone, Copy, Debug)]
pub struct DevHandle(*mut sys::drm_device);

// SAFETY: a plain address; the facade's functions it is used with are
// thread-safe.
unsafe impl Send for DevHandle {}
// SAFETY: as above.
unsafe impl Sync for DevHandle {}

impl DevHandle {
    /// The C device.
    pub fn raw(&self) -> *mut sys::drm_device {
        self.0
    }

    /// Reports a vblank of `crtc` at `time_ns` (`gethrtime` time); thread
    /// context.
    pub fn vblank(&self, crtc: usize, time_ns: i64, _ctx: &Sleepable) {
        // SAFETY: a registered device (see the type's contract).
        unsafe { sys::drm_rkms_vblank(self.0, crtc as u32, time_ns) };
    }
}

/// A registered device: the instance of the driver's `ddi::Driver`.
pub struct Device<D: Driver> {
    dev: NonNull<Dev<D>>,
}

// SAFETY: Dev is Send and Sync.
unsafe impl<D: Driver> Send for Device<D> {}
// SAFETY: as above.
unsafe impl<D: Driver> Sync for Device<D> {}

impl<D: Driver> core::ops::Deref for Device<D> {
    type Target = Dev<D>;
    fn deref(&self) -> &Dev<D> {
        // SAFETY: allocated by register, freed only by unregister.
        unsafe { self.dev.as_ref() }
    }
}

impl<D: Driver> Device<D> {
    /// Allocates and registers a device for the driver's own node, with
    /// `driver` as its state: the facade's `__drm_dev_alloc`, the settings
    /// and `drm_dev_register`. On failure the state is dropped and nothing
    /// stays registered.
    pub fn register(node: &OwnNode, driver: D, s: &Settings<'_>, _ctx: &Sleepable) -> Result<Device<D>, DdiError> {
        let dip = node.as_raw() as *mut sys::dev_info_t;
        let table: &'static sys::drm_driver = &<D as Tables>::TABLE;
        // SAFETY: the driver's own node during its attach, and its static
        // table; the size and offset are those of Dev<D>.
        let p = unsafe { sys::__drm_dev_alloc(dip, table, size_of::<Dev<D>>(), 0) };
        let Ok(p) = check_ptr(p) else {
            node.warn(format_args!("cannot allocate the DRM device"));
            return Err(DdiError);
        };
        let dev = p.cast::<Dev<D>>();
        // SAFETY: the zeroed driver part of the new structure.
        unsafe { ptr::addr_of_mut!((*dev.as_ptr()).driver).write(driver) };
        let me = Device { dev };
        let raw = me.raw();
        let r = (|| -> Result<(), Errno> {
            // SAFETY: an unregistered device of this driver.
            unsafe {
                if let Some(a) = &s.dma {
                    check(sys::drm_dev_illumos_set_dma_attr(
                        raw,
                        a.as_raw() as *const _ as *const sys::ddi_dma_attr_t,
                    ))?;
                }
                if let Some(p) = s.fence_priority {
                    check(sys::drm_dev_illumos_set_fence_priority(raw, p.get()))?;
                }
                if let Some(m) = &s.mode_config {
                    check(sys::drm_dev_illumos_set_mode_config(raw, m))?;
                }
                if let Some(u) = s.unique {
                    check(sys::drm_dev_set_unique(raw, u.as_ptr()))?;
                }
                check(sys::drm_dev_register(raw, 0))
            }
        })();
        if let Err(e) = r {
            node.warn(format_args!("cannot register the DRM device: {e:?}"));
            me.free();
            return Err(DdiError);
        }
        Ok(me)
    }

    /// Unregisters the device (`drm_dev_unregister`): refused, and the
    /// device handed back working, while files are open, objects remain or
    /// fences are alive.
    #[allow(clippy::result_large_err, reason = "the shape of ddi::Driver::detach")]
    pub fn unregister(self, _ctx: &Sleepable) -> Result<(), (Device<D>, DdiError)> {
        // SAFETY: the registered device.
        let r = unsafe { sys::drm_dev_unregister(self.raw()) };
        if r != 0 {
            return Err((self, DdiError));
        }
        self.free();
        Ok(())
    }

    /// Drops the driver state and the device's reference.
    fn free(self) {
        let raw = self.raw();
        // SAFETY: the driver part was written by register; nothing uses it
        // any more (the device is not registered).
        unsafe {
            ptr::drop_in_place(ptr::addr_of_mut!((*self.dev.as_ptr()).driver));
            sys::drm_dev_put(raw);
        }
    }
}

/// An open file of a device, as the facade passes it to a hook.
pub struct File {
    p: NonNull<sys::drm_file>,
}

/// A number that names an open file, unique among the files of the system
/// since `misc/drm_core` loaded (`drm_file.client_id`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct FileId(pub u64);

impl File {
    /// # Safety
    /// `p` is a live file the facade passed to the caller's callback.
    pub(crate) unsafe fn from_raw(p: *mut sys::drm_file) -> Option<File> {
        NonNull::new(p).map(|p| File { p })
    }

    /// The C file.
    pub fn raw(&self) -> *mut sys::drm_file {
        self.p.as_ptr()
    }

    /// The file's number.
    pub fn id(&self) -> FileId {
        // SAFETY: a live file (see from_raw).
        FileId(unsafe { (*self.p.as_ptr()).client_id })
    }

    /// The opening process.
    pub fn pid(&self) -> i32 {
        // SAFETY: as above.
        unsafe { (*self.p.as_ptr()).pid }
    }
}

/// The instance of the driver that owns a DRM minor, for
/// `ddi::Driver::instance_of_minor`.
pub fn instance_of_minor(m: Minor) -> Option<Instance> {
    // SAFETY: plain arithmetic on the core's table.
    let i = unsafe { sys::drm_minor_instance(m.0) };
    (i >= 0).then_some(Instance(i))
}

// --- the C tables -------------------------------------------------------

/// The facade's tables of a driver type, built at compile time.
pub(crate) trait Tables: Driver {
    const DESCS: [sys::drm_ioctl_desc; ioctl::MAX_IOCTLS];
    const TABLE: sys::drm_driver;
}

impl<D: Driver> Tables for D {
    const DESCS: [sys::drm_ioctl_desc; ioctl::MAX_IOCTLS] = ioctl::descs::<D>();
    const TABLE: sys::drm_driver = sys::drm_driver {
        open: Some(open_thunk::<D>),
        postclose: Some(postclose_thunk::<D>),
        release: None,
        gem_prime_import: None,
        dumb_create: if D::DUMB { Some(dumb_thunk::<D>) } else { None },
        dumb_map_offset: None,
        major: D::MAJOR,
        minor: D::MINOR,
        patchlevel: D::PATCHLEVEL,
        name: D::NAME.as_ptr(),
        desc: D::DESC.as_ptr(),
        driver_features: D::FEATURES,
        ioctls: &<D as Tables>::DESCS as *const _ as *const sys::drm_ioctl_desc,
        num_ioctls: ioctl::count::<D>() as c_int,
        illumos_facade_major: sys::DRM_FACADE_MAJOR,
        illumos_facade_minor: sys::DRM_FACADE_MINOR,
        illumos_date: D::DATE.as_ptr(),
        illumos_max_object_size: D::MAX_OBJECT_SIZE,
        illumos_max_memory: D::MAX_MEMORY,
        illumos_before_detach: Some(before_detach_thunk::<D>),
        illumos_before_teardown: Some(before_teardown_thunk::<D>),
        illumos_rkms: if D::KMS { kms::funcs::<D>() } else { ptr::null() },
    };
}

unsafe extern "C" fn open_thunk<D: Driver>(dev: *mut sys::drm_device, file: *mut sys::drm_file) -> c_int {
    // SAFETY: the facade passes this driver's device and a new file.
    let (d, f) = unsafe { (Dev::<D>::from_raw(dev), File::from_raw(file)) };
    let Some(f) = f else { return neg(Errno::EINVAL) };
    match d.driver.file_open(d, &f) {
        Ok(()) => 0,
        Err(e) => neg(e),
    }
}

unsafe extern "C" fn postclose_thunk<D: Driver>(dev: *mut sys::drm_device, file: *mut sys::drm_file) {
    // SAFETY: as above.
    let (d, f) = unsafe { (Dev::<D>::from_raw(dev), File::from_raw(file)) };
    if let Some(f) = f {
        d.driver.file_close(d, &f);
    }
}

unsafe extern "C" fn dumb_thunk<D: Driver>(
    file: *mut sys::drm_file,
    dev: *mut sys::drm_device,
    args: *mut sys::drm_mode_create_dumb,
) -> c_int {
    // SAFETY: as above; args is the uAPI structure.
    let (d, f) = unsafe { (Dev::<D>::from_raw(dev), File::from_raw(file)) };
    let Some(f) = f else { return neg(Errno::EINVAL) };
    // SAFETY: the core passes a struct drm_mode_create_dumb.
    let a = unsafe { &mut *(args as *mut drm_uapi::drm_mode_create_dumb) };
    let req = DumbRequest { width: a.width, height: a.height, bpp: a.bpp, flags: a.flags };
    match d.driver.dumb_create(d, &f, &req) {
        Ok(c) => {
            a.handle = c.handle;
            a.pitch = c.pitch;
            a.size = c.size;
            0
        }
        Err(e) => neg(e),
    }
}

unsafe extern "C" fn before_detach_thunk<D: Driver>(dev: *mut sys::drm_device) {
    // SAFETY: as above.
    let d = unsafe { Dev::<D>::from_raw(dev) };
    d.driver.before_detach(d);
}

unsafe extern "C" fn before_teardown_thunk<D: Driver>(dev: *mut sys::drm_device) {
    // SAFETY: as above.
    let d = unsafe { Dev::<D>::from_raw(dev) };
    d.driver.before_teardown(d);
}

// --- cb_ops ---------------------------------------------------------------

/// The `cb_ops` of every Rust DRM driver: the core's entry points, for
/// `ddi::module! { driver: X, cb_ops: unsafe drm_facade::CB_OPS.get() }`.
/// The `unsafe` is the driver's promise that the devices of its module
/// are registered with [`Device::register`], so that the entry points find
/// them in `misc/drm_core`'s table (illumos-ddi `CharTable`).
#[allow(
    clippy::missing_transmute_annotations,
    clippy::useless_transmute,
    reason = "the facade's and ddi-sys's bindings name the opaque kernel types differently"
)]
pub static CB_OPS: StaticCell<dsys::cb_ops> = StaticCell::new(dsys::cb_ops {
    // SAFETY (all transmutes): the facade's entry points have the C
    // signatures of the cb_ops slots; the bindings differ only in how
    // they name the opaque kernel types.
    cb_open: Some(unsafe { core::mem::transmute::<OpenFn, _>(sys::drm_open as OpenFn) }),
    cb_close: Some(unsafe { core::mem::transmute::<CloseFn, _>(sys::drm_close as CloseFn) }),
    cb_strategy: Some(ddi::module::glue::nodev_strategy),
    cb_print: Some(ddi::module::glue::nodev_print),
    cb_dump: Some(ddi::module::glue::nodev_dump),
    cb_read: Some(unsafe { core::mem::transmute::<ReadFn, _>(sys::drm_read as ReadFn) }),
    cb_write: Some(ddi::module::glue::nodev_rw),
    cb_ioctl: Some(unsafe { core::mem::transmute::<IoctlFn, _>(sys::drm_ioctl as IoctlFn) }),
    cb_devmap: Some(unsafe { core::mem::transmute::<DevmapFn, _>(sys::drm_devmap as DevmapFn) }),
    cb_mmap: Some(ddi::module::glue::nodev_mmap),
    cb_segmap: Some(unsafe { core::mem::transmute::<SegmapFn, _>(sys::drm_segmap as SegmapFn) }),
    cb_chpoll: Some(unsafe { core::mem::transmute::<ChpollFn, _>(sys::drm_chpoll as ChpollFn) }),
    cb_prop_op: Some(dsys::ddi_prop_op),
    cb_str: ptr::null_mut(),
    cb_flag: dsys::D_MP | dsys::D_64BIT | dsys::D_NEW | dsys::D_DEVMAP,
    cb_rev: dsys::CB_REV,
    cb_aread: Some(ddi::module::glue::nodev_aio),
    cb_awrite: Some(ddi::module::glue::nodev_aio),
});

type OpenFn = unsafe extern "C" fn(*mut sys::dev_t, c_int, c_int, *mut sys::cred_t) -> c_int;
type CloseFn = unsafe extern "C" fn(sys::dev_t, c_int, c_int, *mut sys::cred_t) -> c_int;
type ReadFn = unsafe extern "C" fn(sys::dev_t, *mut sys::uio, *mut sys::cred_t) -> c_int;
type IoctlFn = unsafe extern "C" fn(sys::dev_t, c_int, isize, c_int, *mut sys::cred_t, *mut c_int) -> c_int;
type DevmapFn =
    unsafe extern "C" fn(sys::dev_t, sys::devmap_cookie_t, sys::offset_t, usize, *mut usize, sys::uint_t) -> c_int;
type SegmapFn = unsafe extern "C" fn(
    sys::dev_t,
    sys::off_t,
    *mut sys::as_,
    *mut sys::caddr_t,
    sys::off_t,
    sys::uint_t,
    sys::uint_t,
    sys::uint_t,
    *mut sys::cred_t,
) -> c_int;
type ChpollFn = unsafe extern "C" fn(sys::dev_t, i16, c_int, *mut i16, *mut *mut sys::pollhead) -> c_int;
