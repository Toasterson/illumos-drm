// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! A registered DRM device and its character-device entry points.
//!
//! # Concurrency
//!
//! All mutable state of a device (open files, masters, magic numbers,
//! client capabilities) is one [`DeviceState`] behind one `ddi::Mutex`.
//! Every entry point takes the mutex for exactly one state transition:
//! `open(9E)` registers the file, `close(9E)` removes it, and each ioctl
//! runs its permission check and its state change inside the same critical
//! section, so no check can be invalidated before its action (a master
//! cannot be dropped between `AUTH_MAGIC`'s permission check and the
//! authentication, for example). Copies from and to user memory happen
//! outside the mutex, before and after the critical section, so a page
//! fault on a user buffer never holds up other files. The only operation
//! that may block with the mutex held is table growth with `KM_SLEEP`,
//! which waits for memory, not for another file.
//!
//! The lifetime rules come from specfs: every open is a clone open, so each
//! file has its own device number and `close(9E)` is called exactly once,
//! after the last reference to the file is gone, and never concurrently
//! with an ioctl on the same file (an ioctl holds a file reference).
//! `detach(9E)` is not called while any file is open; the core refuses it
//! anyway if the state still counts open files.
//!
//! The state machine behind these entry points is unit-tested on the host,
//! including a test that drives it from eight threads through a mutex
//! (`state::tests::concurrent_open_close_auth_master`); the kernel side is
//! exercised with concurrent opens and ioctls by `tools/test/drm_r0_test.c`.
//!
//! GEM state (handles, names, objects, mappings) lives in a separate
//! [`GemCore`] with its own locks (see [`crate::gem`]); the device mutex is
//! never held while one of them is taken. A GEM ioctl checks its permission
//! under the device mutex and acts under the GEM locks afterwards. That gap
//! is harmless because the permissions GEM ioctls depend on cannot be lost:
//! a file's node kind never changes and a file never loses its
//! authentication. `close(9E)` runs only after every ioctl of the file has
//! returned, so no GEM ioctl can see a file whose GEM state is gone.
//!
//! A Linux-KMS device (facade 2.0, [`crate::lkms`]) adds the event queues
//! of its files behind their own lock, which the driver's KMS code takes
//! from its event functions, also in interrupt context; the device mutex
//! may be held while taking it (a file's master status is mirrored into
//! its queue), never the other way round, and neither is held while the
//! KMS code's hooks run.

use ddi::prelude::*;
use ddi::sys;
use drm_uapi as uapi;

use crate::anon_fd;
use crate::caps;
use crate::driver::{BUS_ID_MAX, BusId, Driver, DumbCreated, ENOSYS, Features};
use crate::dumb::DumbRequest;
use crate::fence_dev::SyncCore;
use crate::gem::{GemCore, registry};
use crate::ioctl::{self, Entry, Op, SizePlan};
use crate::karc::KArc;
use crate::kms::dev::{Caller, Kms};
use crate::kms::ioctl as kmsioctl;
use crate::lkms::{self, dev::LkmsQueues};
use crate::minor::{self, CloneId, MAX_DEVICES, NodeKind};
use crate::offsets::PAGE_SIZE;
use crate::state::{DeviceState, FileFlags, IF_MAJOR, IF_MINOR, Opener, SetVersion};
use crate::uaccess::{ArgIo, CoreArg, UserArg, copy_in_bytes, copy_out_bytes};

/// One DRM device: the driver's state, the card and render minor nodes, and
/// the per-file state.
///
/// A driver module uses `Device<D>` as the instance type of its
/// `ddi::Driver`, creates it with [`Device::attach`] and hands it back to
/// [`Device::detach`]; the `ddi::CharDevice` implementation below provides
/// the open, close and ioctl entry points.
pub struct Device<D: Driver> {
    driver: D,
    dev: DevInfo,
    index: u32,
    bus_id: BusId,
    state: Mutex<DeviceState>,
    has_render: bool,
    /// The GEM state, for drivers with [`Features::GEM`].
    gem: Option<KArc<GemCore<D::Object>>>,
    /// The mode-setting state, for drivers with a [`Driver::kms`].
    kms: Option<Kms<D::Object>>,
    /// Facade 2.0: the event queues of a Linux-KMS device (a driver with
    /// [`Driver::lkms`]), whose mode setting is the driver's C code.
    lkms: Option<LkmsQueues>,
    /// Fences, sync objects, dma-bufs and sync files (R2).
    sync: KArc<SyncCore<D::Object>>,
    // Dropping these removes the minor nodes.
    _card: MinorNode,
    _render: Option<MinorNode>,
    // The device number goes back last, after the nodes that carry it.
    _number: Number,
}

/// The table of DRM device numbers of the system (until R5a the module
/// `misc/drm_minor`): every device of every driver gets the lowest free
/// number `N` and the nodes `card<N>` and `renderD<128+N>`.
static NUMBERS: crate::slots::Slots = crate::slots::Slots::new();

/// The number of device numbers taken, for the module's `_fini`.
pub fn numbers_taken() -> u32 {
    NUMBERS.taken()
}

/// A device number from [`NUMBERS`], recorded as the device's driver
/// instance (`minor::bind`); dropping it forgets the instance and gives
/// the number back.
struct Number {
    index: u32,
    inst: Instance,
    /// The owner token, the device's `dev_info_t`.
    owner: usize,
}

impl Number {
    /// Takes the lowest free device number of the system for `dev`.
    fn claim(dev: &DevInfo) -> Result<Number, DdiError> {
        let owner = dev.as_raw() as usize;
        let Some(index) = NUMBERS.alloc(owner).filter(|i| *i < MAX_DEVICES) else {
            dev.warn(format_args!("all {MAX_DEVICES} DRM device numbers are in use"));
            return Err(DdiError);
        };
        let n = Number { index, inst: dev.instance(), owner };
        if !minor::bind(index, n.inst) {
            // Unreachable while the table hands each number to one device;
            // refuse rather than share a number.
            dev.warn(format_args!("device number {index} is already recorded"));
            NUMBERS.free(index, owner);
            return Err(DdiError);
        }
        Ok(n)
    }
}

impl Drop for Number {
    fn drop(&mut self) {
        minor::unbind(self.index, self.inst);
        NUMBERS.free(self.index, self.owner);
    }
}

impl<D: Driver> Device<D> {
    /// Registers a device for the driver instance `node`, the capability for
    /// the driver's own node that `ddi::Driver::attach` received; the GEM
    /// state keeps it to allocate DMA handles. Takes the lowest free device
    /// number `N` of the system (`NUMBERS`), shared by every
    /// driver built on the core, and creates the primary node `card<N>`
    /// and, if the driver has [`Features::RENDER`], the render node
    /// `renderD<128+N>`. The number stays the device's until it detaches.
    /// Both nodes have the node type `ddi_display:drm`
    /// (`DDI_NT_DISPLAY_DRM`), for which devfsadm creates the `/dev/dri`
    /// links (see `packaging/devlink.tab`).
    pub fn attach(node: OwnNode, driver: D, ctx: &Sleepable) -> Result<Device<D>, DdiError> {
        let dev = node.dev();
        let info = *driver.info();
        // Taken first and dropped last: every early return below gives it
        // back after the nodes are gone.
        let number = Number::claim(&dev)?;
        let index = number.index;
        let mut bus_id = BusId::new();
        if driver.bus_id(&dev, &mut bus_id).is_err() {
            dev.warn(format_args!("bus ID does not fit in {BUS_ID_MAX} bytes"));
            return Err(DdiError);
        }
        if info.features.contains(Features::DUMB) && !info.features.contains(Features::GEM) {
            dev.warn(format_args!("dumb buffers need GEM"));
            return Err(DdiError);
        }
        // The offset arithmetic assumes the amd64 page.
        // SAFETY: ptob(9F) may be called from any context.
        if unsafe { sys::ptob(1) } as u64 != PAGE_SIZE {
            dev.warn(format_args!("unexpected page size"));
            return Err(DdiError);
        }
        // The minor nodes are created on the driver's own node (illumos-ddi
        // takes `&OwnNode` for them) before the GEM state takes the node.
        // Opens wait until attach has returned, and dropping them on a
        // later failure removes them again.
        let (card, render) = Self::nodes(&node, index, info.features)?;
        let gem = if info.features.contains(Features::GEM) {
            match GemCore::new(node, driver.gem_dma(&dev), info.max_memory, ctx) {
                Ok(g) => Some(g),
                Err(e) => {
                    dev.warn(format_args!("cannot set up GEM: {e:?}"));
                    return Err(DdiError);
                }
            }
        } else {
            None
        };
        if info.features.contains(Features::SYNCOBJ_TIMELINE) && !info.features.contains(Features::SYNCOBJ) {
            dev.warn(format_args!("timeline sync objects need SYNCOBJ"));
            return Err(DdiError);
        }
        let sync = match SyncCore::new(dev, index, driver.fence_priority(), ctx) {
            Ok(s) => s,
            Err(e) => {
                dev.warn(format_args!("cannot set up fences: {e:?}"));
                return Err(DdiError);
            }
        };
        // Facade 2.0: a Linux-KMS driver's device has the core's files and
        // event queues, and no mode-setting state of the core's; its mode
        // setting starts later (Device::lkms_start).
        let lkms = match driver.lkms() {
            Some(_) if driver.kms().is_some() => {
                dev.warn(format_args!("a driver has the core's mode setting or Linux's, not both"));
                return Err(DdiError);
            }
            Some(_) => match LkmsQueues::new(driver.fence_priority(), ctx) {
                Ok(q) => Some(q),
                Err(e) => {
                    dev.warn(format_args!("cannot set up the event queues: {e:?}"));
                    return Err(DdiError);
                }
            },
            None => None,
        };
        let kms = match driver.kms() {
            Some(k) if gem.is_some() && info.features.contains(Features::MODESET) => {
                let modifiers = info.mode_config.fb_modifiers;
                match Kms::new(dev, k, info.features.contains(Features::ATOMIC), modifiers, ctx) {
                    Ok(k) => Some(k),
                    Err(e) => {
                        dev.warn(format_args!("cannot set up mode setting: {e:?}"));
                        return Err(DdiError);
                    }
                }
            }
            Some(_) => {
                dev.warn(format_args!("mode setting needs MODESET and GEM"));
                return Err(DdiError);
            }
            None => None,
        };
        // The devmap callbacks find the GEM state through this registry.
        // Recording it is the last step that can fail, so that no failed
        // attach leaves an entry behind: the device number is the lowest
        // free one of the system, and the next attach of this driver would
        // usually be handed the same number and find the entry taken.
        if let Some(g) = &gem
            && !registry::insert(index, &**g)
        {
            dev.warn(format_args!("device {index} is registered twice"));
            if let Some(k) = &kms {
                k.teardown(ctx);
            }
            return Err(DdiError);
        }
        dev.report();
        Ok(Device {
            driver,
            dev,
            index,
            bus_id,
            state: Mutex::new(DeviceState::new()),
            has_render: render.is_some(),
            gem,
            kms,
            lkms,
            sync,
            _card: card,
            _render: render,
            _number: number,
        })
    }

    fn nodes(node: &OwnNode, index: u32, features: Features) -> Result<(MinorNode, Option<MinorNode>), DdiError> {
        let card = Self::node(node, NodeKind::Primary, index)?;
        let render =
            if features.contains(Features::RENDER) { Some(Self::node(node, NodeKind::Render, index)?) } else { None };
        Ok((card, render))
    }

    fn node(node: &OwnNode, kind: NodeKind, index: u32) -> Result<MinorNode, DdiError> {
        let name = minor::node_name(kind, index).ok_or(DdiError)?;
        let m = minor::node_minor(kind, index).ok_or(DdiError)?;
        node.create_minor_node(name, MinorKind::Char, Minor(m), NodeType::Custom(sys::DDI_NT_DISPLAY_DRM))
    }

    /// Unregisters the device. Refuses, and hands the device back, while
    /// files are open, framebuffers are still referenced, GEM objects
    /// remain or fences and sync objects are alive (after the driver's
    /// [`Driver::before_detach`] and the deferred releases have run). All checks run before anything is torn
    /// down, so a refused device keeps working; only then are the driver's
    /// sources stopped ([`Driver::before_teardown`]) and the mode-setting
    /// and GEM state released.
    #[allow(clippy::result_large_err, reason = "the shape of ddi::Driver::detach, which hands the instance back")]
    pub fn detach(self, ctx: &Sleepable) -> Result<(), (Device<D>, DdiError)> {
        if let Err(e) = self.detach_check(ctx) {
            return Err((self, e));
        }
        self.detach_teardown(ctx);
        Ok(())
    }

    /// Whether user space still uses the device: open files (anonymous
    /// files of dma-bufs and sync files included) or user mappings of GEM
    /// objects. A driver whose own teardown must come before the core's
    /// (a Linux driver's remove, which frees its GEM objects) asks this
    /// first: `Err` with a warning while user space holds the device.
    pub fn user_check(&self) -> Result<(), DdiError> {
        let open = self.state.lock().open_files();
        if open != 0 {
            self.dev.warn(format_args!("detach refused: {open} files open"));
            return Err(DdiError);
        }
        if let Some(g) = &self.gem {
            let s = g.stats();
            if s.mappings + s.pending_maps != 0 {
                self.dev.warn(format_args!("detach refused: {} user mappings remain", s.mappings + s.pending_maps));
                return Err(DdiError);
            }
        }
        Ok(())
    }

    /// The first half of [`Device::detach`], on a shared device: every
    /// check that may refuse the detach (open files, framebuffers, GEM
    /// state, fences and sync objects), with [`Driver::before_detach`] and
    /// the release of leftovers before them. A refused device keeps
    /// working. For a caller that must not move the device before it
    /// knows the detach succeeds (`misc/drm_core`, whose device other
    /// threads may be using).
    pub fn detach_check(&self, ctx: &Sleepable) -> Result<(), DdiError> {
        let open = self.state.lock().open_files();
        if open != 0 {
            self.dev.warn(format_args!("detach refused: {open} files open"));
            return Err(DdiError);
        }
        self.driver.before_detach(ctx);
        // Anonymous files are closed (they keep the device open), so this
        // only drops leftovers nothing can reach; dma-bufs hold GEM objects,
        // which hold fences.
        self.sync.clear(ctx);
        // Every check runs before anything is torn down: a refused detach
        // leaves the device attached, so it must leave it working, its
        // mode-setting state, the driver's vblank source and its fence
        // timeline included.
        if let Some(Err(n)) = self.kms.as_ref().map(|k| k.detach_check(ctx)) {
            self.dev.warn(format_args!("detach refused: {n} framebuffers still referenced"));
            return Err(DdiError);
        }
        if let Some(Err(s)) = self.gem.as_ref().map(|g| GemCore::quiesce(g, ctx)) {
            self.dev.warn(format_args!("detach refused: GEM state in use: {s:?}"));
            return Err(DdiError);
        }
        if let Err(s) = SyncCore::quiesce(&self.sync, ctx) {
            self.dev.warn(format_args!("detach refused: fences or sync objects remain: {s:?}"));
            return Err(DdiError);
        }
        Ok(())
    }

    /// The second half of [`Device::detach`], after
    /// [`Device::detach_check`] succeeded: the driver stops its sources,
    /// then the mode-setting state goes, then the GEM state. The device is
    /// dropped afterwards.
    pub fn detach_teardown(&self, ctx: &Sleepable) {
        self.driver.before_teardown(ctx);
        if let Some(k) = &self.kms {
            k.teardown(ctx);
        }
        if self.gem.is_some() {
            registry::remove(self.index);
        }
    }

    /// Facade 2.0: starts the mode setting of a Linux-KMS driver's device
    /// (`drm_dev_illumos_lkms_register()`): `EBUSY` while a file is open
    /// (files opened before would have no KMS state), then the KMS code's
    /// `mode_config` and `register_dev` through `reg`, whose error is the
    /// result (mode setting stays off); from then on files opened get the
    /// KMS code's hooks and `GET_CAP` answers with `features` and the KMS
    /// code's configuration. `EINVAL` for another device.
    pub fn lkms_start(
        &self,
        features: Features,
        h: &dyn lkms::LkmsHooks,
        reg: &mut lkms::Registration,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let q = self.lkms.as_ref().ok_or(Errno::EINVAL)?;
        if q.active() || reg.is_registered() {
            return Err(Errno::EBUSY);
        }
        if self.state.lock().open_files() != 0 {
            return Err(Errno::EBUSY);
        }
        let m = reg.start(h, ctx)?;
        q.start(features, m);
        Ok(())
    }

    /// The number of open files (a snapshot).
    pub fn open_files(&self) -> u32 {
        self.state.lock().open_files()
    }

    /// What `GET_CAP` and `SET_CLIENT_CAP` see ([`lkms::caps_view`]).
    fn caps(&self) -> (Features, crate::driver::ModeConfig) {
        let info = self.driver.info();
        let started = self.lkms.as_ref().and_then(|q| q.started());
        lkms::caps_view((info.features, info.mode_config), self.lkms.is_some(), started)
    }

    /// The core's own `MODE_CREATE_DUMB`: [`Driver::dumb_layout`], the
    /// size limits, the device's memory budget, system memory,
    /// [`Driver::dumb_object`], the object and its handle in `file`.
    pub fn dumb_create_default(&self, file: CloneId, req: &DumbRequest, ctx: &Sleepable) -> Result<DumbCreated, Errno> {
        let g = self.gem_or(ENOSYS)?;
        let layout = self.driver.dumb_layout(req)?;
        let size = usize::try_from(layout.size).map_err(|_| Errno::EINVAL)?;
        if size > self.driver.info().max_object_size {
            return Err(Errno::ENOMEM);
        }
        // Charged before the locked allocation, so that concurrent
        // requests cannot allocate past the device's budget.
        let reservation = g.reserve(size, ctx)?;
        let mem = g.sys_mem(size, ctx)?;
        let obj = self.driver.dumb_object(self, mem, req, &layout, ctx)?;
        let obj = GemCore::create_reserved(g, obj, reservation, ctx)?;
        let size = obj.size() as u64;
        let handle = g.handle_create(file, obj, ctx)?;
        Ok(DumbCreated { handle, pitch: layout.pitch, size })
    }

    /// The core's own `MODE_MAP_DUMB`: the fake offset of the object of
    /// `handle` in `file`, made if it has none.
    pub fn dumb_map_offset_default(&self, file: CloneId, handle: u32, ctx: &Sleepable) -> Result<u64, Errno> {
        let g = self.gem_or(ENOSYS)?;
        let obj = g.lookup(file, handle)?;
        let off = g.mmap_offset(&obj, ctx);
        obj.put(ctx);
        off
    }

    /// Reports whether open file `c` is the device's current master (a
    /// compatibility layer's `drm_is_current_master`).
    pub fn is_current_master(&self, c: CloneId) -> bool {
        self.state.lock().is_current_master(c)
    }

    /// What the facade's `drm_file` of open file `c` mirrors (facade 2.0):
    /// its client capabilities and master status.
    pub fn file_flags(&self, c: CloneId) -> Option<FileFlags> {
        self.state.lock().file_flags(c)
    }

    /// Facade 2.0: the event queues of a Linux-KMS device.
    pub fn lkms_events(&self) -> Option<&LkmsQueues> {
        self.lkms.as_ref()
    }

    /// Passes the state of file `c` that the driver mirrors to it and to
    /// the event queues, after a change, with the state lock held.
    fn mirror(&self, s: &DeviceState, c: CloneId) {
        let Some(f) = s.file_flags(c) else { return };
        if let Some(q) = &self.lkms {
            q.set_master(c, f.is_master);
        }
        self.driver.file_flags(c, &f);
    }

    /// Returns the synchronisation state: the fence domain drivers create
    /// their fences in, and the sync object and anonymous-file tables.
    pub fn sync(&self) -> &KArc<SyncCore<D::Object>> {
        &self.sync
    }

    /// Returns the GEM state, if the driver has [`Features::GEM`].
    pub fn gem(&self) -> Option<&KArc<GemCore<D::Object>>> {
        self.gem.as_ref()
    }

    /// Returns the mode-setting state, if the driver has a [`Driver::kms`].
    pub fn kms(&self) -> Option<&Kms<D::Object>> {
        self.kms.as_ref()
    }

    /// The node kind and opening process of open file `c`.
    pub fn file_info(&self, c: CloneId) -> Option<(NodeKind, i32)> {
        self.state.lock().file(c).map(|f| (f.kind, f.pid))
    }

    /// The device number `N` (`card<N>`, `renderD<128+N>`).
    pub fn index(&self) -> u32 {
        self.index
    }

    /// Whether the device has a render node.
    pub fn has_render(&self) -> bool {
        self.has_render
    }

    /// Returns the driver's state.
    pub fn driver(&self) -> &D {
        &self.driver
    }

    /// Returns the device node.
    pub fn dev_info(&self) -> DevInfo {
        self.dev
    }

    fn version(
        &self,
        c: CloneId,
        perm: crate::state::Perm,
        privileged: &dyn Fn() -> bool,
        arg: &CoreArg<'_>,
    ) -> Result<(), Errno> {
        let mut v = UserArg::<uapi::drm_version>::read(arg)?;
        self.state.lock().permit(c, perm, privileged)?;
        // SAFETY: only field assignments below.
        let a = unsafe { v.get_mut() };
        let info = self.driver.info();
        a.version_major = info.major;
        a.version_minor = info.minor;
        a.version_patchlevel = info.patchlevel;
        copy_field(arg, &mut a.name_len, a.name as usize, info.name.to_bytes())?;
        copy_field(arg, &mut a.date_len, a.date as usize, info.date.to_bytes())?;
        copy_field(arg, &mut a.desc_len, a.desc as usize, info.desc.to_bytes())?;
        v.write(arg)
    }

    fn get_unique(
        &self,
        c: CloneId,
        perm: crate::state::Perm,
        privileged: &dyn Fn() -> bool,
        arg: &CoreArg<'_>,
    ) -> Result<(), Errno> {
        let mut u = UserArg::<uapi::drm_unique>::read(arg)?;
        let set = {
            let s = self.state.lock();
            s.permit(c, perm, privileged)?;
            s.unique_set(c)
        };
        let value: &[u8] = if set { self.bus_id.as_bytes() } else { &[] };
        // SAFETY: only a field assignment below.
        let a = unsafe { u.get_mut() };
        // As on Linux, a buffer that is too small receives nothing; the
        // caller learns the length and asks again.
        if a.unique_len >= value.len() {
            copy_out_bytes(arg, a.unique as usize, value)?;
        }
        a.unique_len = value.len();
        u.write(arg)
    }

    fn set_unique(
        &self,
        c: CloneId,
        perm: crate::state::Perm,
        privileged: &dyn Fn() -> bool,
        arg: &CoreArg<'_>,
    ) -> Result<(), Errno> {
        let u = UserArg::<uapi::drm_unique>::read(arg)?;
        // Check permission before touching the string, so that a caller
        // without it learns nothing else; check again when acting on it.
        self.state.lock().permit(c, perm, privileged)?;
        let len = u.get().unique_len;
        if len == 0 || len > BUS_ID_MAX {
            return Err(Errno::EINVAL);
        }
        let mut buf = [0u8; BUS_ID_MAX];
        let want = buf.get_mut(..len).ok_or(Errno::EINVAL)?;
        copy_in_bytes(arg, u.get().unique as usize, want)?;
        let mut s = self.state.lock();
        s.permit(c, perm, privileged)?;
        // The bus ID is a property of the hardware; the only value a master
        // may set is the device's own.
        if *want != *self.bus_id.as_bytes() {
            return Err(Errno::EINVAL);
        }
        s.set_unique(c)
    }
}

/// Copies one string of `DRM_IOCTL_VERSION`: at most `*len` bytes of
/// `value` go to the caller's buffer, and `*len` becomes the full length of
/// `value`. A short buffer is the documented way for libdrm to learn the
/// length, so the partial copy is the ABI, not a silent truncation.
fn copy_field(arg: &CoreArg<'_>, len: &mut usize, addr: usize, value: &[u8]) -> Result<(), Errno> {
    let n = value.len().min(*len);
    copy_out_bytes(arg, addr, value.get(..n).ok_or(Errno::EINVAL)?)?;
    *len = value.len();
    Ok(())
}

/// The anonymous user ID reported by `GET_CLIENT` (`UID_NOBODY`).
const UID_NOBODY: u64 = 60001;

fn caller_pid() -> i32 {
    // SAFETY: ddi_get_pid(9F) may be called from user context, which every
    // cb_ops entry point runs in.
    unsafe { sys::ddi_get_pid() }
}

impl<D: Driver> CharDevice for Device<D> {
    fn open(
        &self,
        _minor: Minor,
        _flags: OpenFlags,
        _otyp: OpenType,
        _cred: &Cred<'_>,
        _ctx: &Sleepable,
    ) -> Result<()> {
        // Every open goes through clone_open; the glue never calls this.
        Err(Errno::ENXIO)
    }

    fn clone_open(
        &self,
        minor: Minor,
        _flags: OpenFlags,
        otyp: OpenType,
        cred: &Cred<'_>,
        ctx: &Sleepable,
    ) -> Result<Option<Minor>> {
        if otyp != OpenType::Char {
            return Err(Errno::EINVAL);
        }
        // A dma-buf, sync object or sync file being exported: the export
        // path opens its minor itself (crate::anon_fd); no clone.
        if let Some((index, id)) = anon_fd::decode(minor.0) {
            if index != self.index {
                return Err(Errno::ENXIO);
            }
            self.sync.anon_open(id)?;
            return Ok(None);
        }
        let d = minor::decode(minor.0).ok_or(Errno::ENXIO)?;
        if d.index != self.index || d.clone.is_some() || (d.kind == NodeKind::Render && !self.has_render) {
            return Err(Errno::ENXIO);
        }
        let who = Opener { kind: d.kind, privileged: cred.has_sys_devices_priv(), pid: caller_pid() };
        let (clone, m) = {
            let mut s = self.state.lock();
            let clone = s.open(who, ctx)?;
            match minor::file_minor(d.kind, self.index, clone) {
                Some(m) => (clone, m),
                None => {
                    s.close(clone);
                    return Err(Errno::ENXIO);
                }
            }
        };
        // Creating the handle table may block; the device mutex is not held.
        if let Some(g) = &self.gem
            && let Err(e) = g.file_open(clone, ctx)
        {
            self.state.lock().close(clone);
            return Err(e);
        }
        if let Err(e) = self.sync.file_open(clone, ctx) {
            if let Some(g) = &self.gem {
                g.file_close(clone, ctx);
            }
            self.state.lock().close(clone);
            return Err(e);
        }
        if let Some(k) = &self.kms {
            k.file_open(clone, ctx);
        }
        if let Some(q) = &self.lkms {
            let master = self.state.lock().is_current_master(clone);
            if let Err(e) = q.open(clone, master, ctx) {
                if let Some(g) = &self.gem {
                    g.file_close(clone, ctx);
                }
                self.sync.file_close(clone, ctx);
                self.state.lock().close(clone);
                return Err(e);
            }
        }
        // For a Linux-KMS device whose mode setting runs the driver's
        // file_open runs the KMS code's open before the driver's own
        // (lkms::open_file) and marks the file (LkmsQueues::set_kms_open).
        if let Err(e) = self.driver.file_open(self, clone, ctx) {
            if let Some(q) = &self.lkms {
                q.close(clone);
            }
            if let (Some(k), Some(drv)) = (&self.kms, self.driver.kms()) {
                k.file_close(clone, drv, ctx);
            }
            if let Some(g) = &self.gem {
                g.file_close(clone, ctx);
            }
            self.sync.file_close(clone, ctx);
            self.state.lock().close(clone);
            return Err(e);
        }
        Ok(Some(Minor(m)))
    }

    fn close(&self, minor: Minor, _flags: OpenFlags, _otyp: OpenType, _cred: &Cred<'_>, ctx: &Sleepable) -> Result<()> {
        if let Some((_, id)) = anon_fd::decode(minor.0) {
            self.sync.anon_close(id, ctx);
            return Ok(());
        }
        let d = minor::decode(minor.0).ok_or(Errno::ENXIO)?;
        // A node minor without a clone id was never handed out by
        // clone_open, so there is no file to forget.
        if let Some(c) = d.clone {
            // The clone id is released last, after the file's KMS, GEM and
            // sync object state, so that a concurrent open cannot be given
            // the same id while that state still exists (Linux's
            // drm_file_free order; such an open would fail with EBUSY).
            // Framebuffers hold GEM references: KMS goes before GEM. For a
            // Linux-KMS device the file's queued events go first and the
            // KMS code's file_close runs before the release (it removes
            // the file's framebuffers and blobs), lastclose after the last
            // file (lkms::close_file).
            let (hooks, opened) = match &self.lkms {
                Some(q) if q.active() => (self.driver.lkms(), q.kms_open(c)),
                _ => (None, false),
            };
            if let Some(q) = &self.lkms {
                q.begin_close(c);
            }
            lkms::close_file(
                hooks,
                opened,
                c.get(),
                || {
                    if let (Some(k), Some(drv)) = (&self.kms, self.driver.kms()) {
                        k.file_close(c, drv, ctx);
                    }
                    if let Some(g) = &self.gem {
                        g.file_close(c, ctx);
                    }
                    self.sync.file_close(c, ctx);
                    self.driver.file_close(self, c, ctx);
                    if let Some(q) = &self.lkms {
                        q.close(c);
                    }
                    let mut s = self.state.lock();
                    s.close(c);
                    s.open_files() == 0
                },
                ctx,
            );
        }
        Ok(())
    }

    fn read(&self, minor: Minor, uio: &mut Uio<'_>, _cred: &Cred<'_>, ctx: &Sleepable) -> Result<()> {
        let c = minor::decode(minor.0).and_then(|d| d.clone).ok_or(Errno::ENXIO)?;
        if let Some(q) = &self.lkms {
            return q.read(c, uio, ctx);
        }
        self.kms.as_ref().ok_or(Errno::ENXIO)?.read(c, uio, ctx)
    }

    fn chpoll<'a>(
        &'a self,
        minor: Minor,
        _events: PollEvents,
        _anyyet: bool,
    ) -> Result<(PollEvents, PollRegistration<'a>)> {
        // A sync file is readable once its fence has signalled; a dma-buf
        // has no poll semantics here (implicit fences are not polled).
        if let Some((_, id)) = anon_fd::decode(minor.0) {
            return self.sync.anon_chpoll(id);
        }
        let c = minor::decode(minor.0).and_then(|d| d.clone).ok_or(Errno::ENXIO)?;
        if let Some(q) = &self.lkms {
            return q.chpoll(c);
        }
        self.kms.as_ref().ok_or(Errno::ENXIO)?.chpoll(c)
    }

    fn devmap(
        &self,
        minor: Minor,
        dhp: DevmapHandle<'_>,
        off: u64,
        len: usize,
        _model: DataModel,
        ctx: &Sleepable,
    ) -> Result<usize> {
        if let Some((_, id)) = anon_fd::decode(minor.0) {
            return self.sync.anon_devmap(id, self.gem.as_ref(), &dhp, off, len, ctx);
        }
        let c = minor::decode(minor.0).and_then(|d| d.clone).ok_or(Errno::ENXIO)?;
        let g = self.gem.as_ref().ok_or(Errno::ENXIO)?;
        g.devmap(c, &dhp, off, len, ctx)
    }

    fn ioctl(&self, minor: Minor, cmd: IoctlCmd, arg: IoctlArg<'_>, cred: &Cred<'_>, ctx: &Sleepable) -> Result<i32> {
        // Only the LP64 ABI exists. A 32-bit caller gets ENOTSUP for every
        // core command (recognised by group and number, since its structure
        // sizes differ) instead of a misread structure, and ENOTTY for
        // anything else, as any caller would.
        let cmd = cmd.0 as u32;
        // Ioctls on dma-buf and sync files; their structures have the same
        // layout in both data models.
        if let Some((_, id)) = anon_fd::decode(minor.0) {
            self.sync.anon_ioctl(id, cmd, &arg, ctx)?;
            return Ok(0);
        }
        if arg.model() == DataModel::Ilp32 {
            let known =
                ioctl::is_core_nr(cmd) || ioctl::is_driver_nr(self.driver.ioctls(), cmd) || kmsioctl::is_kms_nr(cmd);
            return Err(if known { Errno::ENOTSUP } else { Errno::ENOTTY });
        }
        let c = minor::decode(minor.0).and_then(|d| d.clone).ok_or(Errno::ENXIO)?;
        // drv_priv(9F) is only called when an ioctl's permission needs it.
        let privileged = &|| cred.has_sys_devices_priv();
        // Core and mode-setting commands match whatever their size bits,
        // and their argument is copied with Linux's size rule (a shorter
        // structure is zero-extended, a longer one keeps its extra bytes;
        // crate::ioctl). Driver commands match on their complete value.
        if let Some(entry) = ioctl::lookup(cmd) {
            let a = CoreArg::new(&arg, SizePlan::new(entry.cmd, cmd))?;
            self.core_ioctl(entry, c, privileged, &a, ctx)?;
            return Ok(0);
        }
        if let Some(entry) = kmsioctl::lookup(cmd) {
            let a = CoreArg::new(&arg, SizePlan::new(entry.cmd, cmd))?;
            self.kms_ioctl(entry, cmd, c, privileged, &a, ctx)?;
            return Ok(0);
        }
        let entry = *ioctl::lookup_driver(self.driver.ioctls(), cmd).ok_or(Errno::ENOTTY)?;
        self.state.lock().permit(c, entry.perm, privileged)?;
        self.driver.ioctl(self, c, &entry, cmd, &arg, ctx)?;
        Ok(0)
    }

    fn segmap_check(&self, minor: Minor, req: &MapRequest, ctx: &Sleepable) -> Result<()> {
        if let Some((_, id)) = anon_fd::decode(minor.0) {
            return self.sync.anon_segmap_check(id, req.off, req.len, req.is_shared(), ctx);
        }
        let c = minor::decode(minor.0).and_then(|d| d.clone).ok_or(Errno::ENXIO)?;
        let g = self.gem.as_ref().ok_or(Errno::ENODEV)?;
        g.map_check(c, req.off, req.len, req.is_shared())
    }
}

impl<D: Driver> Device<D> {
    /// A mode-setting ioctl: the permission check under the device mutex,
    /// then the KMS layer (`EOPNOTSUPP` for a device without display), or,
    /// for a Linux-KMS device, the driver's KMS code with the kernel copy
    /// of the argument (`cmd` is the caller's command).
    fn kms_ioctl(
        &self,
        entry: &kmsioctl::KmsEntry,
        cmd: u32,
        c: CloneId,
        privileged: &dyn Fn() -> bool,
        arg: &CoreArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        // A file the KMS code opened, of a device whose mode setting runs;
        // any other file of a Linux-KMS driver's device gets EOPNOTSUPP
        // below, as a device without display.
        if self.lkms.as_ref().is_some_and(|q| q.active() && q.kms_open(c)) {
            let nr = u32::from(drm_uapi::ioc::ioc_nr(entry.cmd));
            return lkms::forward_ioctl(
                nr,
                arg,
                || self.state.lock().permit(c, entry.perm, privileged),
                |nr, data| self.driver.lkms_ioctl(self, c, (entry.cmd, cmd), nr, data, arg.io(), ctx),
            );
        }
        let (caps, master) = {
            let s = self.state.lock();
            s.permit(c, entry.perm, privileged)?;
            (s.file(c).ok_or(Errno::ENXIO)?.caps, s.is_current_master(c))
        };
        let (Some(k), Some(drv)) = (&self.kms, self.driver.kms()) else { return Err(caps::EOPNOTSUPP) };
        let who = Caller { file: c, caps, master, privileged };
        k.ioctl(entry.op, &who, arg, self.gem.as_ref(), drv, ctx)
    }

    fn gem_or(&self, e: Errno) -> Result<&KArc<GemCore<D::Object>>, Errno> {
        self.gem.as_ref().ok_or(e)
    }

    /// The GEM state for the dumb-buffer ioctls: `ENOSYS` without
    /// [`Features::DUMB`], as Linux answers for a driver without
    /// `dumb_create`.
    fn dumb_gem(&self) -> Result<&KArc<GemCore<D::Object>>, Errno> {
        if !self.driver.info().features.contains(Features::DUMB) {
            return Err(ENOSYS);
        }
        self.gem_or(ENOSYS)
    }

    fn core_ioctl(
        &self,
        entry: &Entry,
        c: CloneId,
        privileged: &dyn Fn() -> bool,
        arg: &CoreArg<'_>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let perm = entry.perm;
        match entry.op {
            Op::Version => self.version(c, perm, privileged, arg)?,
            Op::GetUnique => self.get_unique(c, perm, privileged, arg)?,
            Op::SetUnique => self.set_unique(c, perm, privileged, arg)?,
            Op::GetMagic => {
                let magic = {
                    let mut s = self.state.lock();
                    s.permit(c, perm, privileged)?;
                    s.get_magic(c, ctx)?
                };
                let mut a = UserArg::<uapi::drm_auth>::zeroed();
                // SAFETY: a field assignment.
                unsafe { a.get_mut() }.magic = magic;
                a.write(arg)?;
            }
            Op::AuthMagic => {
                let a = UserArg::<uapi::drm_auth>::read(arg)?;
                let mut s = self.state.lock();
                s.permit(c, perm, privileged)?;
                s.auth_magic(c, a.get().magic)?;
            }
            Op::GetClient => {
                let mut cl = UserArg::<uapi::drm_client>::read(arg)?;
                let auth = {
                    let s = self.state.lock();
                    s.permit(c, perm, privileged)?;
                    s.file(c).ok_or(Errno::ENXIO)?.authenticated
                };
                // Index 0 is the caller itself, the only client current Linux
                // reports. As there, the pid is the caller's and the uid is
                // the anonymous one, so that a process that received the file
                // learns nothing about who opened it.
                if cl.get().idx != 0 {
                    return Err(Errno::EINVAL);
                }
                // SAFETY: only field assignments below.
                let a = unsafe { cl.get_mut() };
                a.auth = i32::from(auth);
                a.pid = u64::try_from(caller_pid()).unwrap_or(0);
                a.uid = UID_NOBODY;
                a.magic = 0;
                a.iocs = 0;
                cl.write(arg)?;
            }
            Op::SetVersion => {
                let mut v = UserArg::<uapi::drm_set_version>::read(arg)?;
                let sv = {
                    let a = v.get();
                    SetVersion {
                        di_major: a.drm_di_major,
                        di_minor: a.drm_di_minor,
                        dd_major: a.drm_dd_major,
                        dd_minor: a.drm_dd_minor,
                    }
                };
                let r = {
                    let mut s = self.state.lock();
                    s.permit(c, perm, privileged)?;
                    s.set_version(c, sv, self.driver.info().major, self.driver.info().minor)
                };
                // The versions go back to the caller whether or not the
                // request was acceptable; libdrm reads them in both cases.
                // SAFETY: only field assignments below.
                let a = unsafe { v.get_mut() };
                a.drm_di_major = IF_MAJOR;
                a.drm_di_minor = IF_MINOR;
                a.drm_dd_major = self.driver.info().major;
                a.drm_dd_minor = self.driver.info().minor;
                v.write(arg)?;
                r?;
            }
            Op::GetCap => {
                let mut g = UserArg::<uapi::drm_get_cap>::read(arg)?;
                self.state.lock().permit(c, perm, privileged)?;
                let (features, mode) = self.caps();
                let value = caps::get_cap(features, &mode, g.get().capability)?;
                // SAFETY: a field assignment.
                unsafe { g.get_mut() }.value = value;
                g.write(arg)?;
            }
            Op::SetClientCap => {
                let a = UserArg::<uapi::drm_set_client_cap>::read(arg)?;
                let features = self.caps().0;
                let mut s = self.state.lock();
                s.permit(c, perm, privileged)?;
                let f = s.file_mut(c).ok_or(Errno::ENXIO)?;
                f.caps.set(features, a.get().capability, a.get().value)?;
                self.mirror(&s, c);
            }
            Op::SetMaster => {
                let mut s = self.state.lock();
                s.permit(c, perm, privileged)?;
                s.set_master(c, privileged, caller_pid(), ctx)?;
                self.mirror(&s, c);
            }
            Op::DropMaster => {
                let mut s = self.state.lock();
                s.permit(c, perm, privileged)?;
                s.drop_master(c, privileged, caller_pid())?;
                self.mirror(&s, c);
            }
            Op::GemClose => {
                let a = UserArg::<uapi::drm_gem_close>::read(arg)?;
                self.state.lock().permit(c, perm, privileged)?;
                let g = self.gem_or(caps::EOPNOTSUPP)?;
                // `pad` is ignored, as in Linux's drm_gem_close_ioctl: the
                // kernel has never checked it, so userland need not clear it.
                g.handle_close(c, a.get().handle, ctx)?;
            }
            Op::GemFlink => {
                // `name` is an output; its input value is ignored, as in Linux.
                let mut a = UserArg::<uapi::drm_gem_flink>::read(arg)?;
                self.state.lock().permit(c, perm, privileged)?;
                let g = self.gem_or(caps::EOPNOTSUPP)?;
                let name = g.flink(c, a.get().handle, ctx)?;
                // SAFETY: a field assignment.
                unsafe { a.get_mut() }.name = name;
                a.write(arg)?;
            }
            Op::GemOpen => {
                // `handle` and `size` are outputs, ignored on input as in Linux.
                let mut a = UserArg::<uapi::drm_gem_open>::read(arg)?;
                self.state.lock().permit(c, perm, privileged)?;
                let g = self.gem_or(caps::EOPNOTSUPP)?;
                let (handle, size) = g.open_name(c, a.get().name, ctx)?;
                // SAFETY: only field assignments below.
                let m = unsafe { a.get_mut() };
                m.handle = handle;
                m.size = size;
                a.write(arg)?;
            }
            Op::CreateDumb => {
                // `handle`, `pitch` and `size` are outputs; whatever the
                // caller left in them is overwritten, as in Linux, which
                // cannot refuse garbage there because old userland leaves it.
                let mut a = UserArg::<uapi::drm_mode_create_dumb>::read(arg)?;
                self.state.lock().permit(c, perm, privileged)?;
                self.dumb_gem()?;
                let req = {
                    let v = a.get();
                    DumbRequest { width: v.width, height: v.height, bpp: v.bpp, flags: v.flags }
                };
                let made = self.driver.dumb_create(self, c, &req, ctx)?;
                // SAFETY: only field assignments below.
                let m = unsafe { a.get_mut() };
                m.handle = made.handle;
                m.pitch = made.pitch;
                m.size = made.size;
                a.write(arg)?;
            }
            Op::MapDumb => {
                let mut a = UserArg::<uapi::drm_mode_map_dumb>::read(arg)?;
                self.state.lock().permit(c, perm, privileged)?;
                self.dumb_gem()?;
                // `pad` is ignored, as in Linux's drm_mode_mmap_dumb_ioctl.
                let off = self.driver.dumb_map_offset(self, c, a.get().handle, ctx);
                // SAFETY: a field assignment.
                unsafe { a.get_mut() }.offset = off?;
                a.write(arg)?;
            }
            Op::DestroyDumb => {
                let a = UserArg::<uapi::drm_mode_destroy_dumb>::read(arg)?;
                self.state.lock().permit(c, perm, privileged)?;
                let g = self.dumb_gem()?;
                g.handle_close(c, a.get().handle, ctx)?;
            }
            // R2: PRIME and sync objects (crate::prime, crate::syncobj_ioctl).
            op @ (Op::PrimeHandleToFd | Op::PrimeFdToHandle) => {
                self.state.lock().permit(c, perm, privileged)?;
                self.sync.prime_ioctl(op, c, self.driver.info().features, self, arg, ctx)?;
            }
            op @ (Op::SyncobjCreate
            | Op::SyncobjDestroy
            | Op::SyncobjHandleToFd
            | Op::SyncobjFdToHandle
            | Op::SyncobjWait
            | Op::SyncobjReset
            | Op::SyncobjSignal
            | Op::SyncobjTimelineWait
            | Op::SyncobjQuery
            | Op::SyncobjTransfer
            | Op::SyncobjTimelineSignal) => {
                self.state.lock().permit(c, perm, privileged)?;
                self.sync.syncobj_ioctl(op, c, self.driver.info().features, arg, ctx)?;
            }
        }
        Ok(())
    }
}
