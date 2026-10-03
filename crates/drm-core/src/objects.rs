// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! GEM handles and FLINK names: the rules of Linux's `drm_gem.c`, as plain
//! data so that they can be tested on the host.
//!
//! A GEM object is reference-counted ([`crate::karc::KArc`]). Three kinds
//! of table hold references to it:
//!
//! * the handle table of each open file ([`IdMap`], numbers from 1), one
//!   reference per handle;
//! * the device's name table for `GEM_FLINK`/`GEM_OPEN` (another
//!   [`IdMap`]), one reference per named object;
//! * user mappings and in-flight work, which the kernel layer manages.
//!
//! Every object counts its handles across all files in
//! [`ObjCommon::handle_count`]. As in Linux, the count and the name are
//! only changed with the device's name table locked
//! (`dev->object_name_lock`), and a name exists only while the object has
//! handles: when the last handle goes, the name goes too, and `GEM_OPEN`
//! can no longer find the object even if mappings keep it alive.
//!
//! # Driver hooks
//!
//! A driver may want to know which files use an object (virtio-gpu
//! attaches a resource to the file's rendering context). Linux calls the
//! object's `open` callback for every new handle and `close` for every
//! removed one; here they run once per file and object ([`Hooks`]):
//! `opened` when a file gains its first handle to the object, `closed`
//! when its last one goes. A file may still hold several handles to one
//! object (`GEM_OPEN` and `MODE_GETFB` make a new one each time, as in
//! Linux), but only the first and the last change what the driver set up,
//! so closing a second handle, or importing an object the file holds
//! already, never takes the object away from the file's other handle.
//! Each file's [`FileTable`] has a hook lock that is held across the
//! decision, the hook and the change of the table, so that the hooks of
//! one file and object strictly alternate even when several threads of a
//! process add and remove handles at once. `PRIME_FD_TO_HANDLE` looks for
//! the file's existing handle with the same lock held
//! ([`Locked::find`]), as Linux's per-file PRIME lock does.
//!
//! # Lock order
//!
//! A file's hook lock comes first; the driver's hooks run with it held
//! and no other lock of this module. The name table lock is taken before
//! a file's handle table lock, never after: [`handle_create`] runs with
//! both held (name table first), and closing a handle removes it with only
//! the file's lock held ([`handle_delete`]) and then accounts for it with
//! only the name table lock held ([`handle_released`]). References that
//! come out of the tables are dropped by the caller after the locks are
//! released, because destroying an object takes other locks. The functions
//! here never drop a reference themselves.
//!
//! Adding a handle scans the file's table only for an object that has
//! handles somewhere, and removing one only if the object still has
//! handles, so creating and closing a file's own buffers stays cheap.
//!
//! The tests drive these rules from eight threads with the same locking
//! and check the invariants after every step.

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use ddi::{Errno, Sleepable};

use crate::defer::DeferLink;
use crate::idmap::{IdAlloc, IdMap};
use crate::ksync::{KGuard, KMutex};

/// The part of a GEM object that the core manages.
pub struct ObjCommon {
    /// The size in bytes, a multiple of the page size, fixed for the
    /// object's life.
    size: usize,
    /// The number of handles to the object in all files. Changed only with
    /// the name table locked.
    handle_count: AtomicU32,
    /// The FLINK name, 0 for none. Changed only with the name table locked.
    name: AtomicU32,
    /// The fake mmap offset, 0 until the first `MAP_DUMB` (or driver map
    /// ioctl) assigns one. Set once, with the offset table locked.
    offset: AtomicU64,
    /// The link for deferred destruction.
    pub(crate) link: DeferLink,
}

impl ObjCommon {
    /// The common part of a new object of `size` bytes.
    pub const fn new(size: usize) -> ObjCommon {
        ObjCommon {
            size,
            handle_count: AtomicU32::new(0),
            name: AtomicU32::new(0),
            offset: AtomicU64::new(0),
            link: DeferLink::new(),
        }
    }
    /// Returns the size in bytes.
    pub fn size(&self) -> usize {
        self.size
    }
    /// Returns the number of handles to the object.
    pub fn handle_count(&self) -> u32 {
        self.handle_count.load(Ordering::Relaxed)
    }
    /// Returns the FLINK name, 0 for none.
    pub fn name(&self) -> u32 {
        self.name.load(Ordering::Relaxed)
    }
    /// Returns the fake mmap offset, 0 if none was assigned yet.
    pub fn offset(&self) -> u64 {
        self.offset.load(Ordering::Acquire)
    }
    /// Records the fake mmap offset. The caller holds the offset table
    /// lock and has checked that none is set.
    pub(crate) fn set_offset(&self, off: u64) {
        self.offset.store(off, Ordering::Release);
    }
}

/// A counted reference to a GEM object, as the tables hold it: `KArc` in
/// the tests, `gem::GemRef` in the kernel.
pub trait ObjRef: Sized {
    /// Returns the core's part of the object.
    fn common(&self) -> &ObjCommon;
    /// Takes another reference.
    fn clone_ref(&self) -> Self;
    /// Reports whether both refer to the same object.
    fn same(&self, other: &Self) -> bool;
}

/// Creates a handle to `obj` in `file` and returns it (Linux's
/// `drm_gem_handle_create_tail`). The caller holds the name table lock and
/// then the file's lock. On failure the reference comes back with the
/// error: `ENOMEM` when no handle number is available or the handle count
/// would overflow.
pub fn handle_create<R: ObjRef, A: IdAlloc, B: IdAlloc>(
    _names: &mut IdMap<R, A>,
    file: &mut IdMap<R, B>,
    obj: R,
    ctx: &Sleepable,
) -> Result<u32, (R, Errno)> {
    let c = obj.common();
    let Some(n) = c.handle_count().checked_add(1) else { return Err((obj, Errno::ENOMEM)) };
    c.handle_count.store(n, Ordering::Relaxed);
    match file.insert(obj, ctx) {
        Ok(h) => Ok(h),
        Err((obj, e)) => {
            obj.common().handle_count.store(n - 1, Ordering::Relaxed);
            Err((obj, e))
        }
    }
}

/// Looks up a handle and returns a new reference (Linux's
/// `drm_gem_object_lookup`). The caller holds the file's lock. Unknown
/// handles, 0 included, are `ENOENT`.
pub fn lookup<R: ObjRef, B: IdAlloc>(file: &IdMap<R, B>, handle: u32) -> Result<R, Errno> {
    file.get(handle).map(ObjRef::clone_ref).ok_or(Errno::ENOENT)
}

/// Removes a handle from `file` and returns its reference (the first half
/// of Linux's `drm_gem_handle_delete`). The caller holds only the file's
/// lock, and must then call [`handle_released`] with the name table
/// locked. Unknown handles are `EINVAL`, as in Linux.
pub fn handle_delete<R: ObjRef, B: IdAlloc>(file: &mut IdMap<R, B>, handle: u32) -> Result<R, Errno> {
    file.remove(handle).ok_or(Errno::EINVAL)
}

/// Accounts for a handle to `obj` that was removed from a file (Linux's
/// `drm_gem_object_handle_put_unlocked`). The caller holds the name table
/// lock. When the object has no handles left, its name is removed and the
/// name table's reference is returned for the caller to drop.
pub fn handle_released<R: ObjRef, A: IdAlloc>(names: &mut IdMap<R, A>, obj: &R) -> Option<R> {
    let c = obj.common();
    let n = c.handle_count().saturating_sub(1);
    c.handle_count.store(n, Ordering::Relaxed);
    if n != 0 {
        return None;
    }
    let name = c.name.swap(0, Ordering::Relaxed);
    if name == 0 {
        return None;
    }
    names.remove(name)
}

/// `GEM_FLINK`: returns the object's name, giving it one first if it has
/// none. The caller holds the name table lock and a reference obtained
/// through a handle of the calling file. An object whose handles were all
/// closed in the meantime is `ENOENT`, as in Linux.
pub fn flink<R: ObjRef, A: IdAlloc>(names: &mut IdMap<R, A>, obj: &R, ctx: &Sleepable) -> Result<u32, Errno> {
    let c = obj.common();
    if c.handle_count() == 0 {
        return Err(Errno::ENOENT);
    }
    let name = c.name();
    if name != 0 {
        return Ok(name);
    }
    let name = names.insert(obj.clone_ref(), ctx).map_err(|(_, e)| e)?;
    c.name.store(name, Ordering::Relaxed);
    Ok(name)
}

/// `GEM_OPEN`: returns a new reference to the object named `name`. The
/// caller holds the name table lock, and creates the handle with
/// [`handle_create`] before releasing it. Unknown names, 0 included, are
/// `ENOENT`.
pub fn open<R: ObjRef, A: IdAlloc>(names: &IdMap<R, A>, name: u32) -> Result<R, Errno> {
    names.get(name).map(ObjRef::clone_ref).ok_or(Errno::ENOENT)
}

/// What a driver does when a file starts or stops using an object
/// (Linux's `drm_gem_object_funcs.open` and `.close`), called by
/// [`Locked`] once per file and object, with the file's hook lock held and
/// no other lock of this module.
pub trait Hooks<R> {
    /// The file gains its first handle to `obj`; an error refuses the
    /// handle.
    fn opened(&self, obj: &R) -> Result<(), Errno>;
    /// The file's last handle to `obj` is gone, or the handle for which
    /// [`Hooks::opened`] just succeeded could not be made.
    fn closed(&self, obj: &R);
}

/// The handles of one open file, and the hook lock that keeps the
/// driver's [`Hooks`] in step with them (see the module documentation).
pub struct FileTable<R, B: IdAlloc> {
    /// Held by [`Locked`] for every change of `handles`.
    hooks: KMutex<()>,
    handles: KMutex<IdMap<R, B>>,
}

impl<R: ObjRef, B: IdAlloc> FileTable<R, B> {
    /// An empty table whose handle numbers come from `ids`.
    pub fn new(ids: B) -> FileTable<R, B> {
        FileTable { hooks: KMutex::new(()), handles: KMutex::new(IdMap::new(ids)) }
    }

    /// The handle table, for lookups. Adding and removing handles goes
    /// through [`FileTable::lock`].
    pub fn handles(&self) -> KGuard<'_, IdMap<R, B>> {
        self.handles.lock()
    }

    /// Takes the file's hook lock, which every change of the table needs.
    /// A driver hook must not change the same file's handles, which would
    /// take it again.
    pub fn lock(&self) -> Locked<'_, R, B> {
        Locked { table: self, _hooks: self.hooks.lock() }
    }
}

/// A [`FileTable`] with its hook lock held.
pub struct Locked<'a, R, B: IdAlloc> {
    table: &'a FileTable<R, B>,
    _hooks: KGuard<'a, ()>,
}

impl<R: ObjRef, B: IdAlloc> Locked<'_, R, B> {
    /// The file's handle to an object satisfying `pred`, if it has one
    /// (`PRIME_FD_TO_HANDLE`'s lookup). `pred` runs with the handle table
    /// locked and must not call into the GEM state.
    pub fn find(&self, pred: impl Fn(&R) -> bool) -> Option<u32> {
        self.table.handles.lock().iter().find(|(_, r)| pred(r)).map(|(n, _)| n)
    }

    /// Whether the file holds a handle to `obj`.
    pub fn holds(&self, obj: &R) -> bool {
        self.find(|r| r.same(obj)).is_some()
    }

    /// Creates a new handle to `obj` (Linux's `drm_gem_handle_create_tail`)
    /// and returns it. If the file holds no handle to the object yet,
    /// [`Hooks::opened`] runs first; if the handle cannot be made after
    /// that, [`Hooks::closed`] undoes it. A file that holds the object
    /// already gets another handle and no hook runs, so a failure cannot
    /// undo what the existing handle set up. On failure the reference
    /// comes back with the error.
    pub fn add<A: IdAlloc>(
        &self,
        names: &KMutex<IdMap<R, A>>,
        obj: R,
        hooks: &impl Hooks<R>,
        ctx: &Sleepable,
    ) -> Result<u32, (R, Errno)> {
        // An object without handles is held by no file; only an object
        // with handles needs the scan of the file's table.
        let first = obj.common().handle_count() == 0 || !self.holds(&obj);
        if first && let Err(e) = hooks.opened(&obj) {
            return Err((obj, e));
        }
        let r = {
            let mut n = names.lock();
            let mut h = self.table.handles.lock();
            handle_create(&mut n, &mut h, obj, ctx)
        };
        r.map_err(|(obj, e)| {
            if first {
                hooks.closed(&obj);
            }
            (obj, e)
        })
    }

    /// Removes `handle` (`EINVAL` if there is none) and accounts for it
    /// in the name table ([`handle_released`]); if it was the file's last
    /// handle to its object, [`Hooks::closed`] runs. Returns the handle's
    /// reference and the name table's, if the name went, for the caller to
    /// drop.
    pub fn remove<A: IdAlloc>(
        &self,
        names: &KMutex<IdMap<R, A>>,
        handle: u32,
        hooks: &impl Hooks<R>,
    ) -> Result<(R, Option<R>), Errno> {
        let r = handle_delete(&mut self.table.handles.lock(), handle)?;
        Ok(self.released(names, r, hooks))
    }

    /// Removes the handle with the lowest number, as [`Locked::remove`]
    /// does, or returns `None` if the table is empty (`close(9E)`).
    pub fn pop<A: IdAlloc>(&self, names: &KMutex<IdMap<R, A>>, hooks: &impl Hooks<R>) -> Option<(R, Option<R>)> {
        let (_, r) = self.table.handles.lock().pop_first()?;
        Some(self.released(names, r, hooks))
    }

    fn released<A: IdAlloc>(&self, names: &KMutex<IdMap<R, A>>, r: R, hooks: &impl Hooks<R>) -> (R, Option<R>) {
        let name_ref = handle_released(&mut names.lock(), &r);
        // The file's table cannot change while its hook lock is held: with
        // no handle left anywhere, it holds none either.
        if r.common().handle_count() == 0 || !self.holds(&r) {
            hooks.closed(&r);
        }
        (r, name_ref)
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use crate::idmap::FirstFit;
    use crate::karc::KArc;
    use std::sync::Mutex;
    use std::sync::atomic::AtomicUsize;
    use std::thread;
    use std::vec::Vec;

    struct Obj<'a> {
        common: ObjCommon,
        dropped: &'a AtomicUsize,
        /// Handles removed from a file whose release is not accounted yet:
        /// incremented with the file's lock held, decremented with the name
        /// table's.
        closing: AtomicU32,
        /// Bit `f` is set while file `f`'s driver hooks have the object
        /// open (what virtio-gpu's context attachment is).
        attached: AtomicU32,
    }
    impl Drop for Obj<'_> {
        fn drop(&mut self) {
            self.dropped.fetch_add(1, Ordering::SeqCst);
        }
    }
    impl ObjRef for KArc<Obj<'_>> {
        fn common(&self) -> &ObjCommon {
            &self.common
        }
        fn clone_ref(&self) -> Self {
            self.clone()
        }
        fn same(&self, other: &Self) -> bool {
            KArc::ptr_eq(self, other)
        }
    }

    type Table<'a> = IdMap<KArc<Obj<'a>>, FirstFit>;

    fn ctx() -> Sleepable {
        // SAFETY: host tests may block.
        unsafe { Sleepable::assume() }
    }
    fn table<'a>() -> Table<'a> {
        IdMap::new(FirstFit::new(1, i32::MAX as u32))
    }
    fn obj(dropped: &AtomicUsize) -> KArc<Obj<'_>> {
        KArc::new(
            Obj { common: ObjCommon::new(4096), dropped, closing: AtomicU32::new(0), attached: AtomicU32::new(0) },
            &ctx(),
        )
        .ok()
        .unwrap()
    }

    /// The invariants between objects, handle tables and the name table.
    /// A handle that was removed from its file but not yet accounted for by
    /// `handle_released` still counts, as in Linux, where the count drops
    /// after the idr removal; the test objects track such handles in
    /// `closing`.
    fn check(names: &Table<'_>, files: &[&Table<'_>]) {
        let mut seen: Vec<&KArc<Obj<'_>>> = Vec::new();
        for f in files {
            for (h, o) in f.iter() {
                assert_ne!(h, 0);
                if !seen.iter().any(|s| s.same(o)) {
                    seen.push(o);
                }
            }
        }
        for o in &seen {
            let handles = files.iter().flat_map(|f| f.iter()).filter(|(_, p)| p.same(o)).count();
            let closing = o.closing.load(Ordering::SeqCst) as usize;
            assert_eq!(o.common().handle_count() as usize, handles + closing, "handle count");
            let name = o.common().name();
            if name != 0 {
                assert!(names.get(name).is_some_and(|n| n.same(o)), "a name maps back to its object");
            }
        }
        for (name, o) in names.iter() {
            assert_eq!(o.common().name(), name);
            assert!(o.common().handle_count() > 0, "a named object has handles");
            let reachable = seen.iter().any(|s| s.same(o)) || o.closing.load(Ordering::SeqCst) > 0;
            assert!(reachable, "a named object has a handle in a file or one being closed");
        }
    }

    #[test]
    fn handle_name_life_cycle() {
        let ctx = ctx();
        let dropped = AtomicUsize::new(0);
        let (mut names, mut a, mut b) = (table(), table(), table());
        let h = handle_create(&mut names, &mut a, obj(&dropped), &ctx).ok().unwrap();
        assert_eq!(h, 1);
        let r = lookup(&a, h).unwrap();
        assert_eq!(lookup(&a, 0).err(), Some(Errno::ENOENT));
        assert_eq!(lookup(&a, 2).err(), Some(Errno::ENOENT));
        let name = flink(&mut names, &r, &ctx).unwrap();
        assert_eq!(flink(&mut names, &r, &ctx), Ok(name), "one name per object");
        drop(r);
        let o = open(&names, name).unwrap();
        let hb = handle_create(&mut names, &mut b, o, &ctx).ok().unwrap();
        assert_eq!(open(&names, name + 1).err(), Some(Errno::ENOENT));
        assert_eq!(open(&names, 0).err(), Some(Errno::ENOENT));
        check(&names, &[&a, &b]);
        assert_eq!(lookup(&b, hb).unwrap().common().handle_count(), 2);
        // Closing one handle keeps the name.
        let r = handle_delete(&mut a, h).unwrap();
        assert_eq!(handle_delete(&mut a, h).err(), Some(Errno::EINVAL));
        assert!(handle_released(&mut names, &r).is_none());
        drop(r);
        check(&names, &[&a, &b]);
        assert!(open(&names, name).is_ok());
        // Closing the last handle removes the name; the object dies with the
        // last reference.
        let r = handle_delete(&mut b, hb).unwrap();
        let n = handle_released(&mut names, &r).expect("name reference");
        assert_eq!(open(&names, name).err(), Some(Errno::ENOENT));
        assert_eq!(r.common().name(), 0);
        let stale = r.clone();
        drop((r, n));
        assert_eq!(dropped.load(Ordering::SeqCst), 0, "a mapping-like reference keeps it alive");
        assert_eq!(flink(&mut names, &stale, &ctx), Err(Errno::ENOENT), "no FLINK without handles");
        drop(stale);
        assert_eq!(dropped.load(Ordering::SeqCst), 1);
        check(&names, &[&a, &b]);
    }

    /// A small deterministic generator, so that failures reproduce.
    struct Lcg(u64);
    impl Lcg {
        fn next(&mut self, n: usize) -> usize {
            self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            ((self.0 >> 33) as usize) % n
        }
    }

    /// Eight threads create, close, flink and open handles in four files
    /// while a checker verifies the invariants with all locks held, in the
    /// lock order of the module documentation. At the end every file is
    /// closed and every object must have been destroyed exactly once.
    #[test]
    fn concurrent_handles_and_names() {
        const FILES: usize = 4;
        const THREADS: usize = 8;
        const STEPS: usize = 1500;
        let dropped = AtomicUsize::new(0);
        let created = AtomicUsize::new(0);
        let names: Mutex<Table<'_>> = Mutex::new(table());
        let files: Vec<Mutex<Table<'_>>> = (0..FILES).map(|_| Mutex::new(table())).collect();
        let published: Mutex<Vec<u32>> = Mutex::new(Vec::new());
        let stop = std::sync::atomic::AtomicBool::new(false);
        thread::scope(|s| {
            let checker = s.spawn(|| {
                while !stop.load(Ordering::SeqCst) {
                    let n = names.lock().unwrap();
                    let fs: Vec<_> = files.iter().map(|f| f.lock().unwrap()).collect();
                    let refs: Vec<&Table<'_>> = fs.iter().map(|g| &**g).collect();
                    check(&n, &refs);
                    drop(fs);
                    drop(n);
                    thread::sleep(std::time::Duration::from_micros(200));
                }
            });
            let workers: Vec<_> = (0..THREADS)
                .map(|t| {
                    let (names, files, published, dropped, created) = (&names, &files, &published, &dropped, &created);
                    s.spawn(move || {
                        let ctx = ctx();
                        let mut rng = Lcg(t as u64 + 1);
                        for _ in 0..STEPS {
                            let f = rng.next(FILES);
                            match rng.next(4) {
                                0 => {
                                    created.fetch_add(1, Ordering::SeqCst);
                                    let o = obj(dropped);
                                    let mut n = names.lock().unwrap();
                                    let mut file = files[f].lock().unwrap();
                                    handle_create(&mut n, &mut file, o, &ctx).ok().unwrap();
                                }
                                1 => {
                                    let r = {
                                        let mut file = files[f].lock().unwrap();
                                        let Some((h, _)) = file.iter().next() else { continue };
                                        let r = handle_delete(&mut file, h).unwrap();
                                        r.closing.fetch_add(1, Ordering::SeqCst);
                                        r
                                    };
                                    let name_ref = {
                                        let mut n = names.lock().unwrap();
                                        let name_ref = handle_released(&mut n, &r);
                                        r.closing.fetch_sub(1, Ordering::SeqCst);
                                        name_ref
                                    };
                                    drop((r, name_ref));
                                }
                                2 => {
                                    let r = {
                                        let file = files[f].lock().unwrap();
                                        let Some((h, _)) = file.iter().last() else { continue };
                                        lookup(&file, h).unwrap()
                                    };
                                    let got = flink(&mut names.lock().unwrap(), &r, &ctx);
                                    match got {
                                        Ok(name) => published.lock().unwrap().push(name),
                                        // The handle was closed by another thread in between.
                                        Err(e) => assert_eq!(e, Errno::ENOENT),
                                    }
                                    drop(r);
                                }
                                _ => {
                                    let name = {
                                        let p = published.lock().unwrap();
                                        if p.is_empty() {
                                            continue;
                                        }
                                        p[rng.next(p.len())]
                                    };
                                    let mut n = names.lock().unwrap();
                                    match open(&n, name) {
                                        Ok(o) => {
                                            let mut file = files[f].lock().unwrap();
                                            handle_create(&mut n, &mut file, o, &ctx).ok().unwrap();
                                        }
                                        Err(e) => assert_eq!(e, Errno::ENOENT),
                                    }
                                }
                            }
                        }
                    })
                })
                .collect();
            for w in workers {
                w.join().unwrap();
            }
            stop.store(true, Ordering::SeqCst);
            checker.join().unwrap();
        });
        // Close every file, as close(9E) does.
        for f in &files {
            loop {
                let Some((_, r)) = f.lock().unwrap().pop_first() else { break };
                let n = handle_released(&mut names.lock().unwrap(), &r);
                drop((r, n));
            }
        }
        assert!(names.lock().unwrap().is_empty(), "names go with the last handle");
        assert_eq!(dropped.load(Ordering::SeqCst), created.load(Ordering::SeqCst));
        assert!(created.load(Ordering::SeqCst) > 0);
    }

    /// A driver whose hooks attach an object to a file, as virtio-gpu
    /// attaches a resource to the file's context: opening an object the
    /// file has open already, or closing one it does not, is a test
    /// failure. `fail_open` makes the next `opened` fail.
    struct Driver {
        file: u32,
        opens: AtomicUsize,
        closes: AtomicUsize,
        fail_open: std::sync::atomic::AtomicBool,
        /// Yield inside the hooks, to widen the windows between threads.
        slow: bool,
    }
    impl Driver {
        fn new(file: u32) -> Driver {
            Driver {
                file,
                opens: AtomicUsize::new(0),
                closes: AtomicUsize::new(0),
                fail_open: std::sync::atomic::AtomicBool::new(false),
                slow: false,
            }
        }
        fn counts(&self) -> (usize, usize) {
            (self.opens.load(Ordering::SeqCst), self.closes.load(Ordering::SeqCst))
        }
    }
    impl<'a> Hooks<KArc<Obj<'a>>> for Driver {
        fn opened(&self, obj: &KArc<Obj<'a>>) -> Result<(), Errno> {
            if self.fail_open.swap(false, Ordering::SeqCst) {
                return Err(Errno::EIO);
            }
            if self.slow {
                thread::yield_now();
            }
            let bit = 1 << self.file;
            let was = obj.attached.fetch_or(bit, Ordering::SeqCst);
            assert_eq!(was & bit, 0, "opened for an object the file has open already");
            self.opens.fetch_add(1, Ordering::SeqCst);
            Ok(())
        }
        fn closed(&self, obj: &KArc<Obj<'a>>) {
            if self.slow {
                thread::yield_now();
            }
            let bit = 1 << self.file;
            let was = obj.attached.fetch_and(!bit, Ordering::SeqCst);
            assert_ne!(was & bit, 0, "closed for an object the file does not have open");
            self.closes.fetch_add(1, Ordering::SeqCst);
        }
    }

    type File<'a> = FileTable<KArc<Obj<'a>>, FirstFit>;

    fn file<'a>() -> File<'a> {
        FileTable::new(FirstFit::new(1, i32::MAX as u32))
    }

    /// `PRIME_FD_TO_HANDLE` as the kernel does it: the file's handle for
    /// the object if it has one, else a new one. The dma-buf's reference
    /// stays with the caller.
    fn import<'a>(
        f: &File<'a>,
        names: &KMutex<Table<'a>>,
        buf: &KArc<Obj<'a>>,
        d: &Driver,
        ctx: &Sleepable,
    ) -> Result<u32, Errno> {
        let l = f.lock();
        if let Some(h) = l.find(|r| r.same(buf)) {
            return Ok(h);
        }
        l.add(names, buf.clone(), d, ctx).map_err(|(_, e)| e)
    }

    fn attached(o: &KArc<Obj<'_>>, file: u32) -> bool {
        o.attached.load(Ordering::SeqCst) & (1 << file) != 0
    }

    /// Importing an object a file holds returns its handle and runs no
    /// hook (the G8 bug: the old code opened the object again and then
    /// closed it, which detached the virtio-gpu resource the existing
    /// handle used). A second handle by `GEM_OPEN` runs no hook either, and
    /// only the last handle's removal closes.
    #[test]
    fn reimport_and_second_handles_keep_the_attachment() {
        let ctx = ctx();
        let dropped = AtomicUsize::new(0);
        let names = KMutex::new(table());
        let (a, b) = (file(), file());
        let (da, db) = (Driver::new(0), Driver::new(1));
        let o = obj(&dropped);
        let h = a.lock().add(&names, o.clone(), &da, &ctx).ok().unwrap();
        assert_eq!(da.counts(), (1, 0));
        assert!(attached(&o, 0));
        // The same file imports its own buffer, twice.
        assert_eq!(import(&a, &names, &o, &da, &ctx), Ok(h));
        assert_eq!(import(&a, &names, &o, &da, &ctx), Ok(h));
        assert_eq!(da.counts(), (1, 0), "no hook for a re-import");
        assert!(attached(&o, 0));
        assert_eq!(o.common().handle_count(), 1, "no second handle");
        // Another file imports it twice: one handle, one open.
        let hb = import(&b, &names, &o, &db, &ctx).unwrap();
        assert_eq!(import(&b, &names, &o, &db, &ctx), Ok(hb));
        assert_eq!(db.counts(), (1, 0));
        assert!(attached(&o, 1));
        // GEM_OPEN in the creating file: a second handle, no hook.
        let name = flink(&mut names.lock(), &o, &ctx).unwrap();
        let n = open(&names.lock(), name).unwrap();
        let h2 = a.lock().add(&names, n, &da, &ctx).ok().unwrap();
        assert_ne!(h2, h);
        assert_eq!(da.counts(), (1, 0));
        // An import with two handles finds one of them.
        let found = import(&a, &names, &o, &da, &ctx).unwrap();
        assert!(found == h || found == h2);
        // Closing the first handle keeps the attachment for the second.
        let (r, nr) = a.lock().remove(&names, h, &da).unwrap();
        assert!(nr.is_none());
        drop(r);
        assert_eq!(da.counts(), (1, 0), "the file still holds the object");
        assert!(attached(&o, 0));
        assert_eq!(a.lock().remove(&names, h, &da).err(), Some(Errno::EINVAL));
        // The last handle of the file closes.
        let (r, _) = a.lock().remove(&names, h2, &da).unwrap();
        drop(r);
        assert_eq!(da.counts(), (1, 1));
        assert!(!attached(&o, 0));
        assert!(attached(&o, 1), "the other file is not affected");
        // close(9E) of the other file.
        while let Some((r, nr)) = b.lock().pop(&names, &db) {
            drop((r, nr));
        }
        assert_eq!(db.counts(), (1, 1));
        assert_eq!(o.attached.load(Ordering::SeqCst), 0);
        drop(o);
        assert_eq!(dropped.load(Ordering::SeqCst), 1);
    }

    /// An allocator that hands out `n` numbers and then fails.
    struct Few(FirstFit, u32);
    impl IdAlloc for Few {
        fn alloc(&mut self) -> Option<u32> {
            if self.1 == 0 {
                return None;
            }
            self.1 -= 1;
            self.0.alloc()
        }
        fn free(&mut self, id: u32) {
            self.0.free(id)
        }
    }

    /// A failed handle creation undoes only what it did itself: the open
    /// hook it ran, never the attachment of a handle the file had before.
    #[test]
    fn failed_creation_undoes_only_its_own_open() {
        let ctx = ctx();
        let dropped = AtomicUsize::new(0);
        let names = KMutex::new(table());
        let f: FileTable<KArc<Obj<'_>>, Few> = FileTable::new(Few(FirstFit::new(1, i32::MAX as u32), 1));
        let d = Driver::new(0);
        let (o, p) = (obj(&dropped), obj(&dropped));
        // The open hook refuses: no handle, nothing to undo.
        d.fail_open.store(true, Ordering::SeqCst);
        let (back, e) = f.lock().add(&names, o.clone(), &d, &ctx).err().unwrap();
        assert_eq!(e, Errno::EIO);
        drop(back);
        assert_eq!(d.counts(), (0, 0));
        assert_eq!(o.common().handle_count(), 0);
        // The one number goes to o.
        let h = f.lock().add(&names, o.clone(), &d, &ctx).ok().unwrap();
        assert_eq!(d.counts(), (1, 0));
        // A second handle to o fails for want of numbers: o was open
        // before, so nothing is closed.
        let (back, e) = f.lock().add(&names, o.clone(), &d, &ctx).err().unwrap();
        assert_eq!(e, Errno::ENOMEM);
        drop(back);
        assert_eq!(d.counts(), (1, 0));
        assert!(attached(&o, 0));
        assert_eq!(o.common().handle_count(), 1, "the count is restored");
        // A first handle to p fails the same way: its own open is undone.
        let (back, _) = f.lock().add(&names, p.clone(), &d, &ctx).err().unwrap();
        drop(back);
        assert_eq!(d.counts(), (2, 1));
        assert!(!attached(&p, 0));
        assert!(attached(&o, 0));
        let (r, _) = f.lock().remove(&names, h, &d).unwrap();
        drop(r);
        assert_eq!(d.counts(), (2, 2));
        drop((o, p));
        assert_eq!(dropped.load(Ordering::SeqCst), 2);
    }

    /// Eight threads create objects, import shared buffers (some twice in
    /// a row, as DRI3 does), open them by name and close handles in four
    /// files, with hooks that yield, while a checker verifies with every
    /// hook lock held that a file has an object open exactly when it holds
    /// a handle to it. The driver's assertions fail on any open of an open
    /// object or close of a closed one. At the end every file is closed:
    /// every open has its close and every object is destroyed once.
    #[test]
    fn concurrent_hooks_follow_the_handles() {
        const FILES: usize = 4;
        const THREADS: usize = 8;
        const STEPS: usize = 1500;
        let dropped = AtomicUsize::new(0);
        let created = AtomicUsize::new(0);
        let names: KMutex<Table<'_>> = KMutex::new(table());
        let files: Vec<File<'_>> = (0..FILES).map(|_| file()).collect();
        let drivers: Vec<Driver> = (0..FILES as u32).map(|f| Driver { slow: true, ..Driver::new(f) }).collect();
        // The shared buffers (dma-bufs), each with its own reference.
        let bufs: Vec<KArc<Obj<'_>>> = (0..6).map(|_| obj(&dropped)).collect();
        created.fetch_add(bufs.len(), Ordering::SeqCst);
        let published: Mutex<Vec<u32>> = Mutex::new(Vec::new());
        let stop = std::sync::atomic::AtomicBool::new(false);
        let check_all = || {
            let locks: Vec<_> = files.iter().map(|f| f.lock()).collect();
            let all: Vec<KArc<Obj<'_>>> = files
                .iter()
                .flat_map(|f| f.handles().iter().map(|(_, r)| r.clone()).collect::<Vec<_>>())
                .chain(bufs.iter().cloned())
                .collect();
            for o in &all {
                for (i, l) in locks.iter().enumerate() {
                    assert_eq!(attached(o, i as u32), l.holds(o), "file {i}: open exactly while it holds a handle");
                }
            }
            drop(locks);
            drop(all);
        };
        thread::scope(|s| {
            let checker = s.spawn(|| {
                while !stop.load(Ordering::SeqCst) {
                    check_all();
                    thread::sleep(std::time::Duration::from_micros(200));
                }
            });
            let workers: Vec<_> = (0..THREADS)
                .map(|t| {
                    let (names, files, drivers, bufs, published, dropped, created) =
                        (&names, &files, &drivers, &bufs, &published, &dropped, &created);
                    s.spawn(move || {
                        let ctx = ctx();
                        let mut rng = Lcg(t as u64 + 101);
                        for _ in 0..STEPS {
                            let f = rng.next(FILES);
                            let (file, d) = (&files[f], &drivers[f]);
                            match rng.next(5) {
                                0 => {
                                    created.fetch_add(1, Ordering::SeqCst);
                                    let o = obj(dropped);
                                    file.lock().add(names, o, d, &ctx).ok().unwrap();
                                }
                                1 => {
                                    let b = &bufs[rng.next(bufs.len())];
                                    let h = import(file, names, b, d, &ctx).unwrap();
                                    if rng.next(2) == 0 {
                                        // DRI3 imports what it holds again.
                                        let again = import(file, names, b, d, &ctx).unwrap();
                                        // Another thread may have closed h in between.
                                        let _ = again;
                                    }
                                    if let Ok(name) = flink(&mut names.lock(), b, &ctx) {
                                        published.lock().unwrap().push(name);
                                    }
                                    let _ = h;
                                }
                                2 | 3 => {
                                    let Some((h, _)) = file.handles().iter().next().map(|(h, r)| (h, r.clone())) else {
                                        continue;
                                    };
                                    match file.lock().remove(names, h, d) {
                                        Ok((r, nr)) => drop((r, nr)),
                                        // Removed by another thread meanwhile.
                                        Err(e) => assert_eq!(e, Errno::EINVAL),
                                    }
                                }
                                _ => {
                                    let name = {
                                        let p = published.lock().unwrap();
                                        if p.is_empty() {
                                            continue;
                                        }
                                        p[rng.next(p.len())]
                                    };
                                    let o = open(&names.lock(), name);
                                    match o {
                                        Ok(o) => {
                                            file.lock().add(names, o, d, &ctx).ok().unwrap();
                                        }
                                        Err(e) => assert_eq!(e, Errno::ENOENT),
                                    }
                                }
                            }
                        }
                    })
                })
                .collect();
            // Stop the checker before reporting a worker's failure, so that a
            // failure ends the test instead of hanging it.
            let results: Vec<_> = workers.into_iter().map(|w| w.join()).collect();
            stop.store(true, Ordering::SeqCst);
            let checked = checker.join();
            for r in results {
                r.unwrap();
            }
            checked.unwrap();
        });
        check_all();
        for (f, d) in files.iter().zip(&drivers) {
            while let Some((r, nr)) = f.lock().pop(&names, d) {
                drop((r, nr));
            }
            let (o, c) = d.counts();
            assert_eq!(o, c, "every open has its close");
            assert!(o > 0);
        }
        assert!(bufs.iter().all(|b| b.attached.load(Ordering::SeqCst) == 0));
        drop(bufs);
        assert!(names.lock().is_empty());
        assert_eq!(dropped.load(Ordering::SeqCst), created.load(Ordering::SeqCst));
    }
}
