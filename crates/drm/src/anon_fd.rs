// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! File descriptors for dma-bufs, sync objects and sync files.
//!
//! Linux hands these objects to user space as anonymous files. Here each
//! object that gets a file descriptor gets a minor number of the DRM
//! driver of its own, an *anonymous minor*, and the descriptor is a specfs
//! file for that device number, created by `ddi::private::fd` from inside
//! the ioctl (`makespecvp`, `VOP_OPEN`, `falloc`, `setf`, as `pipe(2)`
//! does). Every later operation on the file reaches the driver's own entry
//! points with that minor: `close(9E)` when the last file of it is closed
//! drops the object, `chpoll(9E)` polls a sync file, `devmap(9E)` maps a
//! dma-buf, `ioctl(9E)` serves the sync file and dma-buf ioctls. Passing
//! the descriptor with `SCM_RIGHTS` or inheriting it through `fork(2)`
//! shares the file as it would any other, and importing it
//! (`PRIME_FD_TO_HANDLE`, `SYNCOBJ_FD_TO_HANDLE`, `IMPORT_SYNC_FILE`)
//! looks the descriptor up with `getf`, checks that it is a character
//! device of this driver, and finds the object by its minor. The driver
//! cannot detach while such a file is open, because specfs holds the
//! device node open. `docs/anon-fd.md` records the choice and the
//! alternatives.
//!
//! # Minor numbers
//!
//! An anonymous minor has bit 30 set, the device index (the instance) in
//! bits 24 to 29 and a number from 1 to 2^24 - 1 in the low 24 bits. DRM
//! file minors use the low 18 bits only ([`crate::minor`]), so the ranges
//! are disjoint, and [`crate::minor::instance_of_minor`] maps both kinds
//! to the instance. The numbers exceed 18 bits, so a 32-bit process that
//! receives such a descriptor gets `EOVERFLOW` from `fstat(2)`; only the
//! LP64 ABI is supported anyway.
//!
//! # Opens
//!
//! The only way to open an anonymous minor is the export path: the entry
//! is created with one pending open, the driver's `open(9E)` accepts the
//! minor only while an open is pending, and the pending count drops when
//! the export returns. A device node made with `mknod(2)` for the number
//! therefore cannot be opened. Exporting a GEM object again while its
//! dma-buf file is open opens the same minor again (one dma-buf per
//! object, as Linux keeps `obj->dma_buf`); if that races with the last
//! close, a new minor is used.

use crate::minor::MAX_DEVICES;

/// The bit that marks an anonymous minor.
pub const ANON_BIT: u32 = 1 << 30;
/// The shift of the device index.
pub const INDEX_SHIFT: u32 = 24;
/// The mask of the object number.
pub const ID_MASK: u32 = (1 << INDEX_SHIFT) - 1;

/// Returns the anonymous minor of object number `id` of device `index`.
pub const fn minor(index: u32, id: u32) -> Option<u32> {
    if index >= MAX_DEVICES || id == 0 || id > ID_MASK {
        return None;
    }
    Some(ANON_BIT | (index << INDEX_SHIFT) | id)
}

/// Takes an anonymous minor apart into the device index and the object
/// number; `None` for any other minor.
pub const fn decode(minor: u32) -> Option<(u32, u32)> {
    if minor & !(ANON_BIT | (0x3f << INDEX_SHIFT) | ID_MASK) != 0 || minor & ANON_BIT == 0 {
        return None;
    }
    let id = minor & ID_MASK;
    if id == 0 {
        return None;
    }
    Some(((minor >> INDEX_SHIFT) & 0x3f, id))
}

#[cfg(not(test))]
pub(crate) use imp::*;

#[cfg(not(test))]
mod imp {
    use core::sync::atomic::Ordering;

    use ddi::prelude::*;
    use ddi::private::fd::{FdFlags, FileRef, NewFd};

    use crate::fence::SyncStats;
    use crate::gem::{GemCore, GemObject, GemRef, KIdSpace, NameBuf};
    use crate::idmap::IdMap;
    use crate::karc::KArc;
    use crate::ksync::KMutex;
    use crate::sync_file::SyncFile;
    use crate::syncobj::Syncobj;

    /// What an anonymous file stands for.
    pub(crate) enum AnonObj<O: GemObject> {
        /// A dma-buf: an exported GEM object.
        DmaBuf(GemRef<O>),
        /// An exported sync object.
        Syncobj(Syncobj),
        /// A sync file.
        SyncFile(KArc<SyncFile>),
    }

    impl<O: GemObject> AnonObj<O> {
        fn same(&self, other: &AnonObj<O>) -> bool {
            match (self, other) {
                (AnonObj::DmaBuf(a), AnonObj::DmaBuf(b)) => crate::objects::ObjRef::same(a, b),
                _ => false,
            }
        }
        fn clone_ref(&self) -> AnonObj<O> {
            match self {
                AnonObj::DmaBuf(g) => AnonObj::DmaBuf(g.clone()),
                AnonObj::Syncobj(s) => AnonObj::Syncobj(s.clone()),
                AnonObj::SyncFile(f) => AnonObj::SyncFile(f.clone()),
            }
        }
        /// Drops the reference in thread context.
        pub(crate) fn put(self, ctx: &Sleepable) {
            match self {
                AnonObj::DmaBuf(g) => g.put(ctx),
                AnonObj::Syncobj(s) => drop(s),
                AnonObj::SyncFile(f) => SyncFile::put(f, ctx),
            }
        }
    }

    struct Entry<O: GemObject> {
        obj: AnonObj<O>,
        /// Opens the export path has asked for and not finished.
        pending: u32,
        /// Whether `open(9E)` succeeded, so that `close(9E)` will come.
        opened: bool,
    }

    /// The anonymous files of one device.
    pub(crate) struct AnonFiles<O: GemObject> {
        index: u32,
        table: KMutex<IdMap<Entry<O>, KIdSpace>>,
    }

    impl<O: GemObject> AnonFiles<O> {
        /// The table of device `index` of the driver of `dev`.
        pub(crate) fn new(dev: &DevInfo, index: u32, ctx: &Sleepable) -> Result<AnonFiles<O>, Errno> {
            let drv = dev.driver_name().to_str().map_err(|_| Errno::EINVAL)?;
            let name = NameBuf::new(format_args!("{drv}{index}.anon"))?;
            let ids = KIdSpace::with_range(name.as_cstr(), 1, super::ID_MASK as i32 + 1, ctx)?;
            Ok(AnonFiles { index, table: KMutex::new(IdMap::new(ids)) })
        }

        /// Hands `obj` to the calling process as a new file descriptor
        /// with `flags`. `write_fd` receives the descriptor number before
        /// it becomes visible (to copy it out); if it fails, everything is
        /// undone. For a dma-buf the object's existing minor is reused while
        /// its file is open.
        #[allow(clippy::too_many_arguments, reason = "the export needs all of them")]
        pub(crate) fn export(
            &self,
            dev: &DevInfo,
            obj: AnonObj<O>,
            flags: FdFlags,
            arg: &IoctlArg<'_>,
            write_fd: impl FnOnce(i32) -> Result<(), Errno>,
            stats: &SyncStats,
            ctx: &Sleepable,
        ) -> Result<(), Errno> {
            let mut obj = Some(obj);
            // Try the object's existing dma-buf first, then a new minor.
            for attempt in 0..2 {
                let Some(o) = obj.take() else { return Err(Errno::EINVAL) };
                let (id, fresh, spare) = match self.reserve(o, attempt == 0, stats, ctx) {
                    Ok(v) => v,
                    Err((o, e)) => {
                        o.put(ctx);
                        return Err(e);
                    }
                };
                let m = super::minor(self.index, id).ok_or(Errno::ENXIO);
                let r = m.and_then(|m| NewFd::open(dev, Minor(m), flags, arg, ctx));
                let leftover = self.settle(id, fresh, r.is_ok());
                if let Some(lo) = leftover {
                    self.dropped(id, lo, stats, ctx);
                }
                match r {
                    Ok(nfd) => {
                        if let Some(s) = spare {
                            s.put(ctx);
                        }
                        write_fd(nfd.fd())?;
                        nfd.install();
                        return Ok(());
                    }
                    // A reused minor that was being closed: try a new one.
                    Err(e) if !fresh && e == Errno::ENXIO => obj = spare,
                    Err(e) => {
                        if let Some(s) = spare {
                            s.put(ctx);
                        }
                        return Err(e);
                    }
                }
            }
            Err(Errno::ENXIO)
        }

        /// Finds or creates the entry for `obj` and books one pending open.
        /// Returns the number, whether the entry is new, and the reference
        /// that was not needed because an entry existed (kept for a retry).
        #[allow(clippy::type_complexity, reason = "private helper")]
        fn reserve(
            &self,
            obj: AnonObj<O>,
            reuse: bool,
            stats: &SyncStats,
            ctx: &Sleepable,
        ) -> Result<(u32, bool, Option<AnonObj<O>>), (AnonObj<O>, Errno)> {
            let mut t = self.table.lock();
            if reuse && let AnonObj::DmaBuf(g) = &obj {
                let id = g.dmabuf().load(Ordering::Acquire);
                if id != 0
                    && let Some(e) = t.get_mut(id)
                    && e.obj.same(&obj)
                {
                    e.pending += 1;
                    return Ok((id, false, Some(obj)));
                }
            }
            let dmabuf = match &obj {
                AnonObj::DmaBuf(g) => Some(g.clone()),
                _ => None,
            };
            let id = match t.insert(Entry { obj, pending: 1, opened: false }, ctx) {
                Ok(id) => id,
                Err((e, err)) => {
                    drop(t);
                    if let Some(g) = dmabuf {
                        g.put(ctx);
                    }
                    return Err((e.obj, err));
                }
            };
            if let Some(g) = dmabuf {
                g.dmabuf().store(id, Ordering::Release);
                SyncStats::inc(&stats.dmabufs);
                drop(t);
                g.put(ctx);
            }
            SyncStats::inc(&stats.anon_files);
            Ok((id, true, None))
        }

        /// Ends a pending open; returns the entry's object if the entry
        /// must go because it never opened.
        fn settle(&self, id: u32, fresh: bool, ok: bool) -> Option<AnonObj<O>> {
            let mut t = self.table.lock();
            let e = t.get_mut(id)?;
            e.pending = e.pending.saturating_sub(1);
            if !ok && fresh && !e.opened && e.pending == 0 {
                return t.remove(id).map(|e| e.obj);
            }
            None
        }

        /// Accounts for an entry that went away and drops its object.
        fn dropped(&self, id: u32, obj: AnonObj<O>, stats: &SyncStats, ctx: &Sleepable) {
            SyncStats::dec(&stats.anon_files);
            if let AnonObj::DmaBuf(g) = &obj {
                let _ = g.dmabuf().compare_exchange(id, 0, Ordering::AcqRel, Ordering::Acquire);
                SyncStats::dec(&stats.dmabufs);
                g.core().dmabuf_closed(id, ctx);
            }
            obj.put(ctx);
        }

        /// `open(9E)` of anonymous minor `id`: allowed only while an export
        /// is opening it.
        pub(crate) fn open(&self, id: u32) -> Result<(), Errno> {
            let mut t = self.table.lock();
            match t.get_mut(id) {
                Some(e) if e.pending > 0 => {
                    e.opened = true;
                    Ok(())
                }
                _ => Err(Errno::ENXIO),
            }
        }

        /// `close(9E)` of anonymous minor `id`: the last file is gone, the
        /// object reference with it.
        pub(crate) fn close(&self, id: u32, stats: &SyncStats, ctx: &Sleepable) {
            let e = self.table.lock().remove(id);
            if let Some(e) = e {
                self.dropped(id, e.obj, stats, ctx);
            }
        }

        /// Returns a reference to the object of anonymous minor `id`.
        pub(crate) fn get(&self, id: u32) -> Option<AnonObj<O>> {
            self.table.lock().get(id).map(|e| e.obj.clone_ref())
        }

        /// Looks up descriptor `fd` of the calling process: the object if it
        /// is an anonymous file of this device, `EINVAL` for any other file
        /// (`EBADF` for a closed descriptor), `ENOTSUP` for an anonymous
        /// file of another device of this driver (importing across devices
        /// is not supported).
        pub(crate) fn lookup(&self, dev: &DevInfo, fd: i32, arg: &IoctlArg<'_>) -> Result<AnonObj<O>, Errno> {
            let f = FileRef::get(fd, arg)?;
            let m = f.own_minor(dev).ok_or(Errno::EINVAL)?;
            let (index, id) = super::decode(m.0).ok_or(Errno::EINVAL)?;
            if index != self.index {
                return Err(Errno::ENOTSUP);
            }
            // The held file keeps the minor open, so the entry exists.
            let r = self.get(id).ok_or(Errno::EINVAL);
            drop(f);
            r
        }

        /// Drops every entry (detach, after every file has closed: nothing
        /// is left then; this only guards against a leak).
        pub(crate) fn clear(&self, stats: &SyncStats, ctx: &Sleepable) {
            loop {
                let e = self.table.lock().pop_first();
                let Some((id, e)) = e else { break };
                self.dropped(id, e.obj, stats, ctx);
            }
        }

        /// Maps the dma-buf of anonymous minor `id` (`devmap(9E)`).
        pub(crate) fn devmap(
            &self,
            id: u32,
            gem: &KArc<GemCore<O>>,
            dhp: &DevmapHandle<'_>,
            off: u64,
            len: usize,
            ctx: &Sleepable,
        ) -> Result<usize, Errno> {
            match self.get(id) {
                Some(AnonObj::DmaBuf(g)) => gem.devmap_dmabuf(id, g, dhp, off, len, ctx),
                Some(o) => {
                    o.put(ctx);
                    Err(Errno::ENXIO)
                }
                None => Err(Errno::ENXIO),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::minor;

    #[test]
    fn anonymous_minors_are_disjoint_from_file_minors() {
        let m = minor(3, 77).unwrap();
        assert_eq!(decode(m), Some((3, 77)));
        assert_eq!(minor(63, ID_MASK).and_then(decode), Some((63, ID_MASK)));
        assert_eq!(minor(64, 1), None);
        assert_eq!(minor(0, 0), None);
        assert_eq!(minor(0, ID_MASK + 1), None);
        assert!(minor::decode(m).is_none(), "not a DRM file minor");
        for f in [
            0,
            128,
            0x3fe00 | 5,
            minor::file_minor(minor::NodeKind::Render, 2, minor::CloneId::new(511).unwrap()).unwrap(),
        ] {
            assert_eq!(decode(f), None, "{f:#x}");
        }
        assert_eq!(decode(ANON_BIT), None, "number 0 is never used");
        assert_eq!(decode(m | (1 << 31)), None);
        assert_eq!(minor::instance_of_minor(ddi::Minor(m)), Some(ddi::Instance(3)));
    }
}
