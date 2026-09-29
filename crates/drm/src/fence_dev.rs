// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The synchronisation state of a device (milestone R2): its fence domain,
//! the sync object handles of each open file, its anonymous files
//! (dma-bufs, sync object files, sync files), the `sync` kstat, and the
//! entry points of anonymous minors.
//!
//! The ioctls live next to their objects: [`crate::syncobj_ioctl`] for the
//! sync object ioctls, [`crate::prime`] for PRIME; the sync file and
//! dma-buf ioctls are here, since they are ioctls on anonymous files.
//!
//! # Locks
//!
//! Each file's handle table has a mutex; the table of files and the
//! anonymous-file table are leaves. No reference is dropped with one of
//! them held when dropping may block (sync objects and GEM references are
//! put after the lock is released). Fence and sync object locks are taken
//! inside, never around, these.

use ddi::prelude::*;
use ddi::private::fd::{FdFlags, FileRef};

use crate::anon_fd::{AnonFiles, AnonObj};
use crate::defer::Ctx;
use crate::fence::{Fence, FenceDomain};
use crate::gem::{GemCore, GemObject, KIdSpace, NameBuf};
use crate::idmap::IdMap;
use crate::karc::KArc;
use crate::ksync::KMutex;
use crate::kvec::KVec;
use crate::minor::CloneId;
use crate::resv::Usage;
use crate::sync_file::{self, DmaBufSync, SyncFenceInfo, SyncFile, SyncFileInfo, SyncMergeData};
use crate::syncobj::Syncobj;
use crate::uaccess::{Plain, UserArg, copy_out_array};

// SAFETY: integer fields only, no padding (sizes asserted in sync_file).
unsafe impl Plain for SyncMergeData {}
// SAFETY: as above.
unsafe impl Plain for SyncFileInfo {}
// SAFETY: as above.
unsafe impl Plain for SyncFenceInfo {}
// SAFETY: as above.
unsafe impl Plain for DmaBufSync {}

/// `ENOTSUP`, a dma-buf of another device.
const ENOTSUP: Errno = Errno::ENOTSUP;

/// `ENODEV`, `mmap(2)` of a file that cannot be mapped.
const ENODEV: Errno = match Errno::new(ddi::sys::errno::ENODEV) {
    Some(e) => e,
    None => Errno::EINVAL,
};

/// The sync object handles of one open file.
pub(crate) struct FileSync {
    pub(crate) objs: KMutex<IdMap<Syncobj, KIdSpace>>,
}

/// The synchronisation state of one device. See the module documentation.
pub struct SyncCore<O: GemObject> {
    pub(crate) domain: KArc<FenceDomain>,
    pub(crate) dev: DevInfo,
    index: u32,
    files: KMutex<KVec<Option<KArc<FileSync>>>>,
    pub(crate) anon: AnonFiles<O>,
    /// A head that is never woken, registered by anonymous files that are
    /// always ready (sync object files), for pollers that asked for other
    /// events.
    idle: PollBox,
}

impl<O: GemObject> SyncCore<O> {
    /// Creates the state of device `index` of the driver of `dev`; `pri` is
    /// the highest interrupt priority its fences are signalled at
    /// ([`crate::Driver::fence_priority`]).
    pub(crate) fn new(
        dev: DevInfo,
        index: u32,
        pri: Option<IntrPriority>,
        ctx: &Sleepable,
    ) -> Result<KArc<SyncCore<O>>, Errno> {
        let domain = FenceDomain::new(dev, pri, ctx)?;
        let anon = AnonFiles::new(&dev, index, ctx)?;
        let idle = PollBox::new(ctx);
        let core = KArc::new(SyncCore { domain, dev, index, files: KMutex::new(KVec::new()), anon, idle }, ctx)?;
        if !registry::insert(index, &*core) {
            return Err(Errno::EEXIST);
        }
        Ok(core)
    }

    /// Looks up descriptor `fd` of the calling process: the object if it
    /// is an anonymous file of any device of this driver (they share this
    /// code, so a sync object or sync file of one device can be imported
    /// into another), `EINVAL` for any other file, `EBADF` for a closed
    /// descriptor. With `local_only` (dma-bufs, whose import across
    /// devices needs an attach the core does not have), another device's
    /// file is `ENOTSUP`; so is any file of a device whose fences are
    /// signalled at another interrupt priority ([`crate::Driver::fence_priority`]),
    /// because the locks waiting on them are created with this device's.
    pub(crate) fn lookup(
        &self,
        fd: i32,
        arg: &IoctlArg<'_>,
        local_only: bool,
        ctx: &Sleepable,
    ) -> Result<AnonObj<O>, Errno> {
        let f = FileRef::get(fd, arg)?;
        let m = f.own_minor(&self.dev).ok_or(Errno::EINVAL)?;
        let (index, id) = crate::anon_fd::decode(m.0).ok_or(Errno::EINVAL)?;
        let obj = if index == self.index {
            self.anon.get(id)
        } else {
            // SAFETY: the held file is an open file of device `index`, which
            // therefore cannot detach (and unregister) during this call.
            let other = unsafe { registry::get::<O>(index) }.ok_or(Errno::EINVAL)?;
            // The other device's fences are signalled at its priority, and
            // the locks this device creates for waiting on them (waiters,
            // chain points, arrays) use this device's: both must agree.
            if other.domain.priority() != self.domain.priority() {
                return Err(ENOTSUP);
            }
            other.anon.get(id)
        };
        drop(f);
        let obj = obj.ok_or(Errno::EINVAL)?;
        if index != self.index && local_only {
            obj.put(ctx);
            return Err(ENOTSUP);
        }
        Ok(obj)
    }

    /// Returns the fence domain, for drivers that create fences.
    pub fn domain(&self) -> &KArc<FenceDomain> {
        &self.domain
    }

    /// Copies the counters to the `sync` kstat. Thread context.
    pub fn publish(&self) {
        self.domain.publish();
    }

    /// The work every thread-context entry point does first.
    pub(crate) fn housekeeping(&self, ctx: &Sleepable) {
        self.domain.drain(ctx);
    }

    /// Drops what is left in the anonymous-file table before detach (after
    /// every file has closed nothing is).
    pub(crate) fn clear(&self, ctx: &Sleepable) {
        self.anon.clear(&self.domain.stats, ctx);
        self.publish();
    }

    /// Prepares for detach once every file is closed and the GEM objects
    /// are gone: fails if fences or objects remain (a driver that did not
    /// signal its fences).
    pub(crate) fn quiesce(this: &KArc<SyncCore<O>>, ctx: &Sleepable) -> Result<(), [u64; 8]> {
        let r = FenceDomain::quiesce(&this.domain, ctx);
        this.publish();
        r
    }

    // --- files ---

    /// Creates the sync object table of a newly opened file.
    pub(crate) fn file_open(&self, clone: CloneId, ctx: &Sleepable) -> Result<(), Errno> {
        let drv = self.dev.driver_name().to_str().map_err(|_| Errno::EINVAL)?;
        let name = NameBuf::new(format_args!("{drv}{}.s{}", self.dev.instance().0, clone.get()))?;
        let ids = KIdSpace::with_range(name.as_cstr(), 1, i32::MAX, ctx)?;
        let fs = KArc::new(FileSync { objs: KMutex::new(IdMap::new(ids)) }, ctx)?;
        let mut files = self.files.lock();
        while files.len() <= clone.index() {
            files.push(None, ctx)?;
        }
        match files.get_mut(clone.index()) {
            Some(slot @ None) => {
                *slot = Some(fs);
                Ok(())
            }
            _ => Err(Errno::EBUSY),
        }
    }

    /// Drops the sync object handles of a closed file.
    pub(crate) fn file_close(&self, clone: CloneId, ctx: &Sleepable) {
        self.housekeeping(ctx);
        let fs = self.files.lock().get_mut(clone.index()).and_then(Option::take);
        if let Some(fs) = fs {
            loop {
                let Some((_, s)) = fs.objs.lock().pop_first() else { break };
                drop(s);
            }
        }
        self.publish();
    }

    pub(crate) fn file(&self, clone: CloneId) -> Result<KArc<FileSync>, Errno> {
        self.files.lock().get(clone.index()).and_then(Option::as_ref).cloned().ok_or(Errno::ENXIO)
    }

    // --- anonymous minors ---

    /// `open(9E)` of anonymous minor `id`.
    pub(crate) fn anon_open(&self, id: u32) -> Result<(), Errno> {
        self.anon.open(id)
    }

    /// `close(9E)` of anonymous minor `id`.
    pub(crate) fn anon_close(&self, id: u32, ctx: &Sleepable) {
        self.housekeeping(ctx);
        self.anon.close(id, &self.domain.stats, ctx);
        self.publish();
    }

    /// `chpoll(9E)` of anonymous minor `id`: a sync file is readable once
    /// its fence has signalled; a sync object file is always ready (it has
    /// no poll semantics, as on Linux). Polling a dma-buf (implicit
    /// synchronisation through its reservation) is not supported yet:
    /// `ENOTSUP`.
    pub(crate) fn anon_chpoll<'a>(&'a self, id: u32) -> Result<(PollEvents, PollRegistration<'a>), Errno> {
        match self.anon.get(id).ok_or(Errno::ENXIO)? {
            AnonObj::SyncFile(sf) => {
                // SAFETY: the anonymous file being polled holds the sync
                // file in the table until its close(9E), and the poll
                // framework holds that file open until the poller is linked.
                let r = unsafe { sf.chpoll() };
                // The file holds the object open; this is not the last
                // reference, and dropping a KArc in thread context is
                // allowed (chpoll runs in the polling thread).
                drop(sf);
                Ok(r)
            }
            AnonObj::Syncobj(s) => {
                drop(s);
                Ok((PollEvents::IN.union(PollEvents::OUT).union(PollEvents::RDNORM), self.idle.head().registration()))
            }
            AnonObj::DmaBuf(g) => {
                drop(g);
                Err(Errno::ENOTSUP)
            }
        }
    }

    /// `ioctl(9E)` of anonymous minor `id`: `SYNC_IOC_MERGE` and
    /// `SYNC_IOC_FILE_INFO` on sync files, `DMA_BUF_IOCTL_SYNC` on
    /// dma-bufs, `ENOTTY` for anything else.
    pub(crate) fn anon_ioctl(&self, id: u32, cmd: u32, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        self.housekeeping(ctx);
        let obj = self.anon.get(id).ok_or(Errno::ENXIO)?;
        let r = match (&obj, cmd) {
            (AnonObj::SyncFile(sf), sync_file::SYNC_IOC_MERGE) => self.sync_merge(sf, arg, ctx),
            (AnonObj::SyncFile(sf), sync_file::SYNC_IOC_FILE_INFO) => self.sync_info(sf, arg, ctx),
            (AnonObj::DmaBuf(g), sync_file::DMA_BUF_IOCTL_SYNC) => {
                // The memory is cache-coherent system memory, so there is
                // nothing to flush. The start of CPU access waits for the
                // GPU work on the buffer, as Linux's
                // dma_buf_begin_cpu_access does: for reading, the writers;
                // for writing, the readers too; interruptibly (EINTR).
                let s = UserArg::<DmaBufSync>::read(arg)?;
                let f = s.get().flags;
                if f & !sync_file::DMA_BUF_SYNC_VALID != 0 || f & sync_file::DMA_BUF_SYNC_RW == 0 {
                    Err(Errno::EINVAL)
                } else if f & sync_file::DMA_BUF_SYNC_END != 0 {
                    Ok(())
                } else {
                    let usage = if f & sync_file::DMA_BUF_SYNC_WRITE != 0 { Usage::Read } else { Usage::Write };
                    g.resv().wait(usage, true, None, ctx)
                }
            }
            _ => Err(Errno::ENOTTY),
        };
        obj.put(ctx);
        self.publish();
        r
    }

    /// Returns the sync object behind `handle` of file `c`: `ENOENT` if
    /// there is none. For drivers that attach fences to sync objects.
    pub fn syncobj(&self, c: CloneId, handle: u32) -> Result<Syncobj, Errno> {
        let f = self.file(c)?;
        let t = f.objs.lock();
        t.get(handle).cloned().ok_or(Errno::ENOENT)
    }

    /// Makes a sync file of `fence` and hands it out to the calling
    /// process as a close-on-exec descriptor, from inside an ioctl;
    /// `write_fd` copies the number out before the descriptor becomes
    /// visible. For drivers whose submissions return an out-fence.
    pub fn export_sync_file(
        &self,
        fence: Fence,
        name: &[u8],
        arg: &IoctlArg<'_>,
        write_fd: impl FnOnce(i32) -> Result<(), Errno>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let sf = match SyncFile::new(fence.clone(), name, ctx) {
            Ok(sf) => sf,
            Err(e) => {
                fence.put(Ctx::Sleep(ctx));
                return Err(e);
            }
        };
        fence.put(Ctx::Sleep(ctx));
        let flags = FdFlags { write: false, cloexec: true };
        self.anon.export(&self.dev, AnonObj::SyncFile(sf), flags, arg, write_fd, &self.domain.stats, ctx)
    }

    /// Returns the fence of sync file descriptor `fd`; `EINVAL` for another
    /// kind of file.
    pub(crate) fn sync_file_fence(&self, fd: i32, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<Fence, Errno> {
        match self.lookup(fd, arg, false, ctx)? {
            AnonObj::SyncFile(sf) => {
                let f = sf.fence().clone();
                SyncFile::put(sf, ctx);
                Ok(f)
            }
            o => {
                o.put(ctx);
                Err(Errno::EINVAL)
            }
        }
    }

    fn sync_merge(&self, sf: &KArc<SyncFile>, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let mut d = UserArg::<SyncMergeData>::read(arg)?;
        if d.get().flags != 0 || d.get().pad != 0 {
            return Err(Errno::EINVAL);
        }
        // Linux answers ENOENT when fd2 is not a sync file.
        let other = self.sync_file_fence(d.get().fd2, arg, ctx).map_err(|e| {
            if e == Errno::EBADF || e == Errno::EINVAL || e == Errno::ENOTSUP { Errno::ENOENT } else { e }
        })?;
        let merged = sync_file::merge(&self.domain, sf.fence(), &other, ctx);
        other.put(Ctx::Sleep(ctx));
        let merged = merged?;
        let name = d.get().name;
        self.export_sync_file(
            merged,
            &name,
            arg,
            |fd| {
                // SAFETY: a field assignment.
                unsafe { d.get_mut() }.fence = fd;
                d.write(arg)
            },
            ctx,
        )
    }

    fn sync_info(&self, sf: &KArc<SyncFile>, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let mut info = UserArg::<SyncFileInfo>::read(arg)?;
        if info.get().flags != 0 || info.get().pad != 0 {
            return Err(Errno::EINVAL);
        }
        let fs = sync_file::members(sf.fence());
        let n = u32::try_from(fs.len()).map_err(|_| Errno::EINVAL)?;
        let room = info.get().num_fences;
        if room != 0 {
            if room < n {
                return Err(Errno::EINVAL);
            }
            let mut out: KVec<SyncFenceInfo> = KVec::new();
            for f in fs {
                let timeline = NameBuf::new(format_args!("ctx{}", f.context()))?;
                let name = sync_file::name_field(timeline.as_cstr().to_bytes());
                out.push(
                    SyncFenceInfo {
                        obj_name: name,
                        driver_name: sync_file::name_field(self.dev.driver_name().to_bytes()),
                        status: sync_file::status(f),
                        flags: 0,
                        timestamp_ns: f.timestamp().map_or(0, |t| t as u64),
                    },
                    ctx,
                )?;
            }
            copy_out_array(arg, info.get().sync_fence_info as usize, &out)?;
        }
        // SAFETY: only field assignments below.
        let m = unsafe { info.get_mut() };
        m.num_fences = n;
        m.status = sync_file::status(sf.fence());
        m.name = *sf.name();
        info.write(arg)
    }

    // --- dma-buf mappings ---

    /// `segmap(9E)` of anonymous minor `id`: only a dma-buf maps, shared,
    /// within its size (`EINVAL` otherwise, `ENODEV` for other anonymous
    /// files, as `mmap(2)` of a file without mappings gives).
    pub(crate) fn anon_segmap_check(
        &self,
        id: u32,
        off: u64,
        len: usize,
        shared: bool,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        match self.anon.get(id).ok_or(Errno::ENXIO)? {
            AnonObj::DmaBuf(g) => {
                let size = g.size() as u64;
                g.put(ctx);
                let end = off.checked_add(len as u64).ok_or(Errno::EINVAL)?;
                if !shared || end > size || len == 0 { Err(Errno::EINVAL) } else { Ok(()) }
            }
            o => {
                o.put(ctx);
                Err(ENODEV)
            }
        }
    }

    /// `devmap(9E)` of anonymous minor `id` (a dma-buf).
    pub(crate) fn anon_devmap(
        &self,
        id: u32,
        gem: Option<&KArc<GemCore<O>>>,
        dhp: &DevmapHandle<'_>,
        off: u64,
        len: usize,
        ctx: &Sleepable,
    ) -> Result<usize, Errno> {
        let gem = gem.ok_or(Errno::ENXIO)?;
        self.anon.devmap(id, gem, dhp, off, len, ctx)
    }
}

impl<O: GemObject> Drop for SyncCore<O> {
    /// Leaves the registry, whatever happened (a failed attach drops the
    /// state without a detach).
    fn drop(&mut self) {
        registry::remove(self.index, self);
    }
}

/// The synchronisation state of each device of this driver, for imports
/// across devices ([`SyncCore::lookup`]). Each kernel module links one DRM
/// driver, so each index belongs to one object type; the type is recorded
/// and a mismatch refused, as in `gem::registry`.
mod registry {
    use core::ffi::c_void;
    use core::ptr;
    use core::sync::atomic::{AtomicPtr, AtomicUsize, Ordering};

    use super::SyncCore;
    use crate::gem::GemObject;
    use crate::minor::MAX_DEVICES;

    static CORES: [AtomicPtr<c_void>; MAX_DEVICES as usize] =
        [const { AtomicPtr::new(ptr::null_mut()) }; MAX_DEVICES as usize];
    static TAGS: [AtomicUsize; MAX_DEVICES as usize] = [const { AtomicUsize::new(0) }; MAX_DEVICES as usize];

    fn tag<O: GemObject>() -> usize {
        get::<O> as *const () as usize
    }

    /// Records the state of device `index`; `false` if the slot is taken.
    pub(super) fn insert<O: GemObject>(index: u32, core: &SyncCore<O>) -> bool {
        let (Some(c), Some(t)) = (CORES.get(index as usize), TAGS.get(index as usize)) else { return false };
        let p = core as *const SyncCore<O> as *mut c_void;
        if c.compare_exchange(ptr::null_mut(), p, Ordering::AcqRel, Ordering::Relaxed).is_err() {
            return false;
        }
        t.store(tag::<O>(), Ordering::Release);
        true
    }

    /// Forgets device `index` if `core` is what is recorded there.
    pub(super) fn remove<O: GemObject>(index: u32, core: &SyncCore<O>) {
        if let Some(c) = CORES.get(index as usize) {
            let p = core as *const SyncCore<O> as *mut c_void;
            let _ = c.compare_exchange(p, ptr::null_mut(), Ordering::AcqRel, Ordering::Relaxed);
        }
    }

    /// Finds the state of device `index`.
    ///
    /// # Safety
    /// The caller holds an open file of that device, which keeps it
    /// attached for the duration of the use.
    pub(super) unsafe fn get<'a, O: GemObject>(index: u32) -> Option<&'a SyncCore<O>> {
        let p = CORES.get(index as usize)?.load(Ordering::Acquire);
        if p.is_null() || TAGS.get(index as usize)?.load(Ordering::Acquire) != tag::<O>() {
            return None;
        }
        // SAFETY: recorded by insert from a live SyncCore<O>; the caller's
        // contract keeps it alive.
        Some(unsafe { &*(p as *const SyncCore<O>) })
    }
}
