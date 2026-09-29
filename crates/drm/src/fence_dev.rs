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
use ddi::private::fd::FdFlags;

use crate::anon_fd::{AnonFiles, AnonObj};
use crate::defer::Ctx;
use crate::fence::{Fence, FenceDomain};
use crate::gem::{GemCore, GemObject, KIdSpace, NameBuf};
use crate::idmap::IdMap;
use crate::karc::KArc;
use crate::ksync::KMutex;
use crate::kvec::KVec;
use crate::minor::CloneId;
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

ddi::named_kstat! {
    /// The `sync` kstat of a DRM device (`kstat -m <driver> -n sync`).
    pub struct SyncKstat {
        /// Live fences.
        pub fences: u64,
        /// Fences signalled so far.
        pub signaled: u64,
        /// Fences signalled with an error so far.
        pub errors: u64,
        /// Registered fence callbacks.
        pub callbacks: u64,
        /// Live fence arrays.
        pub arrays: u64,
        /// Live timeline points.
        pub chains: u64,
        /// Fences released by the task queue so far.
        pub deferred: u64,
        /// Runs of the release task so far.
        pub drains: u64,
        /// Blocking waits so far.
        pub waits: u64,
        /// Waits that timed out so far.
        pub timeouts: u64,
        /// Waits a signal interrupted so far.
        pub interrupted: u64,
        /// Live sync objects.
        pub syncobjs: u64,
        /// Live sync files.
        pub sync_files: u64,
        /// Live dma-bufs.
        pub dmabufs: u64,
        /// Anonymous minors in use.
        pub anon_files: u64,
    }
}

/// The sync object handles of one open file.
pub(crate) struct FileSync {
    pub(crate) objs: KMutex<IdMap<Syncobj, KIdSpace>>,
}

/// The synchronisation state of one device. See the module documentation.
pub struct SyncCore<O: GemObject> {
    pub(crate) domain: KArc<FenceDomain>,
    pub(crate) dev: DevInfo,
    files: KMutex<KVec<Option<KArc<FileSync>>>>,
    pub(crate) anon: AnonFiles<O>,
    /// The poll head of the device's DRM files (no events before R3).
    drm_poll: PollHead,
    kstat: Option<TypedKstat<SyncKstat>>,
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
        let kstat = TypedKstat::<SyncKstat>::new(
            ddi::KstatId { module: dev.driver_name(), instance: dev.instance(), name: c"sync", class: c"misc" },
            ctx,
        );
        if kstat.is_err() {
            dev.warn(format_args!("cannot create the sync kstat; statistics are unavailable"));
        }
        let drm_poll = PollHead::new(ctx);
        KArc::new(SyncCore { domain, dev, files: KMutex::new(KVec::new()), anon, drm_poll, kstat: kstat.ok() }, ctx)
    }

    /// Returns the fence domain, for drivers that create fences.
    pub fn domain(&self) -> &KArc<FenceDomain> {
        &self.domain
    }

    /// Copies the counters to the kstat. Thread context.
    pub fn publish(&self) {
        let Some(k) = &self.kstat else { return };
        let s = &self.domain.stats;
        let l = |a: &core::sync::atomic::AtomicU64| a.load(core::sync::atomic::Ordering::Relaxed);
        let mut g = k.lock();
        g.fences.set(l(&s.fences));
        g.signaled.set(l(&s.signaled));
        g.errors.set(l(&s.errors));
        g.callbacks.set(l(&s.callbacks));
        g.arrays.set(l(&s.arrays));
        g.chains.set(l(&s.chains));
        g.deferred.set(l(&s.deferred));
        g.drains.set(l(&s.drains));
        g.waits.set(l(&s.waits));
        g.timeouts.set(l(&s.timeouts));
        g.interrupted.set(l(&s.interrupted));
        g.syncobjs.set(l(&s.syncobjs));
        g.sync_files.set(l(&s.sync_files));
        g.dmabufs.set(l(&s.dmabufs));
        g.anon_files.set(l(&s.anon_files));
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

    /// `chpoll(9E)` of a DRM file: writable, never readable before the
    /// event queue of R3; the device's head is registered so that a poll
    /// for input waits, as on Linux.
    pub(crate) fn drm_file_chpoll(&self, reg: &mut PollRegistration<'_>) -> PollEvents {
        reg.register(&self.drm_poll);
        PollEvents::OUT.union(PollEvents(ddi::sys::POLLWRNORM))
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
    pub(crate) fn anon_chpoll(&self, id: u32, reg: &mut PollRegistration<'_>) -> Result<PollEvents, Errno> {
        match self.anon.get(id).ok_or(Errno::ENXIO)? {
            AnonObj::SyncFile(sf) => {
                let ev = sf.chpoll(reg);
                // The file holds the object open; this is not the last
                // reference, and dropping a KArc in thread context is
                // allowed (chpoll runs in the polling thread).
                drop(sf);
                Ok(ev)
            }
            AnonObj::Syncobj(s) => {
                drop(s);
                Ok(PollEvents::IN.union(PollEvents::OUT).union(PollEvents::RDNORM))
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
            (AnonObj::DmaBuf(_), sync_file::DMA_BUF_IOCTL_SYNC) => {
                // The memory is cache-coherent system memory, so there is
                // nothing to flush; the flags are checked as Linux does.
                let s = UserArg::<DmaBufSync>::read(arg)?;
                let f = s.get().flags;
                if f & !sync_file::DMA_BUF_SYNC_VALID != 0 || f & sync_file::DMA_BUF_SYNC_RW == 0 {
                    Err(Errno::EINVAL)
                } else {
                    Ok(())
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
        match self.anon.lookup(&self.dev, fd, arg)? {
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
