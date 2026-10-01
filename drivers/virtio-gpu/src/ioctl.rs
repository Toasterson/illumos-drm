// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The `DRM_IOCTL_VIRTGPU_*` ioctls, with the arguments, checks and errnos
//! of Linux's `virtgpu_ioctl.c` and `virtgpu_submit.c` (Linux 6.12),
//! except where the module documentation of the crate lists a deviation.
//!
//! All of them are allowed on render nodes and need no authentication
//! (`DRM_RENDER_ALLOW`, as Linux declares them).
//!
//! The core copies an argument in before the handler and out after it
//! returns success ([`IoctlData`]); the arrays, command streams and
//! buffers an argument points to are copied with `drm_facade::ioctl`.

use alloc::vec::Vec;
use core::mem::size_of;
use core::time::Duration;

use ddi::prelude::*;
use drm_facade::ioctl::{copy_in, copy_in_array, copy_out};
use drm_facade::kvec::KVec;
use drm_facade::sync::{sync_file_export, sync_file_fence};
use drm_facade::{ENOSYS, Fence, File, GemRef, IoctlData, Plain, Syncobj, Usage};
use drm_uapi as uapi;

use crate::VirtioGpu;
use crate::gpu::{self, ContextParam, Dev, DrmDev, FenceReq, Q, parse_context_params};
use crate::object::{self, Create, Kind, VgObject};
use crate::proto::{self, CtrlHdr};

/// `VIRTGPU_CONTEXT_PARAM_DEBUG_NAME`.
const CONTEXT_PARAM_DEBUG_NAME: u64 = 4;
/// The largest command stream of one `EXECBUFFER` or blob creation. Linux
/// has no limit but memory; Mesa 21.3 sends at most 266,240 bytes.
const MAX_COMMAND: u32 = gpu::MAX_DATA as u32;
/// How long `WAIT` waits (Linux: 15 seconds, then `EBUSY`).
const WAIT_TIMEOUT: Duration = Duration::from_secs(15);
/// The most entries of an array an ioctl names (buffer handles, sync
/// objects, context parameters): more is `EINVAL`, as the R4 core's
/// `copy_in_array` refused them, before any memory is allocated for them.
const MAX_ARRAY: usize = 1 << 16;
/// The `EXECBUFFER` flags this driver knows.
const EXECBUF_FLAGS: u32 =
    uapi::VIRTGPU_EXECBUF_FENCE_FD_IN | uapi::VIRTGPU_EXECBUF_FENCE_FD_OUT | uapi::VIRTGPU_EXECBUF_RING_IDX;
/// The offset of `fence_fd` in every layout of the `EXECBUFFER` argument,
/// the only field the ioctl changes.
const FENCE_FD_OFFSET: usize = 28;
const _: () = assert!(core::mem::offset_of!(uapi::drm_virtgpu_execbuffer, fence_fd) == FENCE_FD_OFFSET);
const _: () = assert!(core::mem::offset_of!(uapi::drm_virtgpu_execbuffer_v1, fence_fd) == FENCE_FD_OFFSET);
const _: () = assert!(core::mem::offset_of!(uapi::drm_virtgpu_execbuffer_v2, fence_fd) == FENCE_FD_OFFSET);

/// A uAPI structure of this driver's ioctls: integer fields in an order
/// without padding, so that every bit pattern is a value and writing it
/// back copies out no uninitialised byte.
///
/// # Safety
/// The implementing type is `#[repr(C)]`, has integer fields only (or
/// structures of them) and no padding.
unsafe trait Uapi: Copy + 'static {}

/// A uAPI structure as the facade copies it in and out.
#[repr(transparent)]
#[derive(Clone, Copy)]
struct Arg<T>(T);

// SAFETY: Arg<T> has the layout of T, which Uapi makes Plain.
unsafe impl<T: Uapi> Plain for Arg<T> {}

macro_rules! uapi_args {
    ($($t:ident = $size:literal),* $(,)?) => {
        $(
            // SAFETY: integer fields without padding: the size is the sum
            // of the fields (Linux's layout; checked against gcc by
            // tools/uapi).
            unsafe impl Uapi for uapi::$t {}
            const _: () = assert!(size_of::<uapi::$t>() == $size);
        )*
    };
}

uapi_args! {
    drm_virtgpu_map = 16,
    drm_virtgpu_execbuffer = 64,
    drm_virtgpu_execbuffer_v1 = 32,
    drm_virtgpu_execbuffer_v2 = 40,
    drm_virtgpu_execbuffer_syncobj = 16,
    drm_virtgpu_getparam = 16,
    drm_virtgpu_resource_create = 56,
    drm_virtgpu_resource_info = 16,
    drm_virtgpu_3d_transfer_to_host = 44,
    drm_virtgpu_3d_transfer_to_host_v1 = 36,
    drm_virtgpu_3d_wait = 8,
    drm_virtgpu_get_caps = 24,
    drm_virtgpu_resource_create_blob = 48,
    drm_virtgpu_context_init = 16,
    drm_virtgpu_context_set_param = 16,
}

/// The argument as the core copied it in.
fn read<T: Uapi>(a: &IoctlData<'_>) -> Result<T, Errno> {
    a.read::<Arg<T>>().map(|v| v.0)
}

/// Leaves `v` in the argument for the core's copy-out.
fn write<T: Uapi>(a: &mut IoctlData<'_>, v: T) -> Result<(), Errno> {
    a.write(&Arg(v))
}

/// Copies `n` structures from user address `addr` (`EINVAL` for more than
/// [`MAX_ARRAY`]).
fn copy_in_structs<T: Uapi>(addr: u64, n: usize) -> Result<Vec<T>, Errno> {
    if n > MAX_ARRAY {
        return Err(Errno::EINVAL);
    }
    Ok(copy_in_array::<Arg<T>>(addr, n)?.into_iter().map(|a| a.0).collect())
}

/// The token of an ioctl handler.
fn ctx() -> Sleepable {
    // SAFETY: the facade calls ioctl handlers in thread context.
    unsafe { Sleepable::assume() }
}

/// The handler of every `DRM_IOCTL_VIRTGPU_*` command: dispatches on the
/// command of the table entry the core matched, as each older layout has
/// an entry of its own.
pub(crate) fn ioctl(dev: &DrmDev, a: &mut IoctlData<'_>, file: &File) -> Result<(), Errno> {
    let ctx = ctx();
    let d = dev.driver();
    match a.cmd() {
        uapi::DRM_IOCTL_VIRTGPU_GETPARAM => d.getparam(a),
        uapi::DRM_IOCTL_VIRTGPU_RESOURCE_CREATE => d.resource_create(dev, file, a, &ctx),
        uapi::DRM_IOCTL_VIRTGPU_RESOURCE_INFO => d.resource_info(file, a),
        uapi::DRM_IOCTL_VIRTGPU_MAP => d.map(file, a),
        cmd @ (uapi::DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST | uapi::DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST) => {
            let t: uapi::drm_virtgpu_3d_transfer_to_host = read(a)?;
            d.transfer(dev, file, cmd == uapi::DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, t, &ctx)
        }
        cmd @ (uapi::DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST_V1 | uapi::DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST_V1) => {
            let t: uapi::drm_virtgpu_3d_transfer_to_host_v1 = read(a)?;
            d.transfer(dev, file, cmd == uapi::DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST_V1, widen_transfer(&t), &ctx)
        }
        uapi::DRM_IOCTL_VIRTGPU_WAIT => d.wait(file, a),
        uapi::DRM_IOCTL_VIRTGPU_GET_CAPS => d.get_caps(a, &ctx),
        uapi::DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB => d.create_blob(dev, file, a, &ctx),
        uapi::DRM_IOCTL_VIRTGPU_CONTEXT_INIT => d.context_init(dev, file, a, &ctx),
        cmd @ (uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER
        | uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER_V1
        | uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER_V2) => {
            let x = read_execbuffer(cmd, a)?;
            d.execbuffer(dev, file, x, a, &ctx)
        }
        _ => Err(Errno::ENOTTY),
    }
}

impl VirtioGpu {
    fn dev(&self) -> &Dev {
        &self.virtio
    }

    /// `DRM_IOCTL_VIRTGPU_GETPARAM`: writes an `int` to the address in
    /// `value`, as Linux does.
    fn getparam(&self, a: &IoctlData<'_>) -> Result<(), Errno> {
        let v: uapi::drm_virtgpu_getparam = read(a)?;
        let caps = self.dev().state().caps();
        let out: i32 = match v.param {
            p if p == u64::from(uapi::VIRTGPU_PARAM_3D_FEATURES) => i32::from(caps.virgl),
            p if p == u64::from(uapi::VIRTGPU_PARAM_CAPSET_QUERY_FIX) => 1,
            p if p == u64::from(uapi::VIRTGPU_PARAM_RESOURCE_BLOB) => i32::from(caps.blob),
            // Host-visible memory needs the shared memory regions of the
            // virtio PCI transport, which the gate's framework lacks.
            p if p == u64::from(uapi::VIRTGPU_PARAM_HOST_VISIBLE) => 0,
            // Cross-device sharing needs resource UUIDs; not offered.
            p if p == u64::from(uapi::VIRTGPU_PARAM_CROSS_DEVICE) => 0,
            p if p == u64::from(uapi::VIRTGPU_PARAM_CONTEXT_INIT) => i32::from(caps.context_init),
            p if p == u64::from(uapi::VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs) => caps.capset_mask() as i32,
            p if p == u64::from(uapi::VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME) => i32::from(caps.context_init),
            _ => return Err(Errno::EINVAL),
        };
        copy_out(v.value, &out.to_ne_bytes())
    }

    /// `DRM_IOCTL_VIRTGPU_RESOURCE_CREATE`.
    fn resource_create(&self, dev: &DrmDev, file: &File, a: &mut IoctlData<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let mut v: uapi::drm_virtgpu_resource_create = read(a)?;
        let gpu = self.dev().state();
        let how = if gpu.virgl() {
            gpu.ensure_context(self.dev(), file.id(), ctx)?;
            Create::Res3d(proto::ResourceCreate3d {
                target: v.target,
                format: v.format,
                bind: v.bind,
                width: v.width,
                height: v.height,
                depth: v.depth,
                array_size: v.array_size,
                last_level: v.last_level,
                nr_samples: v.nr_samples,
                flags: v.flags,
                ..proto::ResourceCreate3d::default()
            })
        } else {
            // Linux's checks for 2D: a single 2D image (PIPE_TEXTURE_2D).
            if v.depth > 1 || v.nr_samples > 1 || v.last_level > 1 || v.target != 2 || v.array_size > 1 {
                return Err(Errno::EINVAL);
            }
            Create::Res2d { format: v.format, width: v.width, height: v.height }
        };
        // Linux: "allocate a single page size object" for a size of 0.
        let size = if v.size == 0 { drm_facade::offsets::PAGE_SIZE as usize } else { v.size as usize };
        let obj = object::create(dev, self.dr, size, how, ctx)?;
        v.bo_handle = obj.handle_create(file)?;
        v.res_handle = obj.res();
        write(a, v)
    }

    /// `DRM_IOCTL_VIRTGPU_RESOURCE_INFO`.
    fn resource_info(&self, file: &File, a: &mut IoctlData<'_>) -> Result<(), Errno> {
        let mut v: uapi::drm_virtgpu_resource_info = read(a)?;
        let obj = GemRef::<VgObject>::lookup(file, v.bo_handle)?;
        v.res_handle = obj.res();
        v.size = obj.size() as u32;
        v.blob_mem = obj.blob_mem();
        write(a, v)
    }

    /// `DRM_IOCTL_VIRTGPU_MAP`: the object's mmap offset.
    fn map(&self, file: &File, a: &mut IoctlData<'_>) -> Result<(), Errno> {
        let mut v: uapi::drm_virtgpu_map = read(a)?;
        let obj = GemRef::<VgObject>::lookup(file, v.handle)?;
        v.offset = obj.mmap_offset()?;
        write(a, v)
    }

    /// `DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST` (`to_host` false) and
    /// `TRANSFER_TO_HOST`, in the 44-byte layout (older 36-byte arguments
    /// are widened by the caller).
    fn transfer(
        &self,
        dev: &DrmDev,
        file: &File,
        to_host: bool,
        t: uapi::drm_virtgpu_3d_transfer_to_host,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let gpu = self.dev().state();
        let obj = GemRef::<VgObject>::lookup(file, t.bo_handle)?;
        if !gpu.virgl() {
            if !to_host {
                return Err(ENOSYS);
            }
            // 2D: copy the rectangle into the host resource, no fence.
            let req = proto::TransferToHost2d {
                hdr: CtrlHdr::new(proto::CMD_TRANSFER_TO_HOST_2D),
                r: proto::Rect { x: t.box_.x, y: t.box_.y, width: t.box_.w, height: t.box_.h },
                offset: u64::from(t.offset),
                resource_id: obj.res(),
                padding: 0,
            };
            return gpu.fire(self.dev(), &req, ctx).map(drop);
        }
        // A guest-only blob has no host storage to transfer to or from.
        if obj.kind() == (Kind::Blob { mem: proto::BLOB_MEM_GUEST }) {
            return Err(Errno::EINVAL);
        }
        let ctx_id = gpu.ensure_context(self.dev(), file.id(), ctx)?;
        let req = proto::TransferHost3d {
            hdr: CtrlHdr::ctx(
                if to_host { proto::CMD_TRANSFER_TO_HOST_3D } else { proto::CMD_TRANSFER_FROM_HOST_3D },
                ctx_id,
            ),
            box_: proto::Box3d { x: t.box_.x, y: t.box_.y, z: t.box_.z, w: t.box_.w, h: t.box_.h, d: t.box_.d },
            offset: u64::from(t.offset),
            resource_id: obj.res(),
            level: t.level,
            stride: t.stride,
            layer_stride: t.layer_stride,
        };
        let f = self.submit_with(dev, file, &req, None, core::slice::from_ref(&obj), None, ctx)?;
        obj.add_fence(&f, Usage::Write)
    }

    /// Sends `req` (with the `n` bytes at user address `addr` as its extra
    /// data, if `data` is given) as a fenced request of `file` that holds
    /// `objs` until the device has completed it, and returns its fence.
    #[allow(clippy::too_many_arguments, reason = "the parts of one fenced request")]
    fn submit_with<T: proto::Wire>(
        &self,
        dev: &DrmDev,
        file: &File,
        req: &T,
        data: Option<(u64, u32)>,
        objs: &[GemRef<VgObject>],
        ring: Option<u32>,
        ctx: &Sleepable,
    ) -> Result<Fence, Errno> {
        let gpu = self.dev().state();
        let data_len = data.map_or(0, |(_, n)| n as usize);
        let c = gpu.cmd(self.dev(), req, size_of::<CtrlHdr>(), data_len, Q::Ctrl, ctx)?;
        if let Some((addr, n)) = data
            && n > 0
        {
            let d = c.data().ok_or(Errno::EINVAL)?;
            // Straight from user memory into the DMA buffer.
            // SAFETY: the destination is `n` bytes of DMA memory the request
            // owns and nothing else accesses before it is submitted.
            let dst = unsafe { core::slice::from_raw_parts_mut(d.as_ptr().as_ptr(), n as usize) };
            copy_in(addr, dst)?;
        }
        let mut held = KVec::new();
        for o in objs {
            held.push(o.clone(), ctx)?;
        }
        c.hold(held);
        let fr = gpu.file_fence(file.id(), ring)?;
        gpu.submit(self.dev(), &c, Some((fr, dev)), ctx)?.ok_or(Errno::EIO)
    }

    /// `DRM_IOCTL_VIRTGPU_WAIT`: waits for the object's readers and writers
    /// (15 s, then `EBUSY`), or only tests them with `VIRTGPU_WAIT_NOWAIT`.
    fn wait(&self, file: &File, a: &IoctlData<'_>) -> Result<(), Errno> {
        let v: uapi::drm_virtgpu_3d_wait = read(a)?;
        let obj = GemRef::<VgObject>::lookup(file, v.handle)?;
        if v.flags & uapi::VIRTGPU_WAIT_NOWAIT != 0 {
            return if obj.test_signaled(Usage::Read) { Ok(()) } else { Err(Errno::EBUSY) };
        }
        match obj.wait(Usage::Read, true, WAIT_TIMEOUT.as_nanos() as i64) {
            Err(e) if e == Errno::ETIME => Err(Errno::EBUSY),
            r => r,
        }
    }

    /// `DRM_IOCTL_VIRTGPU_GET_CAPS`.
    fn get_caps(&self, a: &IoctlData<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let v: uapi::drm_virtgpu_get_caps = read(a)?;
        let gpu = self.dev().state();
        let caps = gpu.caps();
        if caps.capsets.is_empty() {
            return Err(ENOSYS);
        }
        if v.size == 0 {
            return Err(Errno::EINVAL);
        }
        let info = caps
            .capsets
            .iter()
            .find(|c| c.id == v.cap_set_id && c.max_version >= v.cap_set_ver)
            .ok_or(Errno::EINVAL)?;
        let data = gpu.capset(self.dev(), info.id, v.cap_set_ver, ctx)?;
        let n = (v.size as usize).min(info.max_size as usize).min(data.len());
        copy_out(v.addr, data.get(..n).ok_or(Errno::EIO)?)
    }

    /// `DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB`: guest-memory blobs
    /// (`BLOB_MEM_GUEST`, `BLOB_MEM_HOST3D_GUEST`) only.
    fn create_blob(&self, dev: &DrmDev, file: &File, a: &mut IoctlData<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let mut v: uapi::drm_virtgpu_resource_create_blob = read(a)?;
        let gpu = self.dev().state();
        let caps = gpu.caps();
        if !caps.blob {
            return Err(Errno::EINVAL);
        }
        let known = uapi::VIRTGPU_BLOB_FLAG_USE_MAPPABLE
            | uapi::VIRTGPU_BLOB_FLAG_USE_SHAREABLE
            | uapi::VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE;
        // Cross-device sharing needs resource UUIDs (CROSS_DEVICE is 0).
        if v.blob_flags & !known != 0 || v.blob_flags & uapi::VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE != 0 {
            return Err(Errno::EINVAL);
        }
        let host3d = match v.blob_mem {
            uapi::VIRTGPU_BLOB_MEM_GUEST => {
                if v.blob_id != 0 || v.cmd_size != 0 {
                    return Err(Errno::EINVAL);
                }
                false
            }
            uapi::VIRTGPU_BLOB_MEM_HOST3D_GUEST => {
                if !caps.virgl || !v.cmd_size.is_multiple_of(4) {
                    return Err(Errno::EINVAL);
                }
                true
            }
            // Host memory needs a shared memory region to map it; the
            // gate's virtio framework has none, so HOST_VISIBLE is 0 and
            // host-only blobs are refused.
            _ => return Err(Errno::EINVAL),
        };
        if v.size == 0 || v.cmd_size > MAX_COMMAND {
            return Err(Errno::EINVAL);
        }
        let size = usize::try_from(v.size).map_err(|_| Errno::ENOMEM)?;
        let ctx_id = if caps.virgl { gpu.ensure_context(self.dev(), file.id(), ctx)? } else { 0 };
        if v.cmd_size > 0 {
            let req = proto::CmdSubmit {
                hdr: CtrlHdr::ctx(proto::CMD_SUBMIT_3D, ctx_id),
                size: v.cmd_size,
                num_in_fences: 0,
            };
            let f = self.submit_with(dev, file, &req, Some((v.cmd, v.cmd_size)), &[], None, ctx)?;
            drop(f);
        }
        let how = Create::Blob {
            mem: v.blob_mem,
            flags: v.blob_flags,
            blob_id: v.blob_id,
            ctx_id: if host3d { ctx_id } else { 0 },
        };
        let obj = object::create(dev, self.dr, size, how, ctx)?;
        v.bo_handle = obj.handle_create(file)?;
        v.res_handle = obj.res();
        write(a, v)
    }

    /// `DRM_IOCTL_VIRTGPU_CONTEXT_INIT`.
    fn context_init(&self, dev: &DrmDev, file: &File, a: &IoctlData<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let v: uapi::drm_virtgpu_context_init = read(a)?;
        let gpu = self.dev().state();
        let caps = gpu.caps();
        if !caps.context_init {
            return Err(Errno::EINVAL);
        }
        // Four parameters exist, each allowed once.
        if v.num_params > 4 {
            return Err(Errno::EINVAL);
        }
        let raw = copy_in_structs::<uapi::drm_virtgpu_context_set_param>(v.ctx_set_params, v.num_params as usize)?;
        let mut params = Vec::new();
        for p in raw.iter() {
            params.push(match p.param {
                x if x == u64::from(uapi::VIRTGPU_CONTEXT_PARAM_CAPSET_ID) => ContextParam::Capset(p.value),
                x if x == u64::from(uapi::VIRTGPU_CONTEXT_PARAM_NUM_RINGS) => ContextParam::Rings(p.value),
                x if x == u64::from(uapi::VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK) => ContextParam::PollRings(p.value),
                CONTEXT_PARAM_DEBUG_NAME => {
                    let mut b = [0u8; 64];
                    copy_in(p.value, &mut b[..63]).or_else(|_| {
                        // A shorter string at the end of a page: read it
                        // byte by byte up to its NUL.
                        read_cstr(p.value, &mut b[..63])
                    })?;
                    let n = b.iter().position(|&c| c == 0).unwrap_or(63);
                    ContextParam::Name(b[..n].to_vec())
                }
                _ => return Err(Errno::EINVAL),
            });
        }
        let parsed = parse_context_params(params, caps.capset_mask())?;
        gpu.init_context(self.dev(), file.id(), &parsed, dev, ctx)
    }

    /// `DRM_IOCTL_VIRTGPU_EXECBUFFER`, in any of its three layouts (`x`
    /// is the argument widened to the current layout, `a` the argument
    /// itself, whose `fence_fd` takes the out-fence).
    fn execbuffer(
        &self,
        dev: &DrmDev,
        file: &File,
        x: uapi::drm_virtgpu_execbuffer,
        a: &mut IoctlData<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let gpu = self.dev().state();
        if !gpu.virgl() {
            return Err(ENOSYS);
        }
        if x.flags & !EXECBUF_FLAGS != 0 {
            return Err(Errno::EINVAL);
        }
        if x.size > MAX_COMMAND {
            return Err(Errno::EINVAL);
        }
        let ring = (x.flags & uapi::VIRTGPU_EXECBUF_RING_IDX != 0).then_some(x.ring_idx);
        // Checks the ring before anything is created (EINVAL).
        let own = gpu.file_fence(file.id(), ring)?;
        if (x.num_in_syncobjs != 0 || x.num_out_syncobjs != 0)
            && x.syncobj_stride as usize != size_of::<uapi::drm_virtgpu_execbuffer_syncobj>()
        {
            return Err(Errno::EINVAL);
        }
        let ins = copy_in_structs::<uapi::drm_virtgpu_execbuffer_syncobj>(x.in_syncobjs, x.num_in_syncobjs as usize)?;
        let outs =
            copy_in_structs::<uapi::drm_virtgpu_execbuffer_syncobj>(x.out_syncobjs, x.num_out_syncobjs as usize)?;
        if ins.iter().chain(outs.iter()).any(|s| s.flags & !uapi::VIRTGPU_EXECBUF_SYNCOBJ_RESET != 0) {
            return Err(Errno::EINVAL);
        }
        // Everything that can fail on the caller's input is looked up before
        // the submission.
        let mut out_objs: Vec<(Syncobj, u64)> = Vec::new();
        for s in outs.iter() {
            out_objs.push((Syncobj::find(file, s.handle)?, s.point));
        }
        let n = x.num_bo_handles as usize;
        if n > MAX_ARRAY {
            return Err(Errno::EINVAL);
        }
        let handles = copy_in_array::<u32>(x.bo_handles, n)?;
        let mut bos = KVec::new();
        for &h in handles.iter() {
            bos.push(GemRef::<VgObject>::lookup(file, h)?, ctx)?;
        }
        let fence = self.execbuffer_locked(dev, file, &x, (ring, own), &ins, &out_objs, &bos, ctx)?;
        if x.flags & uapi::VIRTGPU_EXECBUF_FENCE_FD_OUT != 0 {
            let fd = sync_file_export(&fence, true)?;
            // Only fence_fd changes; the core copies the argument out after
            // the handler returns.
            set_fence_fd(a, fd)?;
        }
        Ok(())
    }

    /// Waits for the in-fences, submits the command stream and publishes its
    /// fence in the objects and the out sync objects; returns the fence.
    #[allow(clippy::too_many_arguments, reason = "the parsed parts of one EXECBUFFER")]
    fn execbuffer_locked(
        &self,
        dev: &DrmDev,
        file: &File,
        x: &uapi::drm_virtgpu_execbuffer,
        (ring, own): (Option<u32>, FenceReq),
        ins: &[uapi::drm_virtgpu_execbuffer_syncobj],
        outs: &[(Syncobj, u64)],
        bos: &KVec<GemRef<VgObject>>,
        ctx: &Sleepable,
    ) -> Result<Fence, Errno> {
        let gpu = self.dev().state();
        // In-fences: the device orders its own fences; others are waited
        // for here, interruptibly, as Linux does.
        if x.flags & uapi::VIRTGPU_EXECBUF_FENCE_FD_IN != 0 {
            let f = sync_file_fence(x.fence_fd)?;
            wait_foreign(own, &f)?;
        }
        let mut resets = Vec::new();
        for s in ins.iter() {
            let so = Syncobj::find(file, s.handle)?;
            let f = Syncobj::find_fence(file, s.handle, s.point, false)?;
            wait_foreign(own, &f)?;
            if s.flags & uapi::VIRTGPU_EXECBUF_SYNCOBJ_RESET != 0 {
                resets.push(so);
            }
        }
        let ctx_id = gpu.ensure_context(self.dev(), file.id(), ctx)?;
        let req = proto::CmdSubmit { hdr: CtrlHdr::ctx(proto::CMD_SUBMIT_3D, ctx_id), size: x.size, num_in_fences: 0 };
        let fence = self.submit_with(dev, file, &req, Some((x.command, x.size)), bos, ring, ctx)?;
        // The work is queued; what follows only publishes its fence.
        let mut r = Ok(());
        for b in bos.iter() {
            if let Err(e) = b.add_fence(&fence, Usage::Write) {
                r = Err(e);
            }
        }
        for (so, point) in outs {
            so.add_point(&fence, *point);
        }
        for so in resets {
            so.replace_fence(None);
        }
        r.map(|()| fence)
    }
}

/// Waits for fence `f` unless it is on `own`, the fence context of the
/// submission, whose order the device keeps (Linux's
/// `dma_fence_match_context`); fences of other contexts, other rings of
/// the same file included, are waited for here, interruptibly.
fn wait_foreign(own: FenceReq, f: &Fence) -> Result<(), Errno> {
    if f.context() == own.context {
        return Ok(());
    }
    f.wait(true, -1)
}

/// Reads a NUL-terminated string of at most `out.len()` bytes from user
/// address `addr`.
fn read_cstr(addr: u64, out: &mut [u8]) -> Result<(), Errno> {
    for (i, b) in out.iter_mut().enumerate() {
        let mut c = [0u8];
        copy_in(addr + i as u64, &mut c)?;
        *b = c[0];
        if c[0] == 0 {
            break;
        }
    }
    Ok(())
}

/// Widens a 36-byte transfer argument.
pub(crate) fn widen_transfer(t: &uapi::drm_virtgpu_3d_transfer_to_host_v1) -> uapi::drm_virtgpu_3d_transfer_to_host {
    uapi::drm_virtgpu_3d_transfer_to_host {
        bo_handle: t.bo_handle,
        box_: t.box_,
        level: t.level,
        offset: t.offset,
        stride: 0,
        layer_stride: 0,
    }
}

/// Sets `fence_fd` in the `EXECBUFFER` argument, in the layout of the
/// matched command.
fn set_fence_fd(a: &mut IoctlData<'_>, fd: i32) -> Result<(), Errno> {
    match a.cmd() {
        uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER => {
            let mut v: uapi::drm_virtgpu_execbuffer = read(a)?;
            v.fence_fd = fd;
            write(a, v)
        }
        uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER_V2 => {
            let mut v: uapi::drm_virtgpu_execbuffer_v2 = read(a)?;
            v.fence_fd = fd;
            write(a, v)
        }
        uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER_V1 => {
            let mut v: uapi::drm_virtgpu_execbuffer_v1 = read(a)?;
            v.fence_fd = fd;
            write(a, v)
        }
        _ => Err(Errno::ENOTTY),
    }
}

/// Reads the `EXECBUFFER` argument of command `cmd`, zero-extended to the
/// current layout.
pub(crate) fn read_execbuffer(cmd: u32, a: &IoctlData<'_>) -> Result<uapi::drm_virtgpu_execbuffer, Errno> {
    let mut x = uapi::drm_virtgpu_execbuffer::default();
    match cmd {
        uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER => x = read(a)?,
        uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER_V2 => {
            let v: uapi::drm_virtgpu_execbuffer_v2 = read(a)?;
            x.flags = v.flags;
            x.size = v.size;
            x.command = v.command;
            x.bo_handles = v.bo_handles;
            x.num_bo_handles = v.num_bo_handles;
            x.fence_fd = v.fence_fd;
            x.ring_idx = v.ring_idx;
        }
        uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER_V1 => {
            let v: uapi::drm_virtgpu_execbuffer_v1 = read(a)?;
            x.flags = v.flags;
            x.size = v.size;
            x.command = v.command;
            x.bo_handles = v.bo_handles;
            x.num_bo_handles = v.num_bo_handles;
            x.fence_fd = v.fence_fd;
        }
        _ => return Err(Errno::ENOTTY),
    }
    Ok(x)
}
