// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! GEM objects: a host resource with guest memory behind it.
//!
//! Every object is a [`Gem<VgObject>`]: system memory of the DRM core
//! (backing 0 of `drm_gem_object_init`), bound for DMA against the
//! device's own node because the device registers with the driver's DMA
//! attributes. The backing's scatter/gather cookies become the resource's
//! memory entries (`RESOURCE_ATTACH_BACKING`, or the entries of
//! `RESOURCE_CREATE_BLOB`). The host resource is 2D (`RESOURCE_CREATE_2D`:
//! dumb buffers, and `RESOURCE_CREATE` without virgl), 3D
//! (`RESOURCE_CREATE_3D`) or a blob with guest memory (`BLOB_MEM_GUEST`,
//! `BLOB_MEM_HOST3D_GUEST`).
//!
//! With virgl, a file's first handle to an object attaches its resource to
//! the file's rendering context and removing its last handle detaches it
//! (`CTX_ATTACH_RESOURCE`, `CTX_DETACH_RESOURCE`, Linux's GEM open and
//! close callbacks, which the core calls once per file and object: the
//! host does not count attachments, so a second handle or a re-import must
//! neither attach nor detach again). When the object is destroyed, `RESOURCE_UNREF` tells
//! the host to drop the resource and its backing, and the driver waits for
//! the device's answer before the core frees the memory. If the device
//! does not answer, the warning says so; the core frees the memory anyway,
//! because the facade has no way to keep an object's memory after its
//! release.

use core::mem::size_of;
use core::ptr::NonNull;
use core::sync::atomic::{AtomicBool, Ordering};

use ddi::prelude::*;
use drm_facade::karc::KArc;
use drm_facade::{File, Gem, GemObject, GemRef};

use crate::gpu::{Cmd, Dev, DrmDev, Gpu, Q, dev_warn};
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
    /// A 2D resource: its size, for scanout transfers.
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

/// The data of a buffer object of `virtio_gpu`, after the core's
/// `drm_gem_object` in a [`Gem<VgObject>`].
pub struct VgObject {
    /// The host resource id (never 0).
    res: u32,
    /// The host resource exists: set once its creation was queued, taken
    /// by the destruction.
    created: AtomicBool,
    kind: Kind,
    dev: DevRef,
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
    fn created(&self) -> bool {
        self.created.load(Ordering::Acquire)
    }
}

/// The memory entries of an object's backing: its DMA cookies, `None` for
/// a cookie longer than an entry can say (4 GiB).
fn entries(gem: &Gem<VgObject>) -> impl Iterator<Item = Option<MemEntry>> + '_ {
    gem.sg_iter(0).map(|c| u32::try_from(c.size).ok().map(|length| MemEntry { addr: c.address, length, padding: 0 }))
}

/// Writes the memory entries of `gem` into the data buffer of `c`.
fn write_entries(c: &Cmd, gem: &Gem<VgObject>) -> Result<(), Errno> {
    let d = c.data().ok_or(Errno::EINVAL)?;
    for (i, e) in entries(gem).enumerate() {
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

/// Makes a GEM object of `size` bytes (rounded up to pages) of DRM device
/// `drm` with a host resource created as `how`: the facade checks the
/// device's limits and allocates the memory, then the resource is created
/// and its backing attached, waiting for both. On failure the object is
/// dropped, and its release unrefs whatever the device may have created.
pub fn create(drm: &DrmDev, dr: DevRef, size: usize, how: Create, ctx: &Sleepable) -> Result<GemRef<VgObject>, Errno> {
    let size = size.checked_next_multiple_of(drm_facade::offsets::PAGE_SIZE as usize).ok_or(Errno::EINVAL)?;
    if size == 0 || size > crate::MAX_OBJECT {
        return Err(if size == 0 { Errno::EINVAL } else { Errno::ENOMEM });
    }
    let gpu = dr.gpu();
    let res = gpu.resource_id()?;
    let kind = match &how {
        Create::Res2d { width, height, .. } => Kind::Res2d { width: *width, height: *height },
        Create::Res3d(_) => Kind::Res3d,
        Create::Blob { mem, .. } => Kind::Blob { mem: *mem },
    };
    let obj = VgObject { res, created: AtomicBool::new(false), kind, dev: dr };
    let gem = GemRef::new(drm, size, obj, true)?;
    let entries = gem.sg_count(0);
    if entries == 0 {
        // The memory must be bound for DMA (Settings::dma).
        return Err(Errno::EIO);
    }
    let data_len = entries.checked_mul(size_of::<MemEntry>()).ok_or(Errno::ENOMEM)?;
    if data_len > crate::gpu::MAX_DATA {
        return Err(Errno::ENOMEM);
    }
    send_create(gpu, dr.dev(), &gem, how, data_len, ctx)?;
    Ok(gem)
}

/// Sends the creation of `gem`'s resource and its backing and waits for
/// both; marks the object created once the first request is queued.
fn send_create(
    gpu: &Gpu,
    dev: &Dev,
    gem: &Gem<VgObject>,
    how: Create,
    data_len: usize,
    ctx: &Sleepable,
) -> Result<(), Errno> {
    let hdr_len = size_of::<CtrlHdr>();
    let res = gem.res;
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
                nr_entries: gem.sg_count(0) as u32,
                blob_id,
                size: gem.size() as u64,
            };
            let c = gpu.cmd(dev, &req, hdr_len, data_len, Q::Ctrl, ctx)?;
            write_entries(&c, gem)?;
            c
        }
    };
    let blob = matches!(gem.kind, Kind::Blob { .. });
    gpu.submit(dev, &first, None, ctx)?;
    gem.created.store(true, Ordering::Release);
    gpu.stats.resources.fetch_add(1, Ordering::Relaxed);
    gpu.stats.live_resources.fetch_add(1, Ordering::Relaxed);
    // A blob gets its memory entries with the creation; the others attach
    // their backing next. The device processes the control queue in order.
    let second = if blob { None } else { Some(attach_backing(gpu, dev, gem, data_len, ctx)?) };
    let t1 = gpu.wait(dev, &first, ctx)?;
    crate::gpu::check(t1, proto::RESP_OK_NODATA)?;
    if let Some(c) = second {
        crate::gpu::check(gpu.wait(dev, &c, ctx)?, proto::RESP_OK_NODATA)?;
    }
    Ok(())
}

fn attach_backing(
    gpu: &Gpu,
    dev: &Dev,
    gem: &Gem<VgObject>,
    data_len: usize,
    ctx: &Sleepable,
) -> Result<KArc<Cmd>, Errno> {
    let req = proto::AttachBacking {
        hdr: CtrlHdr::new(proto::CMD_RESOURCE_ATTACH_BACKING),
        resource_id: gem.res,
        nr_entries: gem.sg_count(0) as u32,
    };
    let c = gpu.cmd(dev, &req, size_of::<CtrlHdr>(), data_len, Q::Ctrl, ctx)?;
    write_entries(&c, gem)?;
    gpu.submit(dev, &c, None, ctx)?;
    Ok(c)
}

/// The facade calls the object's hooks in thread context.
fn ctx() -> Sleepable {
    // SAFETY: GEM open, close and release run in thread context (the
    // facade's contract for drm_gem_object_funcs).
    unsafe { Sleepable::assume() }
}

impl GemObject for VgObject {
    fn open(gem: &Gem<Self>, file: &File) -> Result<(), Errno> {
        let dr = gem.dev;
        let gpu = dr.gpu();
        if !gpu.virgl() || !gem.created() {
            return Ok(());
        }
        let ctx = ctx();
        // As Linux: the context may not exist yet when the first ioctl of a
        // file is MODE_CREATE_DUMB or PRIME_FD_TO_HANDLE.
        let id = gpu.ensure_context(dr.dev(), file.id(), &ctx)?;
        let req = proto::ResourceOnly {
            hdr: CtrlHdr::ctx(proto::CMD_CTX_ATTACH_RESOURCE, id),
            resource_id: gem.res,
            padding: 0,
        };
        gpu.fire(dr.dev(), &req, &ctx).map(drop)
    }

    fn close(gem: &Gem<Self>, file: &File) {
        let dr = gem.dev;
        let gpu = dr.gpu();
        if !gpu.virgl() || !gem.created() {
            return;
        }
        let Some(id) = gpu.context(file.id()) else { return };
        let req = proto::ResourceOnly {
            hdr: CtrlHdr::ctx(proto::CMD_CTX_DETACH_RESOURCE, id),
            resource_id: gem.res,
            padding: 0,
        };
        if let Err(e) = gpu.fire(dr.dev(), &req, &ctx()) {
            dev_warn(gpu.instance, format_args!("CTX_DETACH_RESOURCE of resource {}: {e:?}", gem.res));
        }
    }

    /// Drops the host resource (`RESOURCE_UNREF`, which also detaches the
    /// backing) and waits for the device's answer, after which the core
    /// frees the memory. Also the end of an object whose resource was never
    /// created, or whose creation failed half-way. Without an answer the
    /// device may still use the memory, so the core is told to leak it.
    fn release(gem: &mut Gem<Self>) -> bool {
        if !gem.created.swap(false, Ordering::AcqRel) {
            return true;
        }
        let dr = gem.dev;
        let gpu = dr.gpu();
        let ctx = ctx();
        let req =
            proto::ResourceOnly { hdr: CtrlHdr::new(proto::CMD_RESOURCE_UNREF), resource_id: gem.res, padding: 0 };
        let r = gpu
            .cmd(dr.dev(), &req, size_of::<CtrlHdr>(), 0, Q::Ctrl, &ctx)
            .and_then(|c| gpu.submit(dr.dev(), &c, None, &ctx).and_then(|_| gpu.wait(dr.dev(), &c, &ctx)));
        match r {
            // Any answer means the device is done with the memory; an error
            // answer (a resource the host never created) was logged.
            Ok(_) => {
                gpu.stats.live_resources.fetch_sub(1, Ordering::Relaxed);
                true
            }
            Err(e) => {
                dev_warn(
                    gpu.instance,
                    format_args!(
                        "RESOURCE_UNREF of resource {} failed ({e:?}); leaking its {} bytes",
                        gem.res,
                        gem.size()
                    ),
                );
                false
            }
        }
    }
}
