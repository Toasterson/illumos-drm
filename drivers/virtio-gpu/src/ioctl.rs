// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The `DRM_IOCTL_VIRTGPU_*` ioctls, with the arguments, checks and errnos
//! of Linux's `virtgpu_ioctl.c` and `virtgpu_submit.c` (Linux 6.12),
//! except where the module documentation of the crate lists a deviation.
//!
//! All of them are allowed on render nodes and need no authentication
//! (`DRM_RENDER_ALLOW`, as Linux declares them).

use alloc::vec::Vec;
use core::mem::size_of;
use core::time::Duration;

use ddi::prelude::*;
use drm::defer::Ctx;
use drm::fence::Fence;
use drm::gem::GemRef;
use drm::kvec::KVec;
use drm::minor::CloneId;
use drm::resv::Usage;
use drm::syncobj::Syncobj;
use drm::uaccess::{UserArg, copy_in_array, copy_in_bytes, copy_out_bytes};
use drm_uapi as uapi;

use crate::VirtioGpu;
use crate::gpu::{self, Dev, Q};
use crate::object::{self, Create, Kind, VgObject};
use crate::proto::{self, CtrlHdr};

/// `VIRTGPU_CONTEXT_PARAM_DEBUG_NAME`.
const CONTEXT_PARAM_DEBUG_NAME: u64 = 4;
/// The most rings of a context (Linux's `VIRTGPU_MAX_RINGS`... 64).
const MAX_RINGS: u64 = 64;
/// The largest command stream of one `EXECBUFFER` or blob creation. Linux
/// has no limit but memory; Mesa 21.3 sends at most 266,240 bytes.
const MAX_COMMAND: u32 = gpu::MAX_DATA as u32;
/// How long `WAIT` waits (Linux: 15 seconds, then `EBUSY`).
const WAIT_TIMEOUT: Duration = Duration::from_secs(15);
/// The `EXECBUFFER` flags this driver knows.
const EXECBUF_FLAGS: u32 =
    uapi::VIRTGPU_EXECBUF_FENCE_FD_IN | uapi::VIRTGPU_EXECBUF_FENCE_FD_OUT | uapi::VIRTGPU_EXECBUF_RING_IDX;
/// The offset of `fence_fd` in every layout of the `EXECBUFFER` argument.
const FENCE_FD_OFFSET: isize = 28;
const _: () = assert!(core::mem::offset_of!(uapi::drm_virtgpu_execbuffer, fence_fd) == FENCE_FD_OFFSET as usize);
const _: () = assert!(core::mem::offset_of!(uapi::drm_virtgpu_execbuffer_v1, fence_fd) == FENCE_FD_OFFSET as usize);
const _: () = assert!(core::mem::offset_of!(uapi::drm_virtgpu_execbuffer_v2, fence_fd) == FENCE_FD_OFFSET as usize);

/// `ENOSYS`, which `ddi::Errno` has no constant for.
const ENOSYS: Errno = match Errno::new(ddi::sys::errno::ENOSYS) {
    Some(e) => e,
    None => Errno::EINVAL,
};

fn put_all(v: KVec<GemRef<VgObject>>, ctx: &Sleepable) {
    let mut v = v;
    while let Some(r) = v.swap_remove(0) {
        r.put(ctx);
    }
}

impl VirtioGpu {
    fn dev(&self) -> &Dev {
        &self.virtio
    }

    /// `DRM_IOCTL_VIRTGPU_GETPARAM`: writes an `int` to the address in
    /// `value`, as Linux does.
    pub(crate) fn getparam(&self, arg: &IoctlArg<'_>) -> Result<(), Errno> {
        let a = UserArg::<uapi::drm_virtgpu_getparam>::read(arg)?;
        let caps = self.dev().state().caps();
        let v: i32 = match a.get().param {
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
        copy_out_bytes(arg, a.get().value as usize, &v.to_ne_bytes())
    }

    /// `DRM_IOCTL_VIRTGPU_RESOURCE_CREATE`.
    pub(crate) fn resource_create(
        &self,
        dev: &drm::Device<Self>,
        file: CloneId,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let mut a = UserArg::<uapi::drm_virtgpu_resource_create>::read(arg)?;
        let v = *a.get();
        let gem = dev.gem().ok_or(Errno::ENXIO)?;
        let gpu = self.dev().state();
        let how = if gpu.virgl() {
            gpu.ensure_context(self.dev(), file, ctx)?;
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
        let size = if v.size == 0 { drm::offsets::PAGE_SIZE as usize } else { v.size as usize };
        let obj = object::create(gem, self.dr, size, how, ctx)?;
        let res = obj.object().res();
        let handle = gem.handle_create(file, obj, ctx)?;
        // SAFETY: field assignments only.
        let m = unsafe { a.get_mut() };
        m.bo_handle = handle;
        m.res_handle = res;
        a.write(arg)
    }

    /// `DRM_IOCTL_VIRTGPU_RESOURCE_INFO`.
    pub(crate) fn resource_info(
        &self,
        dev: &drm::Device<Self>,
        file: CloneId,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let mut a = UserArg::<uapi::drm_virtgpu_resource_info>::read(arg)?;
        let obj = dev.gem().ok_or(Errno::ENXIO)?.lookup(file, a.get().bo_handle)?;
        let (res, size, blob) = (obj.object().res(), obj.size(), obj.object().blob_mem());
        obj.put(ctx);
        // SAFETY: field assignments only.
        let m = unsafe { a.get_mut() };
        m.res_handle = res;
        m.size = size as u32;
        m.blob_mem = blob;
        a.write(arg)
    }

    /// `DRM_IOCTL_VIRTGPU_MAP`: the object's mmap offset.
    pub(crate) fn map(
        &self,
        dev: &drm::Device<Self>,
        file: CloneId,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let mut a = UserArg::<uapi::drm_virtgpu_map>::read(arg)?;
        let gem = dev.gem().ok_or(Errno::ENXIO)?;
        let obj = gem.lookup(file, a.get().handle)?;
        let off = gem.mmap_offset(&obj, ctx);
        obj.put(ctx);
        // SAFETY: a field assignment.
        unsafe { a.get_mut() }.offset = off?;
        a.write(arg)
    }

    /// `DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST` (`to_host` false) and
    /// `TRANSFER_TO_HOST`, in the 44-byte layout (older 36-byte arguments
    /// are widened by the caller).
    #[allow(clippy::too_many_arguments, reason = "the fields of the transfer structure")]
    pub(crate) fn transfer(
        &self,
        dev: &drm::Device<Self>,
        file: CloneId,
        to_host: bool,
        t: uapi::drm_virtgpu_3d_transfer_to_host,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let gem = dev.gem().ok_or(Errno::ENXIO)?;
        let gpu = self.dev().state();
        let obj = gem.lookup(file, t.bo_handle)?;
        if !gpu.virgl() {
            if !to_host {
                obj.put(ctx);
                return Err(ENOSYS);
            }
            // 2D: copy the rectangle into the host resource, no fence.
            let req = proto::TransferToHost2d {
                hdr: CtrlHdr::new(proto::CMD_TRANSFER_TO_HOST_2D),
                r: proto::Rect { x: t.box_.x, y: t.box_.y, width: t.box_.w, height: t.box_.h },
                offset: u64::from(t.offset),
                resource_id: obj.object().res(),
                padding: 0,
            };
            let r = gpu.fire(self.dev(), &req, ctx).map(drop);
            obj.put(ctx);
            return r;
        }
        // A guest-only blob has no host storage to transfer to or from.
        if obj.object().kind() == (Kind::Blob { mem: proto::BLOB_MEM_GUEST }) {
            obj.put(ctx);
            return Err(Errno::EINVAL);
        }
        let ctx_id = match gpu.ensure_context(self.dev(), file, ctx) {
            Ok(id) => id,
            Err(e) => {
                obj.put(ctx);
                return Err(e);
            }
        };
        let req = proto::TransferHost3d {
            hdr: CtrlHdr::ctx(
                if to_host { proto::CMD_TRANSFER_TO_HOST_3D } else { proto::CMD_TRANSFER_FROM_HOST_3D },
                ctx_id,
            ),
            box_: proto::Box3d { x: t.box_.x, y: t.box_.y, z: t.box_.z, w: t.box_.w, h: t.box_.h, d: t.box_.d },
            offset: u64::from(t.offset),
            resource_id: obj.object().res(),
            level: t.level,
            stride: t.stride,
            layer_stride: t.layer_stride,
        };
        let fence = self.submit_with(file, &req, None, core::slice::from_ref(&obj), None, ctx);
        let r = match fence {
            Ok(f) => {
                let r = add_fence(&obj, &f, ctx);
                f.put(Ctx::Sleep(ctx));
                r
            }
            Err(e) => Err(e),
        };
        obj.put(ctx);
        r
    }

    /// Sends `req` (with `data` as its extra data, from user memory) as a
    /// fenced request of `file` that holds `objs` until the device has
    /// completed it, and returns its fence.
    fn submit_with<T: proto::Wire>(
        &self,
        file: CloneId,
        req: &T,
        data: Option<(&IoctlArg<'_>, u64, u32)>,
        objs: &[GemRef<VgObject>],
        ring: Option<u32>,
        ctx: &Sleepable,
    ) -> Result<Fence, Errno> {
        let gpu = self.dev().state();
        let data_len = data.map_or(0, |(_, _, n)| n as usize);
        let c = gpu.cmd(self.dev(), req, size_of::<CtrlHdr>(), data_len, Q::Ctrl, ctx)?;
        if let Some((arg, addr, n)) = data
            && n > 0
        {
            let d = c.data().ok_or(Errno::EINVAL)?;
            // Straight from user memory into the DMA buffer.
            // SAFETY: the destination is `n` bytes of DMA memory the request
            // owns and nothing else accesses before it is submitted.
            let dst = unsafe { core::slice::from_raw_parts_mut(d.as_ptr().as_ptr(), n as usize) };
            copy_in_bytes(arg, addr as usize, dst)?;
        }
        let mut held = KVec::new();
        for o in objs {
            held.push(o.clone(), ctx)?;
        }
        c.hold(held);
        let fr = gpu.file_fence(file, ring)?;
        gpu.submit(self.dev(), &c, Some(fr), ctx)?.ok_or(Errno::EIO)
    }

    /// `DRM_IOCTL_VIRTGPU_WAIT`: waits for the object's readers and writers
    /// (15 s, then `EBUSY`), or only tests them with `VIRTGPU_WAIT_NOWAIT`.
    pub(crate) fn wait(
        &self,
        dev: &drm::Device<Self>,
        file: CloneId,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let a = UserArg::<uapi::drm_virtgpu_3d_wait>::read(arg)?;
        let obj = dev.gem().ok_or(Errno::ENXIO)?.lookup(file, a.get().handle)?;
        let r = if a.get().flags & uapi::VIRTGPU_WAIT_NOWAIT != 0 {
            if obj.resv().test_signaled(Usage::Read) { Ok(()) } else { Err(Errno::EBUSY) }
        } else {
            let deadline = HrTime::now().as_nanos().saturating_add(WAIT_TIMEOUT.as_nanos() as i64);
            match obj.resv().wait(Usage::Read, true, Some(deadline), ctx) {
                Err(e) if e == Errno::ETIME => Err(Errno::EBUSY),
                r => r,
            }
        };
        obj.put(ctx);
        r
    }

    /// `DRM_IOCTL_VIRTGPU_GET_CAPS`.
    pub(crate) fn get_caps(&self, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let a = UserArg::<uapi::drm_virtgpu_get_caps>::read(arg)?;
        let v = *a.get();
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
        copy_out_bytes(arg, v.addr as usize, data.get(..n).ok_or(Errno::EIO)?)
    }

    /// `DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB`: guest-memory blobs
    /// (`BLOB_MEM_GUEST`, `BLOB_MEM_HOST3D_GUEST`) only.
    pub(crate) fn create_blob(
        &self,
        dev: &drm::Device<Self>,
        file: CloneId,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let mut a = UserArg::<uapi::drm_virtgpu_resource_create_blob>::read(arg)?;
        let v = *a.get();
        let gem = dev.gem().ok_or(Errno::ENXIO)?;
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
                if !caps.virgl || v.cmd_size % 4 != 0 {
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
        let ctx_id = if caps.virgl { gpu.ensure_context(self.dev(), file, ctx)? } else { 0 };
        if v.cmd_size > 0 {
            let req = proto::CmdSubmit {
                hdr: CtrlHdr::ctx(proto::CMD_SUBMIT_3D, ctx_id),
                size: v.cmd_size,
                num_in_fences: 0,
            };
            let f = self.submit_with(file, &req, Some((arg, v.cmd, v.cmd_size)), &[], None, ctx)?;
            f.put(Ctx::Sleep(ctx));
        }
        let how = Create::Blob {
            mem: v.blob_mem,
            flags: v.blob_flags,
            blob_id: v.blob_id,
            ctx_id: if host3d { ctx_id } else { 0 },
        };
        let obj = object::create(gem, self.dr, size, how, ctx)?;
        let res = obj.object().res();
        let handle = gem.handle_create(file, obj, ctx)?;
        // SAFETY: field assignments only.
        let m = unsafe { a.get_mut() };
        m.bo_handle = handle;
        m.res_handle = res;
        a.write(arg)
    }

    /// `DRM_IOCTL_VIRTGPU_CONTEXT_INIT`.
    pub(crate) fn context_init(&self, file: CloneId, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let a = UserArg::<uapi::drm_virtgpu_context_init>::read(arg)?;
        let v = *a.get();
        let gpu = self.dev().state();
        let caps = gpu.caps();
        if !caps.context_init {
            return Err(Errno::EINVAL);
        }
        if v.num_params == 0 || v.num_params > 16 {
            return Err(Errno::EINVAL);
        }
        let params = copy_in_array::<uapi::drm_virtgpu_context_set_param>(
            arg,
            v.ctx_set_params as usize,
            v.num_params as usize,
            ctx,
        )?;
        let mut capset = 0u32;
        let mut rings = 0u32;
        let mut name: Vec<u8> = Vec::new();
        for p in params.iter() {
            match p.param {
                x if x == u64::from(uapi::VIRTGPU_CONTEXT_PARAM_CAPSET_ID) => {
                    if p.value >= 64 || caps.capset_mask() & (1 << p.value) == 0 || capset != 0 {
                        return Err(Errno::EINVAL);
                    }
                    capset = p.value as u32;
                }
                x if x == u64::from(uapi::VIRTGPU_CONTEXT_PARAM_NUM_RINGS) => {
                    if p.value > MAX_RINGS {
                        return Err(Errno::EINVAL);
                    }
                    rings = p.value as u32;
                }
                // Signalled-fence events on a polled ring need a driver event
                // queue per file, which the core has for mode setting only.
                x if x == u64::from(uapi::VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK) => {
                    if p.value != 0 {
                        return Err(Errno::EINVAL);
                    }
                }
                CONTEXT_PARAM_DEBUG_NAME => {
                    let mut b = [0u8; 64];
                    copy_in_bytes(arg, p.value as usize, &mut b[..63]).or_else(|_| {
                        // A shorter string at the end of a page: read it
                        // byte by byte up to its NUL.
                        read_cstr(arg, p.value as usize, &mut b[..63])
                    })?;
                    let n = b.iter().position(|&c| c == 0).unwrap_or(63);
                    name = b[..n].to_vec();
                }
                _ => return Err(Errno::EINVAL),
            }
        }
        let domain = gpu.domain()?;
        let r = gpu.with_file(file, |f| {
            if f.created {
                return Err(Errno::EEXIST);
            }
            f.capset = capset;
            f.num_rings = rings;
            if rings > 0 {
                f.ring_base = domain.alloc_contexts(u64::from(rings));
            }
            f.name = name;
            Ok(())
        });
        r?;
        gpu.ensure_context(self.dev(), file, ctx).map(drop)
    }

    /// `DRM_IOCTL_VIRTGPU_EXECBUFFER`, in any of its three layouts (`size`
    /// bytes of the argument were copied into `x`, the rest is zero).
    pub(crate) fn execbuffer(
        &self,
        dev: &drm::Device<Self>,
        file: CloneId,
        x: uapi::drm_virtgpu_execbuffer,
        arg: &IoctlArg<'_>,
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
        gpu.file_fence(file, ring)?;
        let sync = dev.sync();
        let gem = dev.gem().ok_or(Errno::ENXIO)?;
        if (x.num_in_syncobjs != 0 || x.num_out_syncobjs != 0)
            && x.syncobj_stride as usize != size_of::<uapi::drm_virtgpu_execbuffer_syncobj>()
        {
            return Err(Errno::EINVAL);
        }
        let ins = copy_in_array::<uapi::drm_virtgpu_execbuffer_syncobj>(
            arg,
            x.in_syncobjs as usize,
            x.num_in_syncobjs as usize,
            ctx,
        )?;
        let outs = copy_in_array::<uapi::drm_virtgpu_execbuffer_syncobj>(
            arg,
            x.out_syncobjs as usize,
            x.num_out_syncobjs as usize,
            ctx,
        )?;
        if ins.iter().chain(outs.iter()).any(|s| s.flags & !uapi::VIRTGPU_EXECBUF_SYNCOBJ_RESET != 0) {
            return Err(Errno::EINVAL);
        }
        // Everything that can fail on the caller's input is looked up before
        // the submission.
        let mut out_objs: Vec<(Syncobj, u64)> = Vec::new();
        for s in outs.iter() {
            out_objs.push((sync.syncobj(file, s.handle)?, s.point));
        }
        let handles = copy_in_array::<u32>(arg, x.bo_handles as usize, x.num_bo_handles as usize, ctx)?;
        let mut bos = KVec::new();
        for &h in handles.iter() {
            match gem.lookup(file, h) {
                Ok(o) => {
                    if let Err(e) = bos.push(o, ctx) {
                        put_all(bos, ctx);
                        return Err(e);
                    }
                }
                Err(e) => {
                    put_all(bos, ctx);
                    return Err(e);
                }
            }
        }
        let r = self.execbuffer_locked(dev, file, &x, ring, &ins, &out_objs, &bos, arg, ctx);
        put_all(bos, ctx);
        for (s, _) in out_objs {
            drop(s);
        }
        r
    }

    #[allow(clippy::too_many_arguments, reason = "the parsed parts of one EXECBUFFER")]
    fn execbuffer_locked(
        &self,
        dev: &drm::Device<Self>,
        file: CloneId,
        x: &uapi::drm_virtgpu_execbuffer,
        ring: Option<u32>,
        ins: &KVec<uapi::drm_virtgpu_execbuffer_syncobj>,
        outs: &[(Syncobj, u64)],
        bos: &KVec<GemRef<VgObject>>,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let gpu = self.dev().state();
        let sync = dev.sync();
        // In-fences: the device orders its own fences; others are waited
        // for here, interruptibly, as Linux does.
        if x.flags & uapi::VIRTGPU_EXECBUF_FENCE_FD_IN != 0 {
            let f = sync.sync_file_fence(x.fence_fd, arg, ctx)?;
            let r = self.wait_foreign(file, &f, ctx);
            f.put(Ctx::Sleep(ctx));
            r?;
        }
        let mut resets = Vec::new();
        for s in ins.iter() {
            let so = sync.syncobj(file, s.handle)?;
            let f = so.find_fence(s.point, false, ctx)?;
            let r = self.wait_foreign(file, &f, ctx);
            f.put(Ctx::Sleep(ctx));
            r?;
            if s.flags & uapi::VIRTGPU_EXECBUF_SYNCOBJ_RESET != 0 {
                resets.push(so);
            }
        }
        let ctx_id = gpu.ensure_context(self.dev(), file, ctx)?;
        let req = proto::CmdSubmit { hdr: CtrlHdr::ctx(proto::CMD_SUBMIT_3D, ctx_id), size: x.size, num_in_fences: 0 };
        let fence = self.submit_with(file, &req, Some((arg, x.command, x.size)), bos, ring, ctx)?;
        // The work is queued; what follows only publishes its fence.
        let mut r = Ok(());
        for b in bos.iter() {
            if let Err(e) = add_fence(b, &fence, ctx) {
                r = Err(e);
            }
        }
        for (so, point) in outs {
            if let Err(e) = so.add_point(fence.clone(), *point, ctx) {
                r = Err(e);
            }
        }
        for so in resets {
            so.replace_fence(None, ctx);
        }
        if r.is_ok() && x.flags & uapi::VIRTGPU_EXECBUF_FENCE_FD_OUT != 0 {
            r = sync.export_sync_file(
                fence.clone(),
                b"virtio_gpu",
                arg,
                |fd| copy_out_bytes(arg, (arg.raw() + FENCE_FD_OFFSET) as usize, &fd.to_ne_bytes()),
                ctx,
            );
        }
        fence.put(Ctx::Sleep(ctx));
        r
    }

    /// Waits for fence `f` unless it is one of the device's own contexts,
    /// whose order the device keeps.
    fn wait_foreign(&self, file: CloneId, f: &Fence, ctx: &Sleepable) -> Result<(), Errno> {
        if self.dev().state().own_context(file, f.context()) {
            return Ok(());
        }
        f.wait(true, None, ctx)
    }
}

/// Adds `f` to the reservation of `obj` as a writer.
fn add_fence(obj: &GemRef<VgObject>, f: &Fence, ctx: &Sleepable) -> Result<(), Errno> {
    let g = obj.resv().lock().lock_plain(ctx);
    obj.resv().add_fence(&g, f.clone(), Usage::Write, ctx)
}

/// Reads a NUL-terminated string of at most `out.len()` bytes from user
/// address `addr`.
fn read_cstr(arg: &IoctlArg<'_>, addr: usize, out: &mut [u8]) -> Result<(), Errno> {
    for (i, b) in out.iter_mut().enumerate() {
        let mut c = [0u8];
        copy_in_bytes(arg, addr + i, &mut c)?;
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

/// Reads the `EXECBUFFER` argument of command `cmd`, zero-extended to the
/// current layout.
pub(crate) fn read_execbuffer(cmd: u32, arg: &IoctlArg<'_>) -> Result<uapi::drm_virtgpu_execbuffer, Errno> {
    let mut x = uapi::drm_virtgpu_execbuffer::default();
    match cmd {
        uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER => x = *UserArg::<uapi::drm_virtgpu_execbuffer>::read(arg)?.get(),
        uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER_V2 => {
            let v = *UserArg::<uapi::drm_virtgpu_execbuffer_v2>::read(arg)?.get();
            x.flags = v.flags;
            x.size = v.size;
            x.command = v.command;
            x.bo_handles = v.bo_handles;
            x.num_bo_handles = v.num_bo_handles;
            x.fence_fd = v.fence_fd;
            x.ring_idx = v.ring_idx;
        }
        uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER_V1 => {
            let v = *UserArg::<uapi::drm_virtgpu_execbuffer_v1>::read(arg)?.get();
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
