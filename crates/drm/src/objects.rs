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
//! # Lock order
//!
//! The name table lock is taken before a file's handle table lock, never
//! after: [`handle_create`] runs with both held (name table first), and
//! closing a handle removes it with only the file's lock held
//! ([`handle_delete`]) and then accounts for it with only the name table
//! lock held ([`handle_released`]). References that come out of the tables
//! are dropped by the caller after both locks are released, because
//! destroying an object takes other locks. The functions here never drop a
//! reference themselves.
//!
//! The tests drive these rules from eight threads with the same locking
//! and check the invariants after every step.

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use ddi::{Errno, Sleepable};

use crate::defer::DeferLink;
use crate::idmap::{IdAlloc, IdMap};

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
        KArc::new(Obj { common: ObjCommon::new(4096), dropped, closing: AtomicU32::new(0) }, &ctx()).ok().unwrap()
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
}
