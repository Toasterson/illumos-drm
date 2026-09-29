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
//! close, the booked open keeps the entry and its number ([`AnonTable`]),
//! and the export opens the same object again.
//!
//! Sync object files and sync files of one device can be imported into
//! another device of the same driver (see `fence_dev::SyncCore::lookup`);
//! dma-bufs cannot (`ENOTSUP`).

use ddi::{Errno, Sleepable};

use crate::idmap::{IdAlloc, IdMap};
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

/// One anonymous minor's entry: the object and the opens in flight.
struct Entry<V> {
    obj: V,
    /// Opens the export path has booked and not finished.
    pending: u32,
    /// Whether the minor is open, so that its `close(9E)` will come.
    opened: bool,
}

/// The table of a device's anonymous minors, as plain logic (the kernel
/// layer keeps it behind a mutex and adds the file descriptors).
///
/// An entry is created by an export with one booked open, and its number
/// stays taken until the entry goes. `open(9E)` succeeds only while an
/// open is booked. `close(9E)` of the last file removes the entry, except
/// while an export has booked another open of the same number: then the
/// entry and its number stay, so that the booked open (which specfs runs
/// after the close) finds the same object, and [`AnonTable::settle`]
/// removes it if that open fails. Without this, a close racing with a
/// second export of the same dma-buf could free the number, a third export
/// could take it for another object, and the second export's open would
/// land on that object.
pub struct AnonTable<V, A: IdAlloc> {
    map: IdMap<Entry<V>, A>,
}

impl<V, A: IdAlloc> AnonTable<V, A> {
    /// An empty table drawing numbers from `ids`.
    pub const fn new(ids: A) -> AnonTable<V, A> {
        AnonTable { map: IdMap::new(ids) }
    }

    /// Creates an entry for `obj` with one booked open and returns its
    /// number; the object comes back with the error if no number is left.
    pub fn insert(&mut self, obj: V, ctx: &Sleepable) -> Result<u32, (V, Errno)> {
        self.map.insert(Entry { obj, pending: 1, opened: false }, ctx).map_err(|(e, err)| (e.obj, err))
    }

    /// Books another open of entry `id` if it exists and `same` accepts its
    /// object (a second export of the same dma-buf).
    pub fn book(&mut self, id: u32, same: impl FnOnce(&V) -> bool) -> bool {
        match self.map.get_mut(id) {
            Some(e) if same(&e.obj) => {
                e.pending = e.pending.saturating_add(1);
                true
            }
            _ => false,
        }
    }

    /// `open(9E)` of `id`: allowed only while an open is booked.
    pub fn open(&mut self, id: u32) -> Result<(), Errno> {
        match self.map.get_mut(id) {
            Some(e) if e.pending > 0 => {
                e.opened = true;
                Ok(())
            }
            _ => Err(Errno::ENXIO),
        }
    }

    /// Ends a booked open of `id` that succeeded (`ok`) or failed; returns
    /// the object if the entry went because no file has it open and no
    /// other open is booked.
    pub fn settle(&mut self, id: u32, ok: bool) -> Option<V> {
        let e = self.map.get_mut(id)?;
        e.pending = e.pending.saturating_sub(1);
        if !ok && !e.opened && e.pending == 0 {
            return self.map.remove(id).map(|e| e.obj);
        }
        None
    }

    /// `close(9E)` of `id`, the last file of the minor: returns the object
    /// of the entry, which goes, unless an open is booked (see the type
    /// documentation).
    pub fn close(&mut self, id: u32) -> Option<V> {
        let e = self.map.get_mut(id)?;
        if e.pending > 0 {
            e.opened = false;
            return None;
        }
        self.map.remove(id).map(|e| e.obj)
    }

    /// Returns the object of entry `id`.
    pub fn get(&self, id: u32) -> Option<&V> {
        self.map.get(id).map(|e| &e.obj)
    }

    /// Removes any entry, for teardown.
    pub fn pop_first(&mut self) -> Option<(u32, V)> {
        self.map.pop_first().map(|(id, e)| (id, e.obj))
    }

    /// Returns the number of entries.
    pub fn len(&self) -> usize {
        self.map.len()
    }

    /// Reports whether the table is empty.
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }
}

#[cfg(not(test))]
pub(crate) use imp::*;

#[cfg(not(test))]
mod imp {
    use core::sync::atomic::Ordering;

    use ddi::prelude::*;
    use ddi::private::fd::{FdFlags, NewFd};

    use crate::fence::SyncStats;
    use crate::gem::{GemCore, GemObject, GemRef, KIdSpace, NameBuf};
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

    /// The anonymous files of one device.
    pub(crate) struct AnonFiles<O: GemObject> {
        index: u32,
        table: KMutex<super::AnonTable<AnonObj<O>, KIdSpace>>,
    }

    impl<O: GemObject> AnonFiles<O> {
        /// The table of device `index` of the driver of `dev`.
        pub(crate) fn new(dev: &DevInfo, index: u32, ctx: &Sleepable) -> Result<AnonFiles<O>, Errno> {
            let drv = dev.driver_name().to_str().map_err(|_| Errno::EINVAL)?;
            let name = NameBuf::new(format_args!("{drv}{index}.anon"))?;
            let ids = KIdSpace::with_range(name.as_cstr(), 1, super::ID_MASK as i32 + 1, ctx)?;
            Ok(AnonFiles { index, table: KMutex::new(super::AnonTable::new(ids)) })
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
                let leftover = self.table.lock().settle(id, r.is_ok());
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
                    // Cannot happen with the booking rule of AnonTable; kept
                    // as a fallback: a reused minor that failed, try a new
                    // one.
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
                if id != 0 && t.book(id, |o| o.same(&obj)) {
                    return Ok((id, false, Some(obj)));
                }
            }
            let dmabuf = match &obj {
                AnonObj::DmaBuf(g) => Some(g.clone()),
                _ => None,
            };
            let id = match t.insert(obj, ctx) {
                Ok(id) => id,
                Err((o, err)) => {
                    drop(t);
                    if let Some(g) = dmabuf {
                        g.put(ctx);
                    }
                    return Err((o, err));
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
            self.table.lock().open(id)
        }

        /// `close(9E)` of anonymous minor `id`: the last file is gone, the
        /// object reference with it.
        pub(crate) fn close(&self, id: u32, stats: &SyncStats, ctx: &Sleepable) {
            let e = self.table.lock().close(id);
            if let Some(o) = e {
                self.dropped(id, o, stats, ctx);
            }
        }

        /// Returns a reference to the object of anonymous minor `id`.
        pub(crate) fn get(&self, id: u32) -> Option<AnonObj<O>> {
            self.table.lock().get(id).map(AnonObj::clone_ref)
        }

        /// Drops every entry (detach, after every file has closed: nothing
        /// is left then; this only guards against a leak).
        pub(crate) fn clear(&self, stats: &SyncStats, ctx: &Sleepable) {
            loop {
                let e = self.table.lock().pop_first();
                let Some((id, o)) = e else { break };
                self.dropped(id, o, stats, ctx);
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
    extern crate std;
    use super::*;
    use crate::minor;

    use crate::idmap::FirstFit;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::{Arc, Mutex};
    use std::thread;
    use std::vec::Vec;

    fn sleep() -> Sleepable {
        // SAFETY: host test threads may block.
        unsafe { Sleepable::assume() }
    }

    #[test]
    fn booking_rules() {
        let ctx = sleep();
        let mut t: AnonTable<u32, FirstFit> = AnonTable::new(FirstFit::new(1, 100));
        let a = t.insert(7, &ctx).ok().unwrap();
        assert_eq!(t.open(a), Ok(()));
        assert_eq!(t.settle(a, true), None);
        assert_eq!(t.open(a), Err(Errno::ENXIO), "no open booked: a mknod'd node cannot open");
        // A second export books an open; the last close races it.
        assert!(t.book(a, |o| *o == 7));
        assert!(!t.book(a, |o| *o == 8), "another object");
        assert_eq!(t.close(a), None, "the booked open keeps the entry and its number");
        let b = t.insert(8, &ctx).ok().unwrap();
        assert_ne!(a, b, "the number is still taken");
        assert_eq!(t.open(a), Ok(()), "the booked open finds the same object");
        assert_eq!(t.get(a), Some(&7));
        assert_eq!(t.settle(a, true), None);
        assert_eq!(t.close(a), Some(7));
        // A booked open that fails after the last close removes the entry.
        assert!(t.book(b, |_| true));
        assert_eq!(t.close(b), None);
        assert_eq!(t.settle(b, false), None, "the export's own booking is still open");
        assert_eq!(t.settle(b, false), Some(8));
        assert!(t.is_empty());
        // A failed first open of a new entry removes it.
        let c = t.insert(9, &ctx).ok().unwrap();
        assert_eq!(t.settle(c, false), Some(9));
        assert!(t.is_empty());
    }

    /// Exports race last closes as in the kernel: specfs serialises open(9E)
    /// and close(9E) of one number (a mutex per number here) and calls
    /// close(9E) when the last file goes. Eight threads export three objects
    /// again and again, re-using an object's number while its file is open,
    /// and close their files at random. Every file must refer to the object
    /// it was exported for, and every entry must go exactly once.
    #[test]
    fn exports_race_last_close() {
        const OBJECTS: usize = 3;
        struct Spec {
            files: usize,
        }
        let table: Arc<Mutex<AnonTable<usize, FirstFit>>> = Arc::new(Mutex::new(AnonTable::new(FirstFit::new(1, 64))));
        // Per number: the specfs open count (the common snode's lock).
        let spec: Arc<Vec<Mutex<Spec>>> = Arc::new((0..64).map(|_| Mutex::new(Spec { files: 0 })).collect());
        // Per object: its current number, 0 for none (the GEM object's
        // dma-buf field).
        let current: Arc<Vec<AtomicUsize>> = Arc::new((0..OBJECTS).map(|_| AtomicUsize::new(0)).collect());
        let removed = Arc::new(AtomicUsize::new(0));
        let created = Arc::new(AtomicUsize::new(0));
        let hs: Vec<_> = (0..8u64)
            .map(|t| {
                let (table, spec, current, removed, created) =
                    (table.clone(), spec.clone(), current.clone(), removed.clone(), created.clone());
                thread::spawn(move || {
                    let ctx = sleep();
                    let mut seed = t * 31 + 7;
                    let mut files: Vec<(u32, usize)> = Vec::new();
                    for _ in 0..3000 {
                        seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
                        if !files.is_empty() && (files.len() >= 2 || (seed >> 40) % 2 == 0) {
                            let (id, _) = files.swap_remove(((seed >> 20) as usize) % files.len());
                            let mut sp = spec[id as usize].lock().unwrap();
                            sp.files -= 1;
                            if sp.files == 0
                                && let Some(o) = table.lock().unwrap().close(id)
                            {
                                let _ =
                                    current[o].compare_exchange(id as usize, 0, Ordering::AcqRel, Ordering::Acquire);
                                removed.fetch_add(1, Ordering::SeqCst);
                            }
                            continue;
                        }
                        let obj = ((seed >> 33) as usize) % OBJECTS;
                        // Book an open of the object's number, or a new one.
                        let id = {
                            let mut tb = table.lock().unwrap();
                            let cur = current[obj].load(Ordering::Acquire) as u32;
                            if cur != 0 && tb.book(cur, |o| *o == obj) {
                                cur
                            } else {
                                let id = tb.insert(obj, &ctx).ok().unwrap();
                                current[obj].store(id as usize, Ordering::Release);
                                created.fetch_add(1, Ordering::SeqCst);
                                id
                            }
                        };
                        thread::yield_now();
                        // spec_open: serialised with close on the number.
                        let ok = {
                            let mut sp = spec[id as usize].lock().unwrap();
                            let ok = table.lock().unwrap().open(id).is_ok();
                            if ok {
                                sp.files += 1;
                                assert_eq!(table.lock().unwrap().get(id), Some(&obj), "the file is the object's");
                            }
                            ok
                        };
                        assert!(ok, "a booked open always succeeds");
                        if let Some(o) = table.lock().unwrap().settle(id, ok) {
                            let _ = current[o].compare_exchange(id as usize, 0, Ordering::AcqRel, Ordering::Acquire);
                            removed.fetch_add(1, Ordering::SeqCst);
                        }
                        files.push((id, obj));
                    }
                    for (id, obj) in files {
                        let mut sp = spec[id as usize].lock().unwrap();
                        assert_eq!(table.lock().unwrap().get(id), Some(&obj));
                        sp.files -= 1;
                        if sp.files == 0
                            && let Some(o) = table.lock().unwrap().close(id)
                        {
                            let _ = current[o].compare_exchange(id as usize, 0, Ordering::AcqRel, Ordering::Acquire);
                            removed.fetch_add(1, Ordering::SeqCst);
                        }
                    }
                })
            })
            .collect();
        for h in hs {
            h.join().unwrap();
        }
        assert!(table.lock().unwrap().is_empty(), "every entry went");
        assert_eq!(removed.load(Ordering::SeqCst), created.load(Ordering::SeqCst), "exactly once");
        assert!(created.load(Ordering::SeqCst) > OBJECTS, "numbers were recycled");
    }

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
