// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The device: requests to the control and cursor queues, their
//! completion, fences, capability sets, display information and the
//! rendering contexts of files.
//!
//! # Requests
//!
//! A request ([`Cmd`]) lives in one piece of DMA memory: the request
//! structure at offset 0 and room for the response after it. Requests that
//! carry more (the memory entries of `ATTACH_BACKING`, the command stream of
//! `SUBMIT_3D`) have a second DMA buffer. The chain is the request (device
//! reads), the extra data (device reads) and the response (device writes);
//! cursor requests have no response. The queue owns a counted reference to
//! the request while it is in flight ([`CmdRef`], a `ChainData`), so its
//! memory lives until the device is done with it.
//!
//! Completions run in thread context: on the virtio framework's task queue
//! after each queue interrupt (`VirtioHandler::queue_work`), or on a
//! thread that waits for a request and polls the queue itself. Completing a
//! request records the response type, wakes its waiters, signals its fence
//! and drops the GEM references it held. Those are dropped through the
//! interrupt-safe path, which parks the last reference for the GEM task
//! queue: destroying an object waits for `RESOURCE_UNREF`, and that must not
//! happen on the thread that completes requests.
//!
//! # Fences
//!
//! A fenced request carries `VIRTIO_GPU_FLAG_FENCE` and a fence id, and the
//! device answers it only when the work is done (for virgl, when the host
//! renderer signalled the fence). Fence ids are taken and requests queued
//! under one mutex, so the ids of a fence context reach the device in
//! order, and the device completes the fences of one context (its own, or
//! one ring of a context) in that order; nothing orders two contexts, so
//! an in-fence of another context is waited for before a submission. The request's
//! completion signals its [`Fence`], on the device's fence context or on a
//! ring context of the file (`CONTEXT_INIT` with rings).

//!
//! # Locks
//!
//! `submit` orders fence ids with queueing; it is held while a request is
//! queued (and, when the queue is full, while completions are drained and
//! the submitter sleeps a tick), never while a completion is waited for.
//! `ctx_lock` serialises context creation and is held across the
//! `CTX_CREATE` round trip; it is taken before `files`. `files`, `caps`,
//! `capset_cache` and a request's `state`, `fence` and `objs` mutexes are
//! leaves. Completion takes only leaves, and the fence's own locks when it
//! signals; it never takes `submit` or `ctx_lock`, so a thread holding
//! either may drain the queue itself. The DRM core's mode-setting lock is
//! held around the display's requests (outside every lock here), and GEM
//! objects are destroyed with no GEM lock held. The virtio framework's
//! mutexes are internal leaves. The threaded part of `drm_r4_test` (eight
//! threads, own and shared files) exercises these paths together in the
//! kernel.

use alloc::collections::BTreeMap;
use alloc::vec::Vec;
use core::mem::size_of;
use core::num::NonZeroU32;
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use core::time::Duration;

use ddi::prelude::*;
use ddi::private::virtio::{
    Chain, ChainData, Direction, QueueId, VirtioConfig, VirtioDev, VirtioDma, VirtioError, VirtioHandler,
};
use drm::defer::Ctx;
use drm::fence::{Fence, FenceDomain};
use drm::gem::GemRef;
use drm::karc::KArc;
use drm::kvec::KVec;
use drm::minor::CloneId;

use crate::object::VgObject;
use crate::proto::{self, CtrlHdr, Wire};

/// The device-specific configuration of virtio-gpu, `struct
/// virtio_gpu_config`.
pub struct GpuConfig;

// SAFETY: every virtio-gpu device has the 16-byte configuration of the
// specification (section 5.7.4), which has not grown since virtio 1.0.
unsafe impl VirtioConfig for GpuConfig {
    const SIZE: usize = proto::CFG_SIZE;
}

/// DMA attributes of request buffers: one cookie (they are small).
const CMD_ATTR: DmaAttr = DmaAttr::bits64();
/// The most DMA cookies of a request's extra data.
pub const DATA_COOKIES: u32 = 128;
/// DMA attributes of the extra data of a request.
const DATA_ATTR: DmaAttr = DmaAttr::bits64().max_cookies(DATA_COOKIES);
/// Descriptors per chain: the request, the data, the response.
pub const MAX_SEGS: NonZeroU32 = match NonZeroU32::new(DATA_COOKIES + 2) {
    Some(n) => n,
    None => NonZeroU32::MIN,
};
/// The largest extra data of one request: `SUBMIT_3D` streams (Mesa 21.3
/// sends at most 266,240 bytes) and memory entry lists (8 MiB of entries
/// describe a 2 GiB object in single pages).
pub const MAX_DATA: usize = 1 << 20;
/// How long an internal request may take before the device counts as
/// hung.
const REQUEST_TIMEOUT: Duration = Duration::from_secs(10);
/// How long a submission retries while the queue is full.
const QUEUE_FULL_TIMEOUT: Duration = Duration::from_secs(2);

/// Where a request goes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Q {
    /// The control queue.
    Ctrl,
    /// The cursor queue.
    Cursor,
}

/// The fence a request signals.
#[derive(Clone, Copy, Debug)]
pub struct FenceReq {
    /// The fence context.
    pub context: u64,
    /// The context's ring, for `VIRTIO_GPU_FLAG_INFO_RING_IDX`.
    pub ring: Option<u8>,
}

#[derive(Default)]
struct CmdState {
    done: bool,
    resp: u32,
}

/// One request and its response.
pub struct Cmd {
    buf: VirtioDma,
    req_len: usize,
    resp_off: usize,
    resp_len: usize,
    data: Option<VirtioDma>,
    data_len: usize,
    queue: Q,
    state: Mutex<CmdState>,
    cv: CondVar,
    fence: Mutex<Option<Fence>>,
    /// GEM objects the device uses for this request.
    objs: Mutex<KVec<GemRef<VgObject>>>,
}

impl Drop for Cmd {
    /// A request is dropped after its completion took the fence, or when
    /// its submission failed (which signals it). A fenced request still in
    /// flight at detach would be evacuated with its fence unsignalled, but
    /// the DRM core refuses the detach while any fence of the device is
    /// alive, and the request holds one; so this only reports a broken
    /// invariant.
    fn drop(&mut self) {
        if let Some(f) = self.fence.get_mut().take() {
            ddi::kwarn!("virtio_gpu: a fenced request was dropped unsignalled (seqno {})", f.seqno());
            drop(f);
        }
    }
}

impl Cmd {
    /// The extra data buffer.
    pub fn data(&self) -> Option<&VirtioDma> {
        self.data.as_ref()
    }

    /// Copies the response (after completion) into `out`, up to its length.
    pub fn response(&self, out: &mut [u8]) -> Result<(), Errno> {
        let n = out.len().min(self.resp_len);
        let dst = out.get_mut(..n).ok_or(Errno::EINVAL)?;
        self.buf.read(self.resp_off, dst).map_err(Errno::from)
    }

    /// Reads the response as a wire structure.
    pub fn response_as<T: Wire + Default>(&self) -> Result<T, Errno> {
        let mut b = [0u8; 512];
        let n = size_of::<T>();
        let dst = b.get_mut(..n).ok_or(Errno::EINVAL)?;
        self.buf.read(self.resp_off, dst)?;
        proto::read(dst).ok_or(Errno::EIO)
    }

    /// Reads `out.len()` bytes of the response from `off` on.
    pub fn response_at(&self, off: usize, out: &mut [u8]) -> Result<(), Errno> {
        if off.checked_add(out.len()).is_none_or(|e| e > self.resp_len) {
            return Err(Errno::EINVAL);
        }
        self.buf.read(self.resp_off + off, out).map_err(Errno::from)
    }

    /// Hands GEM references to the request, which drops them when the
    /// device has completed it.
    pub fn hold(&self, objs: KVec<GemRef<VgObject>>) {
        let old = core::mem::replace(&mut *self.objs.lock(), objs);
        drop(old);
    }

    /// The response type once complete, `None` before.
    pub fn result(&self) -> Option<u32> {
        let s = self.state.lock();
        s.done.then_some(s.resp)
    }
}

/// A request owned by a queue while in flight.
pub struct CmdRef(pub KArc<Cmd>);

impl core::ops::Deref for CmdRef {
    type Target = Cmd;
    fn deref(&self) -> &Cmd {
        &self.0
    }
}

// SAFETY: a KArc keeps its value allocated at a fixed address for as long
// as a reference exists, and into_raw/from_raw round-trip it.
unsafe impl ChainData for CmdRef {
    fn into_raw(this: Self) -> core::ptr::NonNull<core::ffi::c_void> {
        KArc::into_raw(this.0).cast()
    }
    unsafe fn from_raw(p: core::ptr::NonNull<core::ffi::c_void>) -> Self {
        // SAFETY: the caller's contract; the pointer came from into_raw.
        CmdRef(unsafe { KArc::from_raw(p.cast()) })
    }
}

/// A capability set the device offers.
#[derive(Clone, Copy, Debug, Default)]
pub struct CapsetInfo {
    /// Its id (`CAPSET_*`).
    pub id: u32,
    /// Its highest version.
    pub max_version: u32,
    /// Its largest size.
    pub max_size: u32,
}

/// What the device can do, fixed at attach.
#[derive(Clone, Debug, Default)]
pub struct Caps {
    /// `VIRTIO_GPU_F_VIRGL`.
    pub virgl: bool,
    /// `VIRTIO_GPU_F_EDID`.
    pub edid: bool,
    /// `VIRTIO_GPU_F_RESOURCE_BLOB`.
    pub blob: bool,
    /// `VIRTIO_GPU_F_CONTEXT_INIT`.
    pub context_init: bool,
    /// The number of scanouts, 1 to 16.
    pub scanouts: u32,
    /// The capability sets.
    pub capsets: Vec<CapsetInfo>,
}

impl Caps {
    /// `VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs`: a bit per capability set id.
    pub fn capset_mask(&self) -> u64 {
        self.capsets.iter().filter(|c| c.id < 64).fold(0, |m, c| m | 1 << c.id)
    }
}

/// The rendering context of a file (created on first use).
#[derive(Default)]
pub struct FileCtx {
    /// `CTX_CREATE` was sent.
    pub created: bool,
    /// `CONTEXT_INIT`: the capability set (0: the device's default).
    pub capset: u32,
    /// `CONTEXT_INIT`: the number of rings.
    pub num_rings: u32,
    /// The fence context of ring 0; ring `i` is `ring_base + i`.
    pub ring_base: u64,
    /// `CONTEXT_INIT`: the debug name.
    pub name: Vec<u8>,
}

/// The parameters of `CONTEXT_INIT`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ContextParams {
    /// `VIRTGPU_CONTEXT_PARAM_CAPSET_ID`, 0 if not given.
    pub capset: u32,
    /// `VIRTGPU_CONTEXT_PARAM_NUM_RINGS`.
    pub rings: u32,
    /// `VIRTGPU_CONTEXT_PARAM_DEBUG_NAME`, without its NUL.
    pub name: Vec<u8>,
}

/// One parameter of `CONTEXT_INIT` as userland sent it; the debug name
/// is already read from user memory.
pub enum ContextParam {
    /// `VIRTGPU_CONTEXT_PARAM_CAPSET_ID`.
    Capset(u64),
    /// `VIRTGPU_CONTEXT_PARAM_NUM_RINGS`.
    Rings(u64),
    /// `VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK`.
    PollRings(u64),
    /// `VIRTGPU_CONTEXT_PARAM_DEBUG_NAME`.
    Name(Vec<u8>),
}

/// The most rings of a context (Linux's `VIRTGPU_MAX_RINGS`... 64).
pub const MAX_RINGS: u64 = 64;

/// Checks the parameters of `CONTEXT_INIT` against the device's
/// capability sets (`capset_mask`), as Linux does: each parameter at most
/// once, a capability set the device has, at most 64 rings. A polled ring
/// mask other than 0 is refused, because its fence events would need a
/// per-file event queue outside mode setting.
pub fn parse_context_params(params: Vec<ContextParam>, capset_mask: u64) -> Result<ContextParams, Errno> {
    let mut out = ContextParams::default();
    let (mut capset, mut rings, mut poll, mut name) = (false, false, false, false);
    let once = |seen: &mut bool| -> Result<(), Errno> {
        if core::mem::replace(seen, true) { Err(Errno::EINVAL) } else { Ok(()) }
    };
    for p in params {
        match p {
            ContextParam::Capset(v) => {
                once(&mut capset)?;
                if v == 0 || v >= 64 || capset_mask & (1 << v) == 0 {
                    return Err(Errno::EINVAL);
                }
                out.capset = v as u32;
            }
            ContextParam::Rings(v) => {
                once(&mut rings)?;
                if v > MAX_RINGS {
                    return Err(Errno::EINVAL);
                }
                out.rings = v as u32;
            }
            ContextParam::PollRings(v) => {
                once(&mut poll)?;
                if v != 0 {
                    return Err(Errno::EINVAL);
                }
            }
            ContextParam::Name(n) => {
                once(&mut name)?;
                out.name = n;
            }
        }
    }
    Ok(out)
}

/// Counters for the `virtio_gpu` kstat and the log.
#[derive(Default)]
pub struct Stats {
    /// Requests submitted.
    pub submitted: AtomicU64,
    /// Requests completed.
    pub completed: AtomicU64,
    /// Requests the device answered with an error.
    pub errors: AtomicU64,
    /// Fenced requests.
    pub fenced: AtomicU64,
    /// Submissions that found the queue full and waited.
    pub queue_full: AtomicU64,
    /// Waits that timed out (a hung device).
    pub timeouts: AtomicU64,
    /// Resources created.
    pub resources: AtomicU64,
    /// Resources the device still has.
    pub live_resources: AtomicU64,
}

/// The driver state the virtio framework's handlers reach (the `S` of
/// `Virtio<S>`).
pub struct Gpu {
    /// The control queue.
    pub ctrl: QueueId<CmdRef>,
    /// The cursor queue.
    pub cursor: QueueId<CmdRef>,
    /// Features and capability sets, set once during attach.
    pub caps: Mutex<Caps>,
    /// Orders fence ids with submission.
    submit: Mutex<()>,
    next_fence_id: AtomicU64,
    next_resource: AtomicU32,
    /// The device's fence domain and its context for fenced requests, set
    /// once the DRM device exists.
    domain: Mutex<Option<KArc<FenceDomain>>>,
    fence_context: AtomicU64,
    /// Capability set contents, fetched on first use.
    capset_cache: Mutex<Vec<(u32, u32, Vec<u8>)>>,
    /// Rendering contexts by file (clone id).
    files: Mutex<BTreeMap<u32, FileCtx>>,
    /// Serialises context creation.
    ctx_lock: Mutex<()>,
    /// The display configuration changed since the last probe.
    pub display_changed: AtomicBool,
    /// Counters.
    pub stats: Stats,
    /// Error responses logged so far (the log is limited).
    logged: AtomicU32,
    /// The instance, for log lines.
    pub instance: u32,
}

/// The device: the framework object with the driver state.
pub type Dev = VirtioDev<Gpu>;

impl VirtioHandler for Gpu {
    type Config = GpuConfig;
    const CONFIG_CHANGE: bool = true;

    fn queue_work(&self, dev: &Dev, index: u16, ctx: &Sleepable) {
        match index {
            proto::CONTROLQ => self.drain(dev, Q::Ctrl, ctx),
            proto::CURSORQ => self.drain(dev, Q::Cursor, ctx),
            _ => {}
        }
    }

    fn config_changed(&self, dev: &Dev, _ctx: &Sleepable) {
        let cfg = dev.config();
        let Ok(ev) = cfg.get32(proto::CFG_EVENTS_READ) else { return };
        if ev & proto::EVENT_DISPLAY != 0 {
            // The next probe asks the device again.
            self.display_changed.store(true, Ordering::Release);
        }
        let _ = cfg.put32(proto::CFG_EVENTS_CLEAR, ev);
    }
}

impl Gpu {
    /// The state, before the queues run.
    pub fn new(ctrl: QueueId<CmdRef>, cursor: QueueId<CmdRef>, caps: Caps, instance: u32) -> Gpu {
        Gpu {
            ctrl,
            cursor,
            caps: Mutex::new(caps),
            submit: Mutex::new(()),
            next_fence_id: AtomicU64::new(0),
            next_resource: AtomicU32::new(0),
            domain: Mutex::new(None),
            fence_context: AtomicU64::new(0),
            capset_cache: Mutex::new(Vec::new()),
            files: Mutex::new(BTreeMap::new()),
            ctx_lock: Mutex::new(()),
            display_changed: AtomicBool::new(true),
            stats: Stats::default(),
            logged: AtomicU32::new(0),
            instance,
        }
    }

    /// A copy of the capabilities.
    pub fn caps(&self) -> Caps {
        self.caps.lock().clone()
    }

    /// Whether 3D (virgl) is on.
    pub fn virgl(&self) -> bool {
        self.caps.lock().virgl
    }

    /// Connects the device's fence domain, once the DRM device exists.
    pub fn set_domain(&self, domain: KArc<FenceDomain>) {
        self.fence_context.store(domain.alloc_contexts(1), Ordering::Release);
        *self.domain.lock() = Some(domain);
    }

    /// The fence domain.
    pub fn domain(&self) -> Result<KArc<FenceDomain>, Errno> {
        self.domain.lock().clone().ok_or(Errno::ENXIO)
    }

    /// The fence context of fenced requests outside a ring.
    pub fn fence_context(&self) -> u64 {
        self.fence_context.load(Ordering::Acquire)
    }

    /// A new resource id (never 0).
    pub fn resource_id(&self) -> Result<u32, Errno> {
        // After four billion resources, refuse rather than reuse ids the
        // device may still know: the counter stops at its maximum.
        self.next_resource
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |n| n.checked_add(1).filter(|&m| m < u32::MAX))
            .map(|n| n + 1)
            .map_err(|_| Errno::ENOSPC)
    }

    // --- requests ---

    /// Allocates a request `req` with room for a response of `resp_len`
    /// bytes and `data_len` bytes of extra data (zeroed; the caller fills
    /// it).
    pub fn cmd<T: Wire>(
        &self,
        dev: &Dev,
        req: &T,
        resp_len: usize,
        data_len: usize,
        queue: Q,
        ctx: &Sleepable,
    ) -> Result<KArc<Cmd>, Errno> {
        let req_len = size_of::<T>();
        let resp_off = req_len.next_multiple_of(64);
        let buf = VirtioDma::alloc(dev, resp_off + resp_len.max(1), &CMD_ATTR, false, Some(ctx))?;
        buf.write(0, proto::bytes_of(req))?;
        let data = if data_len > 0 {
            if data_len > MAX_DATA {
                return Err(Errno::EINVAL);
            }
            Some(VirtioDma::alloc(dev, data_len, &DATA_ATTR, false, Some(ctx))?)
        } else {
            None
        };
        let c = Cmd {
            buf,
            req_len,
            resp_off,
            resp_len,
            data,
            data_len,
            queue,
            state: Mutex::new(CmdState::default()),
            cv: CondVar::new(),
            fence: Mutex::new(None),
            objs: Mutex::new(KVec::new()),
        };
        KArc::new(c, ctx)
    }

    fn queue_id(&self, q: Q) -> QueueId<CmdRef> {
        match q {
            Q::Ctrl => self.ctrl,
            Q::Cursor => self.cursor,
        }
    }

    /// Queues `cmd`, with a fence if `fence` is given, which is returned.
    /// Waits while the queue is full.
    pub fn submit(
        &self,
        dev: &Dev,
        cmd: &KArc<Cmd>,
        fence: Option<FenceReq>,
        ctx: &Sleepable,
    ) -> Result<Option<Fence>, Errno> {
        let queue = dev.queue(self.queue_id(cmd.queue)).ok_or(Errno::ENXIO)?;
        let _g = self.submit.lock();
        let mut out = None;
        if let Some(fr) = fence {
            let domain = self.domain()?;
            let id = self.next_fence_id.fetch_add(1, Ordering::AcqRel) + 1;
            let mut hdr: CtrlHdr = {
                let mut b = [0u8; size_of::<CtrlHdr>()];
                cmd.buf.read(0, &mut b)?;
                proto::read(&b).ok_or(Errno::EIO)?
            };
            hdr.flags |= proto::FLAG_FENCE;
            hdr.fence_id = id;
            if let Some(r) = fr.ring {
                hdr.flags |= proto::FLAG_INFO_RING_IDX;
                hdr.ring_idx = r;
            }
            cmd.buf.write(0, proto::bytes_of(&hdr))?;
            let f = Fence::new(&domain, fr.context, id, ctx)?;
            *cmd.fence.lock() = Some(f.clone());
            out = Some(f);
            self.stats.fenced.fetch_add(1, Ordering::Relaxed);
        }
        let start = HrTime::now();
        let mut r = CmdRef(cmd.clone());
        loop {
            match queue.submit(r, Some(ctx), true, build_chain) {
                Ok(()) => {
                    self.stats.submitted.fetch_add(1, Ordering::Relaxed);
                    return Ok(out);
                }
                Err((back, VirtioError::NoMemory | VirtioError::ChainFull)) if start.elapsed() < QUEUE_FULL_TIMEOUT => {
                    // No free descriptor: complete what the device has
                    // returned and try again.
                    self.stats.queue_full.fetch_add(1, Ordering::Relaxed);
                    r = back;
                    self.drain(dev, cmd.queue, ctx);
                    ddi::time::delay(Ticks::from_micros(1000), ctx);
                }
                Err((_, e)) => {
                    // Every fence must signal.
                    if let Some(f) = cmd.fence.lock().take() {
                        f.signal_with_error(Errno::EIO, Ctx::Sleep(ctx));
                        f.put(Ctx::Sleep(ctx));
                    }
                    if let Some(f) = out.take() {
                        f.put(Ctx::Sleep(ctx));
                    }
                    return Err(e.errno());
                }
            }
        }
    }

    /// Waits until the device has completed `cmd` and returns the response
    /// type. Uninterruptible, for requests the kernel itself needs; `EIO`
    /// after 10 seconds (`REQUEST_TIMEOUT`).
    pub fn wait(&self, dev: &Dev, cmd: &KArc<Cmd>, ctx: &Sleepable) -> Result<u32, Errno> {
        let start = HrTime::now();
        loop {
            {
                let g = cmd.state.lock();
                if g.done {
                    return Ok(g.resp);
                }
                let (g, _) = cmd.cv.wait_timeout(g, Duration::from_millis(20), ctx);
                if g.done {
                    return Ok(g.resp);
                }
            }
            // The completion may be waiting for the task queue; do it here.
            self.drain(dev, cmd.queue, ctx);
            if start.elapsed() > REQUEST_TIMEOUT {
                self.stats.timeouts.fetch_add(1, Ordering::Relaxed);
                dev_warn(self.instance, format_args!("the device did not answer a request in 10 s"));
                return Err(Errno::EIO);
            }
        }
    }

    /// Sends `req` and waits for the response, which must be `ok` (or any
    /// success type if `ok` is 0); returns the request for reading it.
    pub fn run<T: Wire>(
        &self,
        dev: &Dev,
        req: &T,
        resp_len: usize,
        ok: u32,
        ctx: &Sleepable,
    ) -> Result<KArc<Cmd>, Errno> {
        let c = self.cmd(dev, req, resp_len.max(size_of::<CtrlHdr>()), 0, Q::Ctrl, ctx)?;
        self.submit(dev, &c, None, ctx)?;
        let t = self.wait(dev, &c, ctx)?;
        check(t, ok)?;
        Ok(c)
    }

    /// Sends `req` without waiting for the response (later requests are
    /// processed after it; errors are logged).
    pub fn fire<T: Wire>(&self, dev: &Dev, req: &T, ctx: &Sleepable) -> Result<KArc<Cmd>, Errno> {
        let c = self.cmd(dev, req, size_of::<CtrlHdr>(), 0, Q::Ctrl, ctx)?;
        self.submit(dev, &c, None, ctx)?;
        Ok(c)
    }

    /// Completes every request the device has returned on `q`.
    pub fn drain(&self, dev: &Dev, q: Q, ctx: &Sleepable) {
        let Some(queue) = dev.queue(self.queue_id(q)) else { return };
        while let Some(done) = queue.poll() {
            self.complete(done.data.0, ctx);
        }
    }

    fn complete(&self, cmd: KArc<Cmd>, ctx: &Sleepable) {
        let resp = if cmd.resp_len > 0 {
            cmd.buf.sync_for_kernel();
            let mut b = [0u8; 4];
            match cmd.buf.read(cmd.resp_off, &mut b) {
                Ok(()) => u32::from_le_bytes(b),
                Err(_) => 0,
            }
        } else {
            proto::RESP_OK_NODATA
        };
        self.stats.completed.fetch_add(1, Ordering::Relaxed);
        let ok = proto::resp_ok(resp);
        if !ok {
            self.stats.errors.fetch_add(1, Ordering::Relaxed);
            let mut req = [0u8; 4];
            let _ = cmd.buf.read(0, &mut req);
            if self.logged.fetch_add(1, Ordering::Relaxed) < 32 {
                dev_warn(
                    self.instance,
                    format_args!(
                        "request {:#x} failed: {} ({:#x})",
                        u32::from_le_bytes(req),
                        proto::resp_name(resp),
                        resp
                    ),
                );
            }
        }
        // The objects go through the interrupt-safe path: if this is the
        // last reference, the GEM task queue destroys the object, whose
        // RESOURCE_UNREF must not be waited for on this thread.
        let objs = core::mem::take(&mut *cmd.objs.lock());
        drop(objs);
        {
            let mut s = cmd.state.lock();
            s.done = true;
            s.resp = resp;
            cmd.cv.notify_all();
        }
        let f = cmd.fence.lock().take();
        if let Some(f) = f {
            if !ok {
                f.set_error(Errno::EIO);
            }
            f.signal(Ctx::Sleep(ctx));
            f.put(Ctx::Sleep(ctx));
        }
        drop(cmd);
    }

    // --- device queries ---

    /// `GET_DISPLAY_INFO`.
    pub fn display_info(&self, dev: &Dev, ctx: &Sleepable) -> Result<proto::RespDisplayInfo, Errno> {
        let req = CtrlHdr::new(proto::CMD_GET_DISPLAY_INFO);
        let c = self.run(dev, &req, size_of::<proto::RespDisplayInfo>(), proto::RESP_OK_DISPLAY_INFO, ctx)?;
        c.response_as()
    }

    /// `GET_EDID` of `scanout`: the EDID, if the device has one.
    pub fn edid(&self, dev: &Dev, scanout: u32, ctx: &Sleepable) -> Result<Option<Vec<u8>>, Errno> {
        let req = proto::GetEdid { hdr: CtrlHdr::new(proto::CMD_GET_EDID), scanout, padding: 0 };
        let c = self.run(dev, &req, size_of::<proto::RespEdid>(), proto::RESP_OK_EDID, ctx)?;
        let mut size = [0u8; 4];
        c.response_at(size_of::<CtrlHdr>(), &mut size)?;
        let n = (u32::from_le_bytes(size) as usize).min(proto::EDID_MAX);
        if n < 128 {
            return Ok(None);
        }
        let mut edid = alloc::vec![0u8; n];
        c.response_at(size_of::<CtrlHdr>() + 8, &mut edid)?;
        Ok(Some(edid))
    }

    /// `GET_CAPSET_INFO` for every capability set (the first 64; ids fit
    /// in `SUPPORTED_CAPSET_IDs`, a 64-bit mask, only below 64 anyway).
    pub fn capset_infos(&self, dev: &Dev, n: u32, ctx: &Sleepable) -> Result<Vec<CapsetInfo>, Errno> {
        if n > 64 {
            dev_warn(self.instance, format_args!("the device reports {n} capability sets; using the first 64"));
        }
        let mut v = Vec::new();
        for i in 0..n.min(64) {
            let req =
                proto::GetCapsetInfo { hdr: CtrlHdr::new(proto::CMD_GET_CAPSET_INFO), capset_index: i, padding: 0 };
            let c = self.run(dev, &req, size_of::<proto::RespCapsetInfo>(), proto::RESP_OK_CAPSET_INFO, ctx)?;
            let r: proto::RespCapsetInfo = c.response_as()?;
            v.push(CapsetInfo { id: r.capset_id, max_version: r.capset_max_version, max_size: r.capset_max_size });
        }
        Ok(v)
    }

    /// The contents of capability set `id` at `version` (cached).
    pub fn capset(&self, dev: &Dev, id: u32, version: u32, ctx: &Sleepable) -> Result<Vec<u8>, Errno> {
        if let Some((_, _, d)) = self.capset_cache.lock().iter().find(|(i, v, _)| *i == id && *v == version) {
            return Ok(d.clone());
        }
        let info = self.caps.lock().capsets.iter().copied().find(|c| c.id == id).ok_or(Errno::EINVAL)?;
        if version > info.max_version {
            return Err(Errno::EINVAL);
        }
        let size = info.max_size as usize;
        if size > 64 << 10 {
            return Err(Errno::ENOMEM);
        }
        let req = proto::GetCapset { hdr: CtrlHdr::new(proto::CMD_GET_CAPSET), capset_id: id, capset_version: version };
        let c = self.run(dev, &req, size_of::<CtrlHdr>() + size, proto::RESP_OK_CAPSET, ctx)?;
        let mut d = alloc::vec![0u8; size];
        c.response_at(size_of::<CtrlHdr>(), &mut d)?;
        let mut cache = self.capset_cache.lock();
        if !cache.iter().any(|(i, v, _)| *i == id && *v == version) {
            cache.push((id, version, d.clone()));
        }
        Ok(d)
    }

    // --- contexts ---

    /// The context id of file `file`: its clone id, which the file keeps
    /// until its close has destroyed the context.
    pub fn ctx_id(file: CloneId) -> u32 {
        file.get()
    }

    /// Creates the rendering context of `file` if it does not exist
    /// (Linux's `virtio_gpu_create_context`); returns its id.
    pub fn ensure_context(&self, dev: &Dev, file: CloneId, ctx: &Sleepable) -> Result<u32, Errno> {
        let id = Self::ctx_id(file);
        if self.files.lock().get(&id).is_some_and(|f| f.created) {
            return Ok(id);
        }
        let _g = self.ctx_lock.lock();
        self.create_context_locked(dev, id, ctx)
    }

    /// `CONTEXT_INIT`: sets the capability set, the rings and the name of
    /// the context of `file` and creates it, all under the context lock,
    /// so that of two concurrent calls the second finds the context and
    /// fails with `EEXIST`, as Linux's `context_lock` makes it.
    pub fn init_context(&self, dev: &Dev, file: CloneId, p: &ContextParams, ctx: &Sleepable) -> Result<(), Errno> {
        let id = Self::ctx_id(file);
        let domain = self.domain()?;
        let _g = self.ctx_lock.lock();
        {
            let mut files = self.files.lock();
            let f = files.entry(id).or_default();
            if f.created {
                return Err(Errno::EEXIST);
            }
            // Kept if CTX_CREATE fails below, as Linux keeps them; a retry
            // sets them again and takes fresh ring contexts.
            f.capset = p.capset;
            f.num_rings = p.rings;
            f.ring_base = if p.rings > 0 { domain.alloc_contexts(u64::from(p.rings)) } else { 0 };
            f.name = p.name.clone();
        }
        self.create_context_locked(dev, id, ctx).map(drop)
    }

    /// Sends `CTX_CREATE` for context `id` unless it exists; the caller
    /// holds `ctx_lock`.
    fn create_context_locked(&self, dev: &Dev, id: u32, ctx: &Sleepable) -> Result<u32, Errno> {
        let (capset, name) = {
            let files = self.files.lock();
            match files.get(&id) {
                Some(f) if f.created => return Ok(id),
                Some(f) => (f.capset, f.name.clone()),
                None => (0, Vec::new()),
            }
        };
        let mut req = proto::CtxCreate {
            hdr: CtrlHdr::ctx(proto::CMD_CTX_CREATE, id),
            nlen: 0,
            context_init: if self.caps.lock().context_init { capset } else { 0 },
            debug_name: [0; 64],
        };
        let name: Vec<u8> = if name.is_empty() {
            // Linux names the context after the process; the DDI offers
            // the process ID.
            // SAFETY: ddi_get_pid(9F) may be called from any context.
            let pid = unsafe { ddi::sys::ddi_get_pid() };
            alloc::format!("pid {pid}").into_bytes()
        } else {
            name
        };
        let n = name.len().min(req.debug_name.len());
        if let (Some(d), Some(s)) = (req.debug_name.get_mut(..n), name.get(..n)) {
            d.copy_from_slice(s);
        }
        req.nlen = n as u32;
        self.run(dev, &req, size_of::<CtrlHdr>(), proto::RESP_OK_NODATA, ctx)?;
        self.files.lock().entry(id).or_default().created = true;
        Ok(id)
    }

    /// The fence request of a submission of `file` on `ring` (`None`: the
    /// device's own context).
    pub fn file_fence(&self, file: CloneId, ring: Option<u32>) -> Result<FenceReq, Errno> {
        match ring {
            None => Ok(FenceReq { context: self.fence_context(), ring: None }),
            Some(r) => {
                let files = self.files.lock();
                let f = files.get(&Self::ctx_id(file)).ok_or(Errno::EINVAL)?;
                if r >= f.num_rings || f.ring_base == 0 {
                    return Err(Errno::EINVAL);
                }
                Ok(FenceReq { context: f.ring_base + u64::from(r), ring: Some(r as u8) })
            }
        }
    }

    /// Destroys the context of a closing file.
    pub fn file_close(&self, dev: &Dev, file: CloneId, ctx: &Sleepable) {
        let id = Self::ctx_id(file);
        let _g = self.ctx_lock.lock();
        let f = self.files.lock().remove(&id);
        if f.is_some_and(|f| f.created) {
            let req = CtrlHdr::ctx(proto::CMD_CTX_DESTROY, id);
            if let Err(e) = self.run(dev, &req, size_of::<CtrlHdr>(), proto::RESP_OK_NODATA, ctx) {
                dev_warn(self.instance, format_args!("CTX_DESTROY of context {id}: {e:?}"));
            }
        }
    }

    /// Whether the file has a context.
    pub fn has_context(&self, file: CloneId) -> bool {
        self.files.lock().get(&Self::ctx_id(file)).is_some_and(|f| f.created)
    }
}

/// Appends a request's buffers to its chain.
fn build_chain<'a>(c: &'a Cmd, ch: &mut Chain<'a>) -> Result<(), VirtioError> {
    ch.dma(&c.buf, 0, c.req_len, Direction::DeviceReads)?;
    if let Some(d) = &c.data
        && c.data_len > 0
    {
        ch.dma(d, 0, c.data_len, Direction::DeviceReads)?;
    }
    if c.resp_len > 0 {
        ch.dma(&c.buf, c.resp_off, c.resp_len, Direction::DeviceWrites)?;
    }
    Ok(())
}

/// Maps a response type to the ioctl's errno: `ok` (or any success when
/// `ok` is 0), `ENOMEM` for the device's out-of-memory, `EINVAL` for a
/// parameter the device refused, `EIO` otherwise.
pub fn check(t: u32, ok: u32) -> Result<(), Errno> {
    if (ok == 0 && proto::resp_ok(t)) || (ok != 0 && t == ok) {
        return Ok(());
    }
    Err(match t {
        proto::RESP_ERR_OUT_OF_MEMORY => Errno::ENOMEM,
        proto::RESP_ERR_INVALID_PARAMETER | 0x1202..=0x1204 => Errno::EINVAL,
        _ => Errno::EIO,
    })
}

/// Logs a warning for instance `instance`.
pub fn dev_warn(instance: u32, args: core::fmt::Arguments<'_>) {
    ddi::kwarn!("virtio_gpu{instance}: {args}");
}

/// Logs a note for instance `instance`.
pub fn dev_note(instance: u32, args: core::fmt::Arguments<'_>) {
    ddi::knote!("virtio_gpu{instance}: {args}");
}
