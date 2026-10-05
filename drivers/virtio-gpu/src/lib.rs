// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! A DRM driver for the virtio GPU device (virtio 1.2, section 5.7; PCI
//! `1af4:1050`), the milestone R4 of the illumos DRM core.
//!
//! The device is driven through the gate's virtio framework, wrapped by
//! illumos-ddi's `ddi::private::virtio` (the private tier, gate
//! `cda5b9a49f`): features `VIRTIO_GPU_F_VIRGL`, `F_EDID`,
//! `F_RESOURCE_BLOB` and `F_CONTEXT_INIT` are negotiated when offered, the
//! control queue carries every request and the cursor queue the cursor, and
//! completions run in thread context on the framework's task queue
//! ([`gpu`]).
//!
//! What userland sees:
//!
//! * The DRM device `virtio_gpu`, version 0.1.0 (Mesa's virgl winsys
//!   requires major 0), with a card and a render node, GEM objects in
//!   locked system memory whose DMA cookies are the resources' backing
//!   ([`object`]), dumb buffers as 2D resources, PRIME, and sync objects
//!   with timelines.
//! * The `DRM_IOCTL_VIRTGPU_*` ioctls of Linux 6.12 ([`ioctl`]): `MAP`,
//!   `EXECBUFFER` (in the 32-, 40- and 64-byte layouts, with in and out
//!   fence descriptors and sync object arrays), `GETPARAM`,
//!   `RESOURCE_CREATE`, `RESOURCE_INFO`, `TRANSFER_FROM_HOST`,
//!   `TRANSFER_TO_HOST` (also in the 36-byte layout of libdrm 2.4.109),
//!   `WAIT`, `GET_CAPS`, `RESOURCE_CREATE_BLOB` and `CONTEXT_INIT`.
//! * Mode setting ([`kms`]): a connector, encoder, CRTC, primary and
//!   cursor plane per scanout, with EDID when the device has one, atomic
//!   commits, page-flip events from a software vblank, and `DIRTYFB`.
//!
//! Deviations from Linux: host-visible memory (`HOST_VISIBLE`, blobs in
//! `BLOB_MEM_HOST3D`) needs the shared memory regions of the virtio PCI
//! transport, which the gate's framework does not implement, so it is not
//! offered; neither are resource UUIDs (`CROSS_DEVICE`) or polled rings
//! (`VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK` other than 0), whose fence
//! events would need a per-file event queue outside mode setting. An
//! `EXECBUFFER` command stream is limited to 1 MiB. A context is named
//! after the process ID, since the DDI has no process name.
#![no_std]

extern crate alloc;

pub mod gpu;
pub mod ioctl;
pub mod kms;
pub mod object;
pub mod proto;
#[cfg(test)]
mod tests;

use core::ffi::CStr;
use core::fmt;
use core::num::NonZeroU32;

use ddi::DmaAttr;
use ddi::prelude::*;
use ddi::private::virtio::{Features as VirtioFeatures, Virtio, VirtioError, VirtioSetup};
use drm::dumb::{DumbLayout, DumbRequest};
use drm::gem::{GemRef, SysMem};
use drm::ioctl::DriverIoctl;
use drm::minor::CloneId;
use drm::state::Perm;
use drm::{BusId, Features, ModeConfig};
use drm_uapi as uapi;

use crate::gpu::{Caps, CmdRef, Gpu, MAX_SEGS, dev_note, dev_warn};
use crate::kms::GpuKms;
use crate::object::{Create, DevRef, VgObject};

// The module linkage and its _init, _fini and _info entry points. Not in
// host unit tests: a test binary is a host executable whose C runtime
// (crti.o on Linux) already defines _init and _fini.
#[cfg(not(test))]
ddi::module! {
    driver: VirtioGpuModule,
    ops: [char, devmap],
}

/// The kernel module.
pub struct VirtioGpuModule;

/// The largest GEM object: 256 MiB, which an 8192 by 8192 scanout of 32-bit
/// pixels needs; its memory entries (one per page at worst) fit in the
/// 1 MiB of extra data of one request.
pub const MAX_OBJECT: usize = 256 << 20;

/// DMA attributes of object memory: any address, page-aligned, any number
/// of cookies (each becomes a memory entry).
const OBJECT_DMA: DmaAttr = DmaAttr::bits64().unlimited_cookies();

/// All of the virtio-gpu ioctls: render nodes allowed, no authentication,
/// as Linux declares them.
const RENDER: Perm = Perm { auth: false, master: false, root_only: false, render_allow: true };

/// The DRM driver of one device.
pub struct VirtioGpu {
    /// The display; dropped first, it stops its vblank sources.
    kms: GpuKms,
    dr: DevRef,
    pci: Option<(u8, u8, u8)>,
    instance: u32,
    /// The device; dropped last (after every object, which the core
    /// guarantees), it resets the device and frees the queues.
    virtio: Virtio<Gpu>,
}

impl VirtioGpu {
    /// Brings the device up: negotiates features, allocates the queues,
    /// starts the framework and asks the device for its capability sets.
    fn probe(node: &OwnNode, ctx: &Sleepable) -> Result<VirtioGpu, DdiError> {
        let instance = u32::try_from(node.instance().0).map_err(|_| DdiError)?;
        let fail = |what: &str, e: VirtioError| {
            dev_warn(instance, format_args!("{what}: {e:?}"));
            DdiError
        };
        let setup = VirtioSetup::<Gpu>::init(node, ctx).map_err(|e| fail("virtio_init", e))?;
        let offered = setup.device_features();
        let want = [proto::F_VIRGL, proto::F_EDID, proto::F_RESOURCE_BLOB, proto::F_CONTEXT_INIT]
            .into_iter()
            .fold(VirtioFeatures(0), |f, b| f.union(VirtioFeatures::bit(b)));
        let mut neg = setup.negotiate(want, true).map_err(|e| fail("feature negotiation", e))?;
        let f = neg.features();
        let (scanouts, ncapsets) = {
            let cfg = neg.config();
            let s = cfg.get32(proto::CFG_NUM_SCANOUTS).map_err(|e| fail("num_scanouts", e))?;
            let c = cfg.get32(proto::CFG_NUM_CAPSETS).map_err(|e| fail("num_capsets", e))?;
            (s, c)
        };
        if scanouts == 0 || scanouts as usize > proto::MAX_SCANOUTS {
            dev_warn(instance, format_args!("the device reports {scanouts} scanouts"));
            return Err(DdiError);
        }
        let ctrl = neg
            .alloc_queue::<CmdRef>(proto::CONTROLQ, c"control", MAX_SEGS, false, ctx)
            .map_err(|e| fail("control queue", e))?;
        let cursor = neg
            .alloc_queue::<CmdRef>(proto::CURSORQ, c"cursor", MAX_SEGS, false, ctx)
            .map_err(|e| fail("cursor queue", e))?;
        let caps = Caps {
            virgl: f.has(proto::F_VIRGL),
            edid: f.has(proto::F_EDID),
            blob: f.has(proto::F_RESOURCE_BLOB),
            context_init: f.has(proto::F_CONTEXT_INIT),
            scanouts,
            capsets: alloc::vec::Vec::new(),
        };
        let virtio: Virtio<Gpu> = neg
            .complete(&[], |_view, _pri| Ok::<Gpu, VirtioError>(Gpu::new(ctrl, cursor, caps, instance)), ctx)
            .map_err(|e| fail("virtio_init_complete", e))?;
        let gpu = virtio.state();
        if ncapsets > 0 {
            match gpu.capset_infos(&virtio, ncapsets, ctx) {
                Ok(v) => gpu.caps.lock().capsets = v,
                Err(e) => dev_warn(instance, format_args!("GET_CAPSET_INFO: {e:?}")),
            }
        }
        // SAFETY: the device lives in the Virtio's allocation, which the
        // VirtioGpu keeps until it is dropped at detach, after every object
        // and the display are gone.
        let dr = unsafe { DevRef::new(&virtio) };
        let caps = gpu.caps();
        dev_note(
            instance,
            format_args!(
                "virtio-gpu: {scanouts} scanout(s), features {:#x} of {:#x} (virgl {}, edid {}, blob {}, context_init {}), \
                 {} capset(s) {:#x}, {} interface",
                f.0,
                offered.0,
                caps.virgl,
                caps.edid,
                caps.blob,
                caps.context_init,
                caps.capsets.len(),
                caps.capset_mask(),
                if virtio.is_modern() { "modern" } else { "legacy" }
            ),
        );
        let kms = GpuKms::new(dr, scanouts as usize);
        let pci = pci_location(node, ctx);
        Ok(VirtioGpu { kms, dr, pci, instance, virtio })
    }

    fn dispatch(
        &self,
        dev: &drm::Device<Self>,
        file: CloneId,
        cmd: u32,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        match cmd {
            uapi::DRM_IOCTL_VIRTGPU_GETPARAM => self.getparam(arg),
            uapi::DRM_IOCTL_VIRTGPU_RESOURCE_CREATE => self.resource_create(dev, file, arg, ctx),
            uapi::DRM_IOCTL_VIRTGPU_RESOURCE_INFO => self.resource_info(dev, file, arg, ctx),
            uapi::DRM_IOCTL_VIRTGPU_MAP => self.map(dev, file, arg, ctx),
            uapi::DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST | uapi::DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST => {
                let t = *drm::uaccess::UserArg::<uapi::drm_virtgpu_3d_transfer_to_host>::read(arg)?.get();
                self.transfer(dev, file, cmd == uapi::DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, t, ctx)
            }
            uapi::DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST_V1 | uapi::DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST_V1 => {
                let t = *drm::uaccess::UserArg::<uapi::drm_virtgpu_3d_transfer_to_host_v1>::read(arg)?.get();
                self.transfer(
                    dev,
                    file,
                    cmd == uapi::DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST_V1,
                    ioctl::widen_transfer(&t),
                    ctx,
                )
            }
            uapi::DRM_IOCTL_VIRTGPU_WAIT => self.wait(dev, file, arg, ctx),
            uapi::DRM_IOCTL_VIRTGPU_GET_CAPS => self.get_caps(arg, ctx),
            uapi::DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB => self.create_blob(dev, file, arg, ctx),
            uapi::DRM_IOCTL_VIRTGPU_CONTEXT_INIT => self.context_init(dev, file, arg, ctx),
            uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER
            | uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER_V1
            | uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER_V2 => {
                let x = ioctl::read_execbuffer(cmd, arg)?;
                self.execbuffer(dev, file, x, arg, ctx)
            }
            _ => Err(Errno::ENOTTY),
        }
    }
}

/// The PCI bus, device and function of `node`, from the first cell of its
/// `reg` property (IEEE 1275 PCI binding: `npt000ss bbbbbbbb dddddfff
/// rrrrrrrr`).
fn pci_location(node: &OwnNode, ctx: &Sleepable) -> Option<(u8, u8, u8)> {
    let reg = node.props().int_array(c"reg", ctx).ok().flatten()?;
    let hi = *reg.first()? as u32;
    Some(((hi >> 16) as u8, ((hi >> 11) & 0x1f) as u8, ((hi >> 8) & 7) as u8))
}

impl drm::Driver for VirtioGpu {
    const NAME: &'static CStr = c"virtio_gpu";
    const DATE: &'static CStr = c"0";
    const DESC: &'static CStr = c"virtio GPU";
    // Linux's virtio-gpu reports 0.1.0, and Mesa's virgl winsys refuses any
    // other major version.
    const MAJOR: i32 = 0;
    const MINOR: i32 = 1;
    const PATCHLEVEL: i32 = 0;
    const FEATURES: Features = Features::MODESET
        .union(Features::ATOMIC)
        .union(Features::RENDER)
        .union(Features::GEM)
        .union(Features::DUMB)
        .union(Features::SYNCOBJ)
        .union(Features::SYNCOBJ_TIMELINE);
    const MODE_CONFIG: ModeConfig = ModeConfig {
        cursor_width: kms::CURSOR_MAX as u64,
        cursor_height: kms::CURSOR_MAX as u64,
        preferred_depth: 24,
        prefer_shadow: false,
        fb_modifiers: false,
    };
    const DMA_ATTR: Option<DmaAttr> = Some(OBJECT_DMA);
    const MAX_OBJECT_SIZE: usize = MAX_OBJECT;
    const IOCTLS: &'static [DriverIoctl] = &[
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_MAP, perm: RENDER },
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER, perm: RENDER },
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER_V1, perm: RENDER },
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_EXECBUFFER_V2, perm: RENDER },
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_GETPARAM, perm: RENDER },
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, perm: RENDER },
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_RESOURCE_INFO, perm: RENDER },
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, perm: RENDER },
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST_V1, perm: RENDER },
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, perm: RENDER },
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST_V1, perm: RENDER },
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_WAIT, perm: RENDER },
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_GET_CAPS, perm: RENDER },
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, perm: RENDER },
        DriverIoctl { cmd: uapi::DRM_IOCTL_VIRTGPU_CONTEXT_INIT, perm: RENDER },
    ];
    type Object = VgObject;

    fn bus_id(&self, dev: &DevInfo, out: &mut BusId) -> fmt::Result {
        match self.pci {
            Some((b, d, f)) => fmt::Write::write_fmt(out, format_args!("pci:0000:{b:02x}:{d:02x}.{f}")),
            None => fmt::Write::write_fmt(out, format_args!("virtio_gpu@{}", dev.instance().0)),
        }
    }

    /// Only 32 bits per pixel, and rows exactly `width` pixels long: the
    /// device copies 2D resources with their own stride.
    fn dumb_layout(&self, req: &DumbRequest) -> Result<DumbLayout, Errno> {
        if req.bpp != 32 {
            return Err(Errno::EINVAL);
        }
        drm::dumb::layout(req, 4, 1)
    }

    /// A dumb buffer is a 2D resource in XRGB8888.
    fn dumb_object(
        &self,
        _dev: &drm::Device<Self>,
        mem: SysMem,
        req: &DumbRequest,
        _layout: &DumbLayout,
        ctx: &Sleepable,
    ) -> Result<VgObject, Errno> {
        let how = Create::Res2d { format: proto::FORMAT_B8G8R8X8_UNORM, width: req.width, height: req.height };
        object::with_resource(self.dr, mem, how, ctx)
    }

    fn ioctl(
        &self,
        dev: &drm::Device<Self>,
        file: CloneId,
        cmd: u32,
        arg: &IoctlArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        self.dispatch(dev, file, cmd, arg, ctx)
    }

    fn kms(&self) -> Option<&dyn drm::kms::KmsDriver<GemRef<VgObject>>> {
        Some(&self.kms)
    }

    fn file_close(&self, _dev: &drm::Device<Self>, file: CloneId, ctx: &Sleepable) {
        self.virtio.state().file_close(&self.virtio, file, ctx);
    }

    fn before_detach(&self, ctx: &Sleepable) {
        // Complete whatever the device has returned, so that the objects
        // those requests held are released before the core counts them.
        let gpu = self.virtio.state();
        gpu.drain(&self.virtio, gpu::Q::Ctrl, ctx);
        gpu.drain(&self.virtio, gpu::Q::Cursor, ctx);
    }

    fn before_teardown(&self, ctx: &Sleepable) {
        self.kms.shutdown(ctx);
        let s = &self.virtio.state().stats;
        use core::sync::atomic::Ordering::Relaxed;
        dev_note(
            self.instance,
            format_args!(
                "detach: {} requests ({} fenced, {} failed, {} queue-full waits, {} timeouts), {} resources created, {} left",
                s.submitted.load(Relaxed),
                s.fenced.load(Relaxed),
                s.errors.load(Relaxed),
                s.queue_full.load(Relaxed),
                s.timeouts.load(Relaxed),
                s.resources.load(Relaxed),
                s.live_resources.load(Relaxed),
            ),
        );
    }
}

impl Driver for VirtioGpuModule {
    type Instance = drm::Device<VirtioGpu>;
    const LINKINFO: &'static CStr = c"virtio-gpu DRM driver (illumos-drm R4)";

    fn attach(node: OwnNode, ctx: &Sleepable) -> Result<Self::Instance, DdiError> {
        let gpu = VirtioGpu::probe(&node, ctx)?;
        let dev = drm::Device::attach(node, gpu, ctx)?;
        dev.driver().virtio.state().set_fence_context(dev.sync().domain());
        if let Some(k) = dev.kms() {
            dev.driver().kms.set_vblank(k.vblank_handle());
        }
        Ok(dev)
    }

    fn detach(inst: Self::Instance, _dev: DevInfo, ctx: &Sleepable) -> Result<(), (Self::Instance, DdiError)> {
        inst.detach(ctx)
    }

    fn quiesce(inst: &Self::Instance, _dev: DevInfo) -> Result<(), DdiError> {
        inst.driver().virtio.quiesce();
        Ok(())
    }

    fn instance_of_minor(minor: Minor) -> Option<Instance> {
        drm::minor::instance_of_minor(minor)
    }
}

const _: NonZeroU32 = MAX_SEGS;
