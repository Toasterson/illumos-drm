// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! GEM objects: a host resource with guest memory behind it.
//!
//! Every object is system memory from the core's R1 backend
//! ([`SysMem`]), bound for DMA against the device's own node, whose
//! scatter/gather cookies become the resource's memory entries
//! (`RESOURCE_ATTACH_BACKING`, or the entries of `RESOURCE_CREATE_BLOB`).
//! The host resource is 2D (`RESOURCE_CREATE_2D`: dumb buffers, and
//! `RESOURCE_CREATE` without virgl), 3D (`RESOURCE_CREATE_3D`) or a blob
//! with guest memory (`BLOB_MEM_GUEST`, `BLOB_MEM_HOST3D_GUEST`).
//!
//! With virgl, a handle to an object attaches its resource to the file's
//! rendering context and removing the handle detaches it
//! (`CTX_ATTACH_RESOURCE`, `CTX_DETACH_RESOURCE`, Linux's GEM open and
//! close callbacks). When the object is destroyed, `RESOURCE_UNREF` tells
//! the host to drop the resource and its backing, and the memory is freed
//! only once the device has answered; if it never does, the memory is
//! leaked rather than freed under the device.

use core::mem::size_of;
use core::ptr::NonNull;
use core::sync::atomic::Ordering;

use ddi::prelude::*;
use drm::gem::{GemCore, GemObject, GemRef, SysMem, UmemCookie};
use drm::karc::KArc;
use drm::minor::CloneId;

use crate::gpu::{Cmd, Dev, Gpu, Q, dev_warn};
use crate::proto::{self, CtrlHdr, MemEntry};

/// A pointer to the device, which outlives every object (the DRM core
/// refuses to detach while objects exist).
#[derive(Clone, Copy)]
pub struct DevRef(NonNull<Dev>);

// SAFETY: the device is Sync and outlives every holder (see above).
unsafe impl Send for DevRef {}
// SAFETY: as above.
unsafe impl Sync for DevRef {}

impl DevRef {
    /// Wraps the device.
    ///
    /// # Safety
    /// The device must outlive the value and everything it is copied into.
    pub unsafe fn new(dev: &Dev) -> DevRef {
        DevRef(NonNull::from(dev))
    }
    /// The device.
    pub fn dev(&self) -> &Dev {
        // SAFETY: the contract of `new`.
        unsafe { self.0.as_ref() }
    }
    /// The driver state.
    pub fn gpu(&self) -> &Gpu {
        self.dev().state()
    }
}

/// What kind of host resource an object has.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// No resource (memory converted by the core; never used by this
    /// driver).
    None,
    /// A 2D resource: its size and virtio format, for scanout transfers.
    Res2d {
        /// Width in pixels.
        width: u32,
        /// Height in pixels.
        height: u32,
    },
    /// A 3D (virgl) resource.
    Res3d,
    /// A blob resource with guest memory.
    Blob {
        /// `VIRTGPU_BLOB_MEM_*`.
        mem: u32,
    },
}

/// A buffer object of `virtio_gpu`.
pub struct VgObject {
    mem: Option<SysMem>,
    cookie: UmemCookie,
    size: usize,
    segments: usize,
    /// The host resource id, 0 for none.
    res: u32,
    /// The host resource exists.
    created: bool,
    kind: Kind,
    dev: Option<DevRef>,
}

// SAFETY: the memory is SysMem (Send and Sync) and its cookie, a handle the
// kernel accepts from any thread; the rest is plain data and DevRef.
unsafe impl Send for VgObject {}
// SAFETY: as above; `&VgObject` only reads fixed fields.
unsafe impl Sync for VgObject {}

impl From<SysMem> for VgObject {
    /// Memory without a host resource. The core converts dumb buffer memory
    /// this way by default; this driver creates dumb buffers with
    /// `Driver::dumb_object` instead, so no such object is ever made.
    fn from(m: SysMem) -> VgObject {
        VgObject {
            cookie: m.umem_cookie(),
            size: m.size(),
            segments: m.sg_len(),
            mem: Some(m),
            res: 0,
            created: false,
            kind: Kind::None,
            dev: None,
        }
    }
}

impl VgObject {
    /// The host resource id.
    pub fn res(&self) -> u32 {
        self.res
    }
    /// The kind of resource.
    pub fn kind(&self) -> Kind {
        self.kind
    }
    /// `VIRTGPU_BLOB_MEM_*` for `RESOURCE_INFO`: 0 for resources that are
    /// not blobs.
    pub fn blob_mem(&self) -> u32 {
        match self.kind {
            Kind::Blob { mem } => mem,
            _ => 0,
        }
    }
    /// The memory entries of the backing: its DMA cookies, `None` for a
    /// cookie longer than an entry can say (4 GiB).
    fn entries(&self) -> impl Iterator<Item = Option<MemEntry>> + '_ {
        self.mem
            .iter()
            .flat_map(|m| m.sg_iter())
            .map(|c| u32::try_from(c.size).ok().map(|length| MemEntry { addr: c.address, length, padding: 0 }))
    }
    fn nr_entries(&self) -> usize {
        self.segments
    }
}

/// Writes the memory entries of `obj` into the data buffer of `c`.
fn write_entries(c: &Cmd, obj: &VgObject) -> Result<(), Errno> {
    let d = c.data().ok_or(Errno::EINVAL)?;
    for (i, e) in obj.entries().enumerate() {
        let e = e.ok_or(Errno::EINVAL)?;
        d.write(i * size_of::<MemEntry>(), proto::bytes_of(&e))?;
    }
    Ok(())
}

/// How to create an object's resource.
pub enum Create {
    /// `RESOURCE_CREATE_2D`.
    Res2d {
        /// `FORMAT_*`.
        format: u32,
        /// Width.
        width: u32,
        /// Height.
        height: u32,
    },
    /// `RESOURCE_CREATE_3D`.
    Res3d(proto::ResourceCreate3d),
    /// `RESOURCE_CREATE_BLOB`.
    Blob {
        /// `BLOB_MEM_*`.
        mem: u32,
        /// `VIRTGPU_BLOB_FLAG_*`.
        flags: u32,
        /// The host object (3D blobs).
        blob_id: u64,
        /// The context (3D blobs).
        ctx_id: u32,
    },
}

/// Makes a GEM object of `size` bytes (rounded up to pages) with a host
/// resource created as `how`. The memory is charged to the device's budget
/// first; everything is undone on failure.
pub fn create(
    gem: &KArc<GemCore<VgObject>>,
    dr: DevRef,
    size: usize,
    how: Create,
    ctx: &Sleepable,
) -> Result<GemRef<VgObject>, Errno> {
    let size = size.checked_next_multiple_of(drm::offsets::PAGE_SIZE as usize).ok_or(Errno::EINVAL)?;
    if size == 0 || size > crate::MAX_OBJECT {
        return Err(if size == 0 { Errno::EINVAL } else { Errno::ENOMEM });
    }
    let reservation = gem.reserve(size, ctx)?;
    let mem = gem.sys_mem(size, ctx)?;
    let obj = with_resource(dr, mem, how, ctx)?;
    GemCore::create_reserved(gem, obj, reservation, ctx)
}

/// Wraps `mem` in an object and creates its host resource: sends the
/// creation and the backing, and waits for both.
pub fn with_resource(dr: DevRef, mem: SysMem, how: Create, ctx: &Sleepable) -> Result<VgObject, Errno> {
    let gpu = dr.gpu();
    let dev = dr.dev();
    let res = gpu.resource_id()?;
    let kind = match &how {
        Create::Res2d { width, height, .. } => Kind::Res2d { width: *width, height: *height },
        Create::Res3d(_) => Kind::Res3d,
        Create::Blob { mem, .. } => Kind::Blob { mem: *mem },
    };
    let mut obj = VgObject {
        cookie: mem.umem_cookie(),
        size: mem.size(),
        segments: mem.sg_len(),
        mem: Some(mem),
        res,
        created: false,
        kind,
        dev: Some(dr),
    };
    let entries = obj.nr_entries();
    if entries == 0 {
        // The memory must be bound for DMA (Driver::DMA_ATTR).
        return Err(Errno::EIO);
    }
    let data_len = entries.checked_mul(size_of::<MemEntry>()).ok_or(Errno::ENOMEM)?;
    if data_len > crate::gpu::MAX_DATA {
        return Err(Errno::ENOMEM);
    }
    match send_create(gpu, dev, &mut obj, how, data_len, ctx) {
        Ok(()) => Ok(obj),
        Err(e) => {
            // Not a GEM object yet, so the core will not call release();
            // unref whatever the device may have created.
            obj.destroy(ctx);
            Err(e)
        }
    }
}

/// Sends the creation of `obj`'s resource and its backing and waits for
/// both; marks the object created once the first request is queued.
fn send_create(
    gpu: &Gpu,
    dev: &Dev,
    obj: &mut VgObject,
    how: Create,
    data_len: usize,
    ctx: &Sleepable,
) -> Result<(), Errno> {
    let hdr_len = size_of::<CtrlHdr>();
    let res = obj.res;
    let first = match how {
        Create::Res2d { format, width, height } => {
            let req = proto::ResourceCreate2d {
                hdr: CtrlHdr::new(proto::CMD_RESOURCE_CREATE_2D),
                resource_id: res,
                format,
                width,
                height,
            };
            gpu.cmd(dev, &req, hdr_len, 0, Q::Ctrl, ctx)?
        }
        Create::Res3d(mut req) => {
            req.hdr = CtrlHdr::new(proto::CMD_RESOURCE_CREATE_3D);
            req.resource_id = res;
            gpu.cmd(dev, &req, hdr_len, 0, Q::Ctrl, ctx)?
        }
        Create::Blob { mem, flags, blob_id, ctx_id } => {
            let req = proto::ResourceCreateBlob {
                hdr: CtrlHdr::ctx(proto::CMD_RESOURCE_CREATE_BLOB, ctx_id),
                resource_id: res,
                blob_mem: mem,
                blob_flags: flags,
                nr_entries: obj.nr_entries() as u32,
                blob_id,
                size: obj.size as u64,
            };
            let c = gpu.cmd(dev, &req, hdr_len, data_len, Q::Ctrl, ctx)?;
            write_entries(&c, obj)?;
            c
        }
    };
    let blob = matches!(obj.kind, Kind::Blob { .. });
    gpu.submit(dev, &first, None, ctx)?;
    obj.created = true;
    gpu.stats.resources.fetch_add(1, Ordering::Relaxed);
    gpu.stats.live_resources.fetch_add(1, Ordering::Relaxed);
    // A blob gets its memory entries with the creation; the others attach
    // their backing next. The device processes the control queue in order.
    let second = if blob { None } else { Some(attach_backing(gpu, dev, obj, data_len, ctx)?) };
    let t1 = gpu.wait(dev, &first, ctx)?;
    crate::gpu::check(t1, proto::RESP_OK_NODATA)?;
    if let Some(c) = second {
        crate::gpu::check(gpu.wait(dev, &c, ctx)?, proto::RESP_OK_NODATA)?;
    }
    Ok(())
}

fn attach_backing(gpu: &Gpu, dev: &Dev, obj: &VgObject, data_len: usize, ctx: &Sleepable) -> Result<KArc<Cmd>, Errno> {
    let req = proto::AttachBacking {
        hdr: CtrlHdr::new(proto::CMD_RESOURCE_ATTACH_BACKING),
        resource_id: obj.res,
        nr_entries: obj.nr_entries() as u32,
    };
    let c = gpu.cmd(dev, &req, size_of::<CtrlHdr>(), data_len, Q::Ctrl, ctx)?;
    write_entries(&c, obj)?;
    gpu.submit(dev, &c, None, ctx)?;
    Ok(c)
}

impl GemObject for VgObject {
    fn size(&self) -> usize {
        self.size
    }
    fn dma_segments(&self) -> usize {
        self.segments
    }
    fn umem(&self) -> UmemCookie {
        self.cookie
    }
    fn handle_opened(&self, file: CloneId, ctx: &Sleepable) -> Result<(), Errno> {
        let Some(dr) = self.dev else { return Ok(()) };
        let gpu = dr.gpu();
        if !gpu.virgl() || !self.created {
            return Ok(());
        }
        // As Linux: the context may not exist yet when the first ioctl of a
        // file is MODE_CREATE_DUMB or PRIME_FD_TO_HANDLE.
        let id = gpu.ensure_context(dr.dev(), file, ctx)?;
        let req = proto::ResourceOnly {
            hdr: CtrlHdr::ctx(proto::CMD_CTX_ATTACH_RESOURCE, id),
            resource_id: self.res,
            padding: 0,
        };
        gpu.fire(dr.dev(), &req, ctx).map(drop)
    }
    fn handle_closed(&self, file: CloneId, ctx: &Sleepable) {
        let Some(dr) = self.dev else { return };
        let gpu = dr.gpu();
        if !gpu.virgl() || !self.created || !gpu.has_context(file) {
            return;
        }
        let req = proto::ResourceOnly {
            hdr: CtrlHdr::ctx(proto::CMD_CTX_DETACH_RESOURCE, Gpu::ctx_id(file)),
            resource_id: self.res,
            padding: 0,
        };
        if let Err(e) = gpu.fire(dr.dev(), &req, ctx) {
            dev_warn(gpu.instance, format_args!("CTX_DETACH_RESOURCE of resource {}: {e:?}", self.res));
        }
    }
    fn release(&mut self, ctx: &Sleepable) {
        self.destroy(ctx);
    }
}

impl VgObject {
    /// Drops the host resource (`RESOURCE_UNREF`, which also detaches the
    /// backing) and waits for the device's answer, after which the memory
    /// may be freed. If the device does not answer, the memory is leaked.
    fn destroy(&mut self, ctx: &Sleepable) {
        let Some(dr) = self.dev else { return };
        if !core::mem::take(&mut self.created) {
            return;
        }
        let gpu = dr.gpu();
        let req =
            proto::ResourceOnly { hdr: CtrlHdr::new(proto::CMD_RESOURCE_UNREF), resource_id: self.res, padding: 0 };
        let r = gpu
            .cmd(dr.dev(), &req, size_of::<CtrlHdr>(), 0, Q::Ctrl, ctx)
            .and_then(|c| gpu.submit(dr.dev(), &c, None, ctx).and_then(|_| gpu.wait(dr.dev(), &c, ctx)));
        match r {
            // Any answer means the device is done with the memory; an error
            // answer (a resource the host never created) was logged.
            Ok(_) => {
                gpu.stats.live_resources.fetch_sub(1, Ordering::Relaxed);
            }
            Err(e) => {
                dev_warn(
                    gpu.instance,
                    format_args!(
                        "RESOURCE_UNREF of resource {} failed ({e:?}); leaking its {} bytes",
                        self.res, self.size
                    ),
                );
                if let Some(m) = self.mem.take() {
                    core::mem::forget(m);
                }
            }
        }
    }
}
