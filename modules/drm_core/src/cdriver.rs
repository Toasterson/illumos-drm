// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The facade driver: the one implementation of the core's `Driver` trait,
//! forwarding every hook to a driver's C table.

use alloc::boxed::Box;
use core::fmt;
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicU64, Ordering};

use ddi::sys;
use ddi::{DevInfo, Errno, IntrPriority, IoctlArg, Sleepable};
use drm_core::gem::{GemDma, GemRef};
use drm_core::ioctl::{DriverIoctl, SizePlan};
use drm_core::minor::{CloneId, NodeKind};
use drm_core::uaccess::{ArgIo, CoreArg};
use drm_core::{BusId, Device, Driver, DriverInfo, DumbCreated};
use drm_facade_sys as f;
use drm_uapi as uapi;

use crate::devcore::{DevCore, from_neg};
use crate::fobj::FObj;

/// The facade driver of one device.
pub(crate) struct CDriver {
    core: NonNull<DevCore>,
}

// SAFETY: DevCore is Send and Sync; the pointer stays valid while the
// registered device (which owns this driver value) exists.
unsafe impl Send for CDriver {}
// SAFETY: as above.
unsafe impl Sync for CDriver {}

/// A number for `drm_file.client_id`, unique among the files of the system
/// since the module loaded.
static CLIENT_IDS: AtomicU64 = AtomicU64::new(1);

impl CDriver {
    pub(crate) fn new(core: NonNull<DevCore>) -> CDriver {
        CDriver { core }
    }

    pub(crate) fn core(&self) -> &DevCore {
        // SAFETY: see the Send implementation.
        unsafe { self.core.as_ref() }
    }

    /// Calls the C handler of a matched ioctl with a kernel copy of its
    /// argument, as Linux's `drm_ioctl()`.
    fn call(
        &self,
        file: CloneId,
        entry: &DriverIoctl,
        cmd: u32,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let core = self.core();
        let desc = core.desc(entry.cmd).ok_or(Errno::ENOTTY)?;
        let func = desc.func.ok_or(Errno::ENOTTY)?;
        let fp = core.file(file).ok_or(Errno::ENXIO)?;
        let plan = SizePlan::new(entry.cmd, cmd);
        let a = CoreArg::new(arg, plan)?;
        // The kernel structure: at most 255 bytes (the size field of an
        // illumos command is eight bits), aligned for any field.
        #[repr(C, align(8))]
        struct Buf([u8; 256]);
        let mut buf = Buf([0; 256]);
        let k = plan.kernel;
        // SAFETY: k <= 255 bytes of buf.
        unsafe { a.copy_in_struct(buf.0.as_mut_ptr(), k)? };
        let ddev = core.ddev();
        let r = crate::ctxt::with_frame(self.core, (entry.cmd, cmd), arg, ctx, || {
            // SAFETY: the driver's handler with its device, a buffer of the
            // table's size and its file.
            unsafe { func(ddev, buf.0.as_mut_ptr().cast(), fp.as_ptr()) }
        })?;
        from_neg(r)?;
        // SAFETY: k initialised bytes.
        unsafe { a.copy_out_struct(buf.0.as_ptr(), k) }
    }
}

impl Driver for CDriver {
    fn info(&self) -> &DriverInfo {
        self.core().info()
    }

    fn ioctls(&self) -> &[DriverIoctl] {
        &self.core().ioctls
    }

    type Object = FObj;

    fn gem_dma(&self, _dev: &DevInfo) -> Option<GemDma> {
        self.core().setup.lock().dma.map(GemDma::new)
    }

    fn bus_id(&self, dev: &DevInfo, out: &mut BusId) -> fmt::Result {
        let s = self.core().setup.lock();
        if s.unique_len > 0 {
            let b = s.unique.get(..s.unique_len).ok_or(fmt::Error)?;
            let t = core::str::from_utf8(b).map_err(|_| fmt::Error)?;
            return fmt::Write::write_str(out, t);
        }
        drop(s);
        let name = dev.driver_name().to_str().map_err(|_| fmt::Error)?;
        fmt::Write::write_fmt(out, format_args!("pseudo:{}@{}", name, dev.instance().0))
    }

    fn dumb_create(
        &self,
        _dev: &Device<Self>,
        file: CloneId,
        req: &drm_core::dumb::DumbRequest,
        _ctx: &Sleepable,
    ) -> Result<DumbCreated, Errno> {
        let core = self.core();
        let create = core.table.dumb_create.ok_or(drm_core::driver::ENOSYS)?;
        let fp = core.file(file).ok_or(Errno::ENXIO)?;
        let mut a = uapi::drm_mode_create_dumb {
            height: req.height,
            width: req.width,
            bpp: req.bpp,
            flags: req.flags,
            ..Default::default()
        };
        // SAFETY: the driver's callback with its file, its device and a
        // structure of the uAPI layout.
        let r = unsafe { create(fp.as_ptr(), core.ddev(), ptr::addr_of_mut!(a).cast()) };
        from_neg(r)?;
        Ok(DumbCreated { handle: a.handle, pitch: a.pitch, size: a.size })
    }

    fn dumb_map_offset(&self, _dev: &Device<Self>, file: CloneId, handle: u32, _ctx: &Sleepable) -> Result<u64, Errno> {
        let core = self.core();
        let fp = core.file(file).ok_or(Errno::ENXIO)?;
        let mut off = 0u64;
        let r = match core.table.dumb_map_offset {
            // SAFETY: the driver's callback with its file and device.
            Some(cb) => unsafe { cb(fp.as_ptr(), core.ddev(), handle, &mut off) },
            // SAFETY: the facade's default with the same arguments.
            None => unsafe { crate::ffi::gem::drm_gem_dumb_map_offset(fp.as_ptr(), core.ddev(), handle, &mut off) },
        };
        from_neg(r)?;
        Ok(off)
    }

    fn ioctl(
        &self,
        _dev: &Device<Self>,
        file: CloneId,
        entry: &DriverIoctl,
        cmd: u32,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        self.call(file, entry, cmd, arg, ctx)
    }

    fn kms(&self) -> Option<&dyn drm_core::kms::KmsDriver<GemRef<FObj>>> {
        self.core().kms.as_ref().map(|k| k as &dyn drm_core::kms::KmsDriver<GemRef<FObj>>)
    }

    fn fence_priority(&self) -> Option<IntrPriority> {
        self.core().setup.lock().fence_pri
    }

    fn file_open(&self, dev: &Device<Self>, file: CloneId, ctx: &Sleepable) -> Result<(), Errno> {
        let core = self.core();
        let (kind, pid) = dev.file_info(file).ok_or(Errno::ENXIO)?;
        // SAFETY: plain data; zero is valid.
        let mut fp: Box<f::drm_file> = Box::new(unsafe { core::mem::zeroed() });
        fp.pid = pid;
        fp.client_id = CLIENT_IDS.fetch_add(1, Ordering::Relaxed);
        // SAFETY: the minor descriptors are set at registration and live
        // with the device state.
        let minors = unsafe { &mut *core.minors.get() };
        fp.minor = match kind {
            NodeKind::Primary => ptr::addr_of_mut!(minors[0]),
            NodeKind::Render => ptr::addr_of_mut!(minors[1]),
        };
        fp.__core[0] = u64::from(file.get());
        let fp = Box::into_raw(fp);
        fp_set_magic(fp);
        // SAFETY: just made.
        let fpn = unsafe { NonNull::new_unchecked(fp) };
        if let Err(e) = core.files.lock().push((file, fpn), ctx) {
            // SAFETY: never shared.
            drop(unsafe { Box::from_raw(fp) });
            return Err(e);
        }
        link_file(core, fp);
        if let Some(open) = core.table.open {
            // SAFETY: the driver's callback with its device and new file.
            let r = unsafe { open(core.ddev(), fp) };
            if let Err(e) = from_neg(r) {
                forget_file(core, file);
                return Err(e);
            }
        }
        Ok(())
    }

    fn file_close(&self, _dev: &Device<Self>, file: CloneId, _ctx: &Sleepable) {
        let core = self.core();
        let Some(fp) = core.file(file) else { return };
        if let Some(post) = core.table.postclose {
            // SAFETY: the driver's callback with its device and file.
            unsafe { post(core.ddev(), fp.as_ptr()) };
        }
        forget_file(core, file);
    }

    fn prime_import(
        &self,
        dev: &Device<Self>,
        exporter: &GemRef<FObj>,
        ctx: &Sleepable,
    ) -> Result<GemRef<FObj>, Errno> {
        let core = self.core();
        let Some(hook) = core.table.gem_prime_import else {
            return crate::fobj::import_object(core, exporter, ctx);
        };
        let buf = crate::ffi::prime::DmaBuf::make(exporter.clone());
        // SAFETY: the driver's callback with its device and a dma-buf it
        // may keep (it takes a reference with get_dma_buf).
        let obj = unsafe { hook(core.ddev(), buf) };
        // SAFETY: the dma-buf made above; drop this function's reference.
        unsafe { crate::ffi::prime::dma_buf_put(buf) };
        if obj.is_null() {
            return Err(Errno::ENOMEM);
        }
        let v = obj as isize;
        if (-4095..0).contains(&v) {
            return Err(Errno::new(-v as i32).unwrap_or(Errno::EINVAL));
        }
        // SAFETY: the hook returns an object with one reference for us.
        let r = unsafe { crate::fobj::gem_ref(obj) }.ok_or(Errno::EINVAL)?;
        // The returned reference was counted; gem_ref took another.
        // SAFETY: drop the hook's reference.
        unsafe { crate::ffi::gem::drm_gem_object_put(obj) };
        // The hook must return an object of this device: anything else
        // would put another device's object into this device's tables.
        if !dev.gem().is_some_and(|g| drm_core::karc::KArc::ptr_eq(r.core(), g)) {
            core.dip.warn(format_args!("gem_prime_import returned an object of another device"));
            r.put(ctx);
            return Err(Errno::EINVAL);
        }
        r.object().set_imported(exporter.clone());
        Ok(r)
    }

    fn before_detach(&self, _ctx: &Sleepable) {
        let core = self.core();
        if let Some(cb) = core.table.illumos_before_detach {
            // SAFETY: the driver's callback with its device.
            unsafe { cb(core.ddev()) };
        }
    }

    fn before_teardown(&self, _ctx: &Sleepable) {
        let core = self.core();
        if let Some(cb) = core.table.illumos_before_teardown {
            // SAFETY: as above.
            unsafe { cb(core.ddev()) };
        }
    }
}

fn fp_set_magic(fp: *mut f::drm_file) {
    // SAFETY: a live file structure being set up.
    unsafe { (*fp).__core[1] = (fp as u64) ^ crate::MAGIC_FILE };
}

/// The clone id of a `drm_file` the facade made, and its device state.
///
/// # Safety
/// `fp` is NULL or a `drm_file` the caller may read, of an open file.
pub(crate) unsafe fn file_of<'a>(fp: *const f::drm_file) -> Option<(CloneId, &'a DevCore)> {
    if fp.is_null() {
        return None;
    }
    // SAFETY: the caller's contract.
    let (id, check, minor) = unsafe { ((*fp).__core[0], (*fp).__core[1], (*fp).minor) };
    if check != (fp as u64) ^ crate::MAGIC_FILE || minor.is_null() {
        return None;
    }
    // SAFETY: set at open to the device's minor descriptor.
    let ddev = unsafe { (*minor).dev };
    // SAFETY: a live device (its file is open).
    let core = unsafe { DevCore::of(ddev) }?;
    Some((CloneId::new(u32::try_from(id).ok()?)?, core))
}

fn link_file(core: &DevCore, fp: *mut f::drm_file) {
    let d = core.ddev();
    // SAFETY: the list and its mutex live in the device structure; the
    // node in the new file.
    unsafe {
        sys::mutex_enter(ptr::addr_of_mut!((*d).filelist_mutex.m) as *mut sys::kmutex_t);
        let head = ptr::addr_of_mut!((*d).filelist);
        let node = ptr::addr_of_mut!((*fp).lhead);
        (*node).next = head;
        (*node).prev = (*head).prev;
        (*(*head).prev).next = node;
        (*head).prev = node;
        sys::mutex_exit(ptr::addr_of_mut!((*d).filelist_mutex.m) as *mut sys::kmutex_t);
    }
}

/// Unlinks and frees the `drm_file` of `file`.
fn forget_file(core: &DevCore, file: CloneId) {
    let fp = {
        let mut l = core.files.lock();
        let Some(i) = l.iter().position(|(c, _)| *c == file) else { return };
        l.swap_remove(i).map(|(_, p)| p)
    };
    let Some(fp) = fp else { return };
    let d = core.ddev();
    // SAFETY: as in link_file; the file is no longer reachable.
    unsafe {
        sys::mutex_enter(ptr::addr_of_mut!((*d).filelist_mutex.m) as *mut sys::kmutex_t);
        let node = ptr::addr_of_mut!((*fp.as_ptr()).lhead);
        (*(*node).prev).next = (*node).next;
        (*(*node).next).prev = (*node).prev;
        sys::mutex_exit(ptr::addr_of_mut!((*d).filelist_mutex.m) as *mut sys::kmutex_t);
        (*fp.as_ptr()).__core[1] = 0;
        drop(Box::from_raw(fp.as_ptr()));
    }
}
