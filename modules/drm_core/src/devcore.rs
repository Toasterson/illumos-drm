// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! What `misc/drm_core` keeps behind a `struct drm_device`: the driver's table,
//! the settings made before registration, the registered core device, and
//! the `struct drm_file` of every open file.
//!
//! `__drm_dev_alloc` allocates the driver's structure (zeroed, with
//! `kmem_zalloc(9F)`) and a [`DevCore`] (boxed), and links them: the
//! `drm_device` inside the structure points at its `DevCore` from
//! `__core[0]`, with `__core[1]` the address XORed with
//! [`MAGIC_DEVICE`](crate::MAGIC_DEVICE). The structure lives until the
//! last `drm_dev_put`; the registered device ([`Device`]) between
//! `drm_dev_register` and a successful `drm_dev_unregister`.

use alloc::boxed::Box;
use core::cell::UnsafeCell;
use core::ffi::{CStr, c_void};
use core::ops::Deref;
use core::ptr::{self, NonNull};

use ddi::sys;
use ddi::{DevInfo, DmaAttr, Errno, IntrPriority, Sleepable};
use drm_core::driver::{BUS_ID_MAX, DEFAULT_MAX_MEMORY, DEFAULT_MAX_OBJECT_SIZE};
use drm_core::ioctl::DriverIoctl;
use drm_core::ksync::KMutex;
use drm_core::kvec::KVec;
use drm_core::lkms::Registration;
use drm_core::minor::CloneId;
use drm_core::published::{PubGuard, Published};
use drm_core::state::Perm;
use drm_core::{Device, DriverInfo, Features, ModeConfig};
use drm_facade_sys as f;

use crate::cdriver::CDriver;
use crate::ckms::CKms;

/// The settings a driver makes between allocation and registration.
pub(crate) struct Setup {
    pub(crate) registered: bool,
    /// `drm_dev_illumos_unplug` took the device out of the table: its last
    /// put runs the driver's release first, then unregisters (see put).
    pub(crate) unplugged: bool,
    pub(crate) dma: Option<DmaAttr>,
    pub(crate) fence_pri: Option<IntrPriority>,
    /// The bus ID (`drm_dev_set_unique`), NUL-terminated.
    pub(crate) unique: [u8; BUS_ID_MAX + 1],
    pub(crate) unique_len: usize,
    /// Facade 2.0: whether the KMS code's `unregister_dev` is owed.
    pub(crate) lkms_reg: Registration,
    /// Facade 2.0: `drm_dev_illumos_lkms_register()` was called (a second
    /// call is refused).
    pub(crate) lkms_called: bool,
}

/// The state behind one `struct drm_device`.
pub(crate) struct DevCore {
    /// The device structure, inside the driver's allocation.
    pub(crate) ddev: NonNull<f::drm_device>,
    /// The driver's allocation: start and size, for `kmem_free(9F)`.
    alloc: (NonNull<u8>, usize),
    /// The driver's table; it lives in the driver module, which cannot
    /// unload while one of its devices exists.
    pub(crate) table: &'static f::drm_driver,
    /// The driver's node.
    pub(crate) dip: DevInfo,
    /// Written only before registration (the mode configuration).
    info: UnsafeCell<DriverInfo>,
    pub(crate) ioctls: KVec<DriverIoctl>,
    /// The C entries, in the order of [`DevCore::ioctls`].
    pub(crate) descs: KVec<&'static f::drm_ioctl_desc>,
    pub(crate) setup: KMutex<Setup>,
    /// The registered device, between `drm_dev_register` and a successful
    /// `drm_dev_unregister`.
    device: Published<Device<CDriver>>,
    /// The `drm_file` of every open file.
    pub(crate) files: KMutex<KVec<(CloneId, NonNull<f::drm_file>)>>,
    /// The minor descriptors `primary` and `render` point at.
    pub(crate) minors: UnsafeCell<[f::drm_minor; 2]>,
    /// The mode setting of a driver with `illumos_rkms`.
    pub(crate) kms: Option<CKms>,
    /// Facade 2.0: the hooks of a driver with `illumos_lkms`, whose
    /// mode setting `drm_dev_illumos_lkms_register()` starts.
    pub(crate) lkms: Option<&'static f::drm_illumos_lkms_funcs>,
}

// SAFETY: the raw pointers are the device structure (whose fields the core
// changes only during registration, which the DDI serialises with every
// other use of the device) and the driver's static table; the rest is
// behind locks or atomics.
unsafe impl Send for DevCore {}
// SAFETY: as above.
unsafe impl Sync for DevCore {}

/// The facade's error convention: a negative errno.
pub(crate) fn neg(e: Errno) -> i32 {
    -e.get()
}

/// An errno from a negative C result (0 or positive is success).
pub(crate) fn from_neg(r: i32) -> Result<(), Errno> {
    if r >= 0 { Ok(()) } else { Err(Errno::new(-r).unwrap_or(Errno::EINVAL)) }
}

fn cstr(p: *const core::ffi::c_char, default: &'static CStr) -> &'static CStr {
    if p.is_null() {
        default
    } else {
        // SAFETY: the driver's table holds NUL-terminated strings in the
        // driver module, which outlives its devices.
        unsafe { CStr::from_ptr(p) }
    }
}

/// The core's permission flags of a facade ioctl entry.
fn perm(flags: u32) -> Perm {
    Perm {
        auth: flags & f::DRM_AUTH != 0,
        master: flags & f::DRM_MASTER != 0,
        root_only: flags & f::DRM_ROOT_ONLY != 0,
        render_allow: flags & f::DRM_RENDER_ALLOW != 0,
    }
}

/// The core's features of a driver table, with the `DRIVER_*` flags
/// `driver_features` (the table's, or a device's at registration), masked
/// with the table's own: a feature counts only if both have it (Linux's
/// `drm_core_check_feature()`, so a device cannot add one its driver
/// lacks).
pub(crate) fn features(t: &f::drm_driver, driver_features: u32) -> Features {
    let driver_features = driver_features & t.driver_features;
    let mut out = Features::NONE;
    let map = [
        (f::DRIVER_GEM, Features::GEM),
        (f::DRIVER_MODESET, Features::MODESET),
        (f::DRIVER_RENDER, Features::RENDER),
        (f::DRIVER_ATOMIC, Features::ATOMIC),
        (f::DRIVER_SYNCOBJ, Features::SYNCOBJ),
        (f::DRIVER_SYNCOBJ_TIMELINE, Features::SYNCOBJ_TIMELINE),
    ];
    for (bit, feat) in map {
        if driver_features & bit != 0 {
            out = out.union(feat);
        }
    }
    if t.dumb_create.is_some() {
        out = out.union(Features::DUMB);
    }
    out
}

impl DevCore {
    /// Allocates a driver structure of `size` bytes with a `drm_device` at
    /// `offset` and its `DevCore`. Thread context.
    ///
    /// # Safety
    /// `table` points to a driver table that stays valid while the device
    /// exists; `dip` is the driver's own node.
    pub(crate) unsafe fn alloc(
        dip: *mut sys::dev_info,
        table: *const f::drm_driver,
        size: usize,
        offset: usize,
        ctx: &Sleepable,
    ) -> Result<NonNull<u8>, Errno> {
        if dip.is_null() || table.is_null() {
            return Err(Errno::EINVAL);
        }
        // SAFETY: the caller's contract.
        let table: &'static f::drm_driver = unsafe { &*table };
        if table.illumos_facade_major != f::DRM_FACADE_MAJOR || table.illumos_facade_minor > f::DRM_FACADE_MINOR {
            // SAFETY: a valid node and constant formats.
            unsafe {
                sys::dev_err(
                    dip,
                    sys::CE_WARN,
                    c"driver built for DRM facade %u.%u; misc/drm_core is %u.%u".as_ptr(),
                    table.illumos_facade_major,
                    table.illumos_facade_minor,
                    f::DRM_FACADE_MAJOR,
                    f::DRM_FACADE_MINOR,
                )
            };
            return Err(Errno::EINVAL);
        }
        let end = offset.checked_add(size_of::<f::drm_device>()).ok_or(Errno::EINVAL)?;
        if end > size || !offset.is_multiple_of(align_of::<f::drm_device>()) || table.name.is_null() {
            return Err(Errno::EINVAL);
        }
        // SAFETY: the node is live during the driver's attach.
        let dev = unsafe { DevInfo::from_raw(dip) };
        let info = DriverInfo {
            name: cstr(table.name, c"drm"),
            date: cstr(table.illumos_date, c"0"),
            desc: cstr(table.desc, c""),
            major: table.major,
            minor: table.minor,
            patchlevel: table.patchlevel,
            features: features(table, table.driver_features),
            mode_config: ModeConfig::DEFAULT,
            max_object_size: if table.illumos_max_object_size == 0 {
                DEFAULT_MAX_OBJECT_SIZE
            } else {
                usize::try_from(table.illumos_max_object_size).unwrap_or(usize::MAX)
            },
            max_memory: if table.illumos_max_memory == 0 {
                DEFAULT_MAX_MEMORY
            } else {
                usize::try_from(table.illumos_max_memory).unwrap_or(usize::MAX)
            },
        };
        let mut ioctls = KVec::new();
        let mut descs = KVec::new();
        let n = usize::try_from(table.num_ioctls).map_err(|_| Errno::EINVAL)?;
        if n > 0 && table.ioctls.is_null() {
            return Err(Errno::EINVAL);
        }
        for i in 0..n {
            // SAFETY: the table has num_ioctls entries.
            let d: &'static f::drm_ioctl_desc = unsafe { &*table.ioctls.add(i) };
            if d.func.is_none() {
                continue;
            }
            let e = DriverIoctl { cmd: d.cmd, perm: perm(d.flags) };
            if !e.in_driver_range() {
                dev.warn(format_args!("ioctl {:#x} is outside the driver range; ignored", d.cmd));
                continue;
            }
            ioctls.push(e, ctx)?;
            descs.push(d, ctx)?;
        }
        // Facade 2.0: the core's mode setting or Linux's, never both; the
        // KMS code's required hooks must be there.
        if !table.illumos_rkms.is_null() && !table.illumos_lkms.is_null() {
            dev.warn(format_args!("driver table has both illumos_rkms and illumos_lkms"));
            return Err(Errno::EINVAL);
        }
        // SAFETY: as the table, the funcs live in the driver module.
        let lkms = unsafe { table.illumos_lkms.as_ref() };
        if let Some(l) = lkms
            && (l.ioctl.is_none()
                || l.file_open.is_none()
                || l.file_close.is_none()
                || l.register_dev.is_none()
                || l.unregister_dev.is_none()
                || l.mode_config.is_none())
        {
            dev.warn(format_args!("illumos_lkms lacks a required hook"));
            return Err(Errno::EINVAL);
        }
        let kms = if table.illumos_rkms.is_null() {
            None
        } else {
            // SAFETY: as the table, the funcs live in the driver module.
            Some(CKms::new(unsafe { &*table.illumos_rkms }))
        };
        // SAFETY: kmem_zalloc(9F) with KM_SLEEP in thread context.
        let base = unsafe { sys::kmem_zalloc(size, sys::KM_SLEEP) } as *mut u8;
        let base = NonNull::new(base).ok_or(Errno::ENOMEM)?;
        // SAFETY: offset + size_of::<drm_device> <= size, aligned.
        let ddev = unsafe { NonNull::new_unchecked(base.as_ptr().add(offset) as *mut f::drm_device) };
        let core = Box::new(DevCore {
            ddev,
            alloc: (base, size),
            table,
            dip: dev,
            info: UnsafeCell::new(info),
            ioctls,
            descs,
            setup: KMutex::new(Setup {
                registered: false,
                unplugged: false,
                dma: None,
                fence_pri: None,
                unique: [0; BUS_ID_MAX + 1],
                unique_len: 0,
                lkms_reg: Registration::new(),
                lkms_called: false,
            }),
            device: Published::new(),
            files: KMutex::new(KVec::new()),
            // SAFETY: drm_minor is plain data; zero is a valid value.
            minors: UnsafeCell::new(unsafe { core::mem::zeroed() }),
            kms,
            lkms,
        });
        let core = Box::into_raw(core);
        // SAFETY: ddev points into the zeroed allocation.
        unsafe {
            let d = ddev.as_ptr();
            (*d).if_version = 0;
            (*d).ref_.refcount.refs.counter = 1;
            (*d).dev = dip as *mut f::dev_info_t;
            (*d).dma_dev = dip as *mut f::dev_info_t;
            (*d).driver = table;
            (*d).driver_features = table.driver_features;
            (*d).switch_power_state = f::DRM_SWITCH_POWER_ON;
            sys::mutex_init(
                ptr::addr_of_mut!((*d).filelist_mutex.m) as *mut sys::kmutex_t,
                ptr::null(),
                sys::kmutex_type_t::MUTEX_DRIVER,
                ptr::null_mut(),
            );
            sys::mutex_init(
                ptr::addr_of_mut!((*d).vblank_time_lock.rlock) as *mut sys::kmutex_t,
                ptr::null(),
                sys::kmutex_type_t::MUTEX_DRIVER,
                ptr::null_mut(),
            );
            sys::mutex_init(
                ptr::addr_of_mut!((*d).event_lock.rlock) as *mut sys::kmutex_t,
                ptr::null(),
                sys::kmutex_type_t::MUTEX_DRIVER,
                ptr::null_mut(),
            );
            let head = ptr::addr_of_mut!((*d).filelist);
            (*head).next = head;
            (*head).prev = head;
            drm_core::managed::init(d, base.as_ptr() as *mut c_void);
            (*d).__core[0] = core as u64;
            (*d).__core[1] = (core as u64) ^ crate::MAGIC_DEVICE;
        }
        Ok(base)
    }

    /// The `DevCore` of a device structure the facade set up, or `None`.
    ///
    /// # Safety
    /// `ddev` is NULL or points to a `struct drm_device` the caller may
    /// read, whose last reference has not been dropped.
    pub(crate) unsafe fn of<'a>(ddev: *const f::drm_device) -> Option<&'a DevCore> {
        if ddev.is_null() {
            return None;
        }
        // SAFETY: the caller's contract.
        let (p, check) = unsafe { ((*ddev).__core[0], (*ddev).__core[1]) };
        if p == 0 || p ^ crate::MAGIC_DEVICE != check {
            return None;
        }
        // SAFETY: set by alloc to a live DevCore, which lives as long as
        // the structure.
        let core = unsafe { &*(p as *const DevCore) };
        if !ptr::eq(core.ddev.as_ptr(), ddev) {
            return None;
        }
        Some(core)
    }

    pub(crate) fn info(&self) -> &DriverInfo {
        // SAFETY: changed only by set_mode_config before registration,
        // which the DDI serialises with every use of the device.
        unsafe { &*self.info.get() }
    }

    /// Sets the mode configuration; before registration only.
    ///
    /// # Safety
    /// The device is not registered and no other thread uses it.
    pub(crate) unsafe fn set_mode_config(&self, m: ModeConfig) {
        // SAFETY: the caller's contract.
        unsafe { (*self.info.get()).mode_config = m };
    }

    /// Sets the features (from the device's `driver_features` at
    /// registration); before registration only.
    ///
    /// # Safety
    /// As [`DevCore::set_mode_config`].
    pub(crate) unsafe fn set_features(&self, features: Features) {
        // SAFETY: the caller's contract.
        unsafe { (*self.info.get()).features = features };
    }

    /// Facade 2.0: the KMS code's hooks of a Linux-KMS driver's device.
    pub(crate) fn lkms_funcs(&self) -> Option<&'static f::drm_illumos_lkms_funcs> {
        self.lkms
    }

    pub(crate) fn ddev(&self) -> *mut f::drm_device {
        self.ddev.as_ptr()
    }

    /// The registered device, if any, held for the guard's life: a
    /// concurrent `drm_dev_unregister` waits until the guard is dropped.
    /// Any context.
    pub(crate) fn device(&self) -> Option<DevGuard<'_>> {
        let g = self.device.enter()?;
        // SAFETY: published by register from a live box; withdraw clears
        // the pointer and waits for every guard before the box goes.
        let d = unsafe { &*g.get() };
        Some(DevGuard { _g: g, d })
    }

    pub(crate) fn publish(&self, d: *mut Device<CDriver>) {
        self.device.publish(d);
    }

    /// Withdraws the registered device and waits until no facade call uses
    /// it any more. Thread context.
    pub(crate) fn withdraw(&self) -> *mut Device<CDriver> {
        // SAFETY: delay(9F) in thread context (drm_dev_unregister).
        self.device.withdraw(|| unsafe { sys::delay(1) })
    }

    /// The `drm_file` of open file `c`.
    pub(crate) fn file(&self, c: CloneId) -> Option<NonNull<f::drm_file>> {
        self.files.lock().iter().find(|(id, _)| *id == c).map(|(_, p)| *p)
    }

    /// The C entry of a matched ioctl.
    pub(crate) fn desc(&self, cmd: u32) -> Option<&'static f::drm_ioctl_desc> {
        let i = self.ioctls.iter().position(|e| e.cmd == cmd)?;
        self.descs.get(i).copied()
    }

    /// Drops a reference to the device; the last one calls the driver's
    /// release callback and frees the structure and this state.
    ///
    /// # Safety
    /// `this` is a live `DevCore` whose device holds a reference the
    /// caller gives up.
    pub(crate) unsafe fn put(this: *const DevCore) -> Result<(), Errno> {
        // SAFETY: the caller's contract.
        let core = unsafe { &*this };
        let d = core.ddev();
        // SAFETY: the structure is live while a reference is held; the
        // count is changed only atomically.
        let left = unsafe {
            let c = ptr::addr_of_mut!((*d).ref_.refcount.refs.counter) as *mut u32;
            sys::atomic_dec_32_nv(c)
        };
        if left != 0 {
            return Ok(());
        }
        if core.device.is_published() {
            if !core.setup.lock().unplugged {
                core.dip.warn(format_args!("last reference to a registered DRM device dropped; device leaked"));
                return Err(Errno::EBUSY);
            }
            // A driver that unplugged its device (Linux's order: unplug,
            // remove, last put) frees its own GEM objects in its release:
            // release first, then the core unregisters the device, now
            // that the driver's objects are gone. If the core still finds
            // state (fences, objects), the device stays, leaked, with a
            // warning: the caller's detach then fails.
            if let Some(rel) = core.table.release {
                // SAFETY: the driver's callback, with its device.
                unsafe { rel(d) };
            }
            // Fences the driver's last requests signal may still be
            // released by the deferred releases: up to five seconds.
            let mut tries = 0;
            // SAFETY: the device's last reference, thread context (the
            // caller's detach).
            while unsafe { crate::ffi::device::unregister(d, core) } != 0 {
                tries += 1;
                if tries >= 100 {
                    core.dip.warn(format_args!("the DRM device keeps state after its driver's release; device leaked"));
                    return Err(Errno::EBUSY);
                }
                // SAFETY: delay(9F) in thread context.
                unsafe { sys::delay(sys::drv_usectohz(50_000)) };
            }
        } else if let Some(rel) = core.table.release {
            // SAFETY: the driver's callback, with its device.
            unsafe { rel(d) };
        }
        // The managed resources, newest first, after the driver's release
        // and before the structure goes (Linux's drm_managed_release).
        // SAFETY: the device's last reference is gone; thread context.
        unsafe {
            drm_core::managed::release_all(d);
            drm_core::managed::fini(d);
        }
        // SAFETY: nothing refers to the structure or the state any more.
        unsafe {
            sys::mutex_destroy(ptr::addr_of_mut!((*d).filelist_mutex.m) as *mut sys::kmutex_t);
            sys::mutex_destroy(ptr::addr_of_mut!((*d).vblank_time_lock.rlock) as *mut sys::kmutex_t);
            sys::mutex_destroy(ptr::addr_of_mut!((*d).event_lock.rlock) as *mut sys::kmutex_t);
            let (base, size) = core.alloc;
            drop(Box::from_raw(this as *mut DevCore));
            sys::kmem_free(base.as_ptr() as *mut c_void, size);
        }
        Ok(())
    }

    /// Takes another reference.
    pub(crate) fn get(&self) {
        let d = self.ddev();
        // SAFETY: as in put.
        unsafe {
            let c = ptr::addr_of_mut!((*d).ref_.refcount.refs.counter) as *mut u32;
            sys::atomic_inc_32(c);
        }
    }
}

/// A registered device in use by a facade call (see [`DevCore::device`]).
pub(crate) struct DevGuard<'a> {
    _g: PubGuard<'a, Device<CDriver>>,
    d: &'a Device<CDriver>,
}

impl Deref for DevGuard<'_> {
    type Target = Device<CDriver>;
    fn deref(&self) -> &Device<CDriver> {
        self.d
    }
}
