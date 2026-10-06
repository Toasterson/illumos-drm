// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The mutable state of one DRM device: open files, masters and the
//! authentication magic table.
//!
//! Everything here is plain data with no kernel calls, so that the rules
//! can be unit-tested on the host. [`crate::Device`] keeps one
//! `DeviceState` behind one `ddi::Mutex` and performs every state
//! transition (open, close, and the permission check plus action of every
//! ioctl) with that mutex held; see the concurrency notes there.
//!
//! The model follows Linux's `drm_auth.c` and `drm_file.c`:
//!
//! * Each file on a primary node is bound to a master object. The first
//!   file opened while the device has no current master creates a new
//!   master object, becomes the current master and is authenticated; later
//!   files are bound to the current master's object.
//! * Magic numbers from `GET_MAGIC` live in the namespace of the master
//!   object the file is bound to. `AUTH_MAGIC` by the current master
//!   authenticates a file bound to the same object and consumes the entry.
//! * `SET_MASTER` needs `PRIV_SYS_DEVICES`, or a file that was master
//!   before and is used by the process that opened it. A file that never
//!   owned a master object gets a new one.
//! * The bus ID (`GET_UNIQUE`) is a property of the master object: empty
//!   until the master sets interface version 1.1 or later, or calls
//!   `SET_UNIQUE`. That is what libdrm's `drmOpenByName` and
//!   `drmOpenByBusid` expect.
//! * Render-node files have no master and are never authenticated; the
//!   ioctl permission table keeps them away from everything that would
//!   need either.

use ddi::{Errno, Sleepable};

use crate::caps::ClientCaps;
use crate::kvec::KVec;
use crate::minor::{CloneId, MAX_FILES, NodeKind};

/// The DRM interface version the core implements (`DRM_IF_MAJOR`,
/// `DRM_IF_MINOR`).
pub const IF_MAJOR: i32 = 1;
/// See [`IF_MAJOR`].
pub const IF_MINOR: i32 = 4;

/// The identity of a master object.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MasterId(u64);

/// The per-file state of one open of a card or render node.
#[derive(Debug)]
pub struct DrmFile {
    /// The node the file was opened on.
    pub kind: NodeKind,
    /// Whether the file may use `DRM_AUTH` ioctls. Files of privileged
    /// openers and masters are authenticated from the start; others become
    /// authenticated through `AUTH_MAGIC`.
    pub authenticated: bool,
    /// The client capabilities set with `SET_CLIENT_CAP`.
    pub caps: ClientCaps,
    /// The process that opened the file.
    pub pid: i32,
    master: Option<MasterId>,
    owns_master: bool,
    was_master: bool,
    magic: u32,
}

impl DrmFile {
    /// Returns the file's magic number, 0 if it never asked for one.
    pub fn magic(&self) -> u32 {
        self.magic
    }
}

/// What the facade's `struct drm_file` of an open file mirrors of its
/// state (facade 2.0): the client capabilities and whether the file is,
/// or ever was, its device's master. The core writes it into the file's
/// structure after every change ([`FileFlags::store`]), for C code that
/// reads Linux's fields; [`DeviceState`] stays the authority.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FileFlags {
    /// The client capabilities.
    pub caps: ClientCaps,
    /// The file is the current master (Linux's `drm_file.is_master`).
    pub is_master: bool,
    /// The file was master at some time (Linux's `drm_file.was_master`).
    pub was_master: bool,
}

impl FileFlags {
    /// The booleans at the start of `struct drm_file`, in Linux's order:
    /// `stereo_allowed`, `universal_planes`, `atomic`,
    /// `aspect_ratio_allowed`, `writeback_connectors`,
    /// `plane_color_pipeline`, `was_master`, `is_master`,
    /// `supports_virtualized_cursor_plane`. The core refuses the client
    /// capabilities of the sixth and the last, which stay false.
    pub fn linux(&self) -> [bool; 9] {
        let c = &self.caps;
        [
            c.stereo_3d,
            c.universal_planes,
            c.atomic,
            c.aspect_ratio,
            c.writeback_connectors,
            false,
            self.was_master,
            self.is_master,
            false,
        ]
    }

    /// Writes the flags into the facade structure `fp`, one atomic byte
    /// store per field, so that C code reading them concurrently sees each
    /// field old or new (a snapshot, as `drm_file.h` documents).
    ///
    /// # Safety
    /// `fp` points to a live `struct drm_file`.
    pub unsafe fn store(&self, fp: *mut drm_facade_sys::drm_file) {
        use core::sync::atomic::{AtomicBool, Ordering};
        let base = fp as *mut u8;
        for (i, v) in self.linux().into_iter().enumerate() {
            // SAFETY: the nine booleans are the structure's first nine
            // bytes (asserted below); the caller's pointer is live, and
            // AtomicBool has bool's layout.
            unsafe { AtomicBool::from_ptr(base.add(i).cast::<bool>()).store(v, Ordering::Relaxed) };
        }
    }
}

const _: () = {
    use core::mem::offset_of;
    use drm_facade_sys::drm_file as F;
    assert!(offset_of!(F, stereo_allowed) == 0);
    assert!(offset_of!(F, universal_planes) == 1);
    assert!(offset_of!(F, atomic) == 2);
    assert!(offset_of!(F, aspect_ratio_allowed) == 3);
    assert!(offset_of!(F, writeback_connectors) == 4);
    assert!(offset_of!(F, plane_color_pipeline) == 5);
    assert!(offset_of!(F, was_master) == 6);
    assert!(offset_of!(F, is_master) == 7);
    assert!(offset_of!(F, supports_virtualized_cursor_plane) == 8);
};

/// Who opens a file, as far as the state cares.
#[derive(Clone, Copy, Debug)]
pub struct Opener {
    /// The node being opened.
    pub kind: NodeKind,
    /// Whether the opener holds `PRIV_SYS_DEVICES` (the illumos
    /// counterpart of Linux's `CAP_SYS_ADMIN` check).
    pub privileged: bool,
    /// The opener's process ID.
    pub pid: i32,
}

/// The permission flags of an ioctl, as in Linux's `DRM_IOCTL_DEF`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Perm {
    /// `DRM_AUTH`: primary-node files must be authenticated.
    pub auth: bool,
    /// `DRM_MASTER`: the file must be the current master.
    pub master: bool,
    /// `DRM_ROOT_ONLY`: the caller must hold `PRIV_SYS_DEVICES`.
    pub root_only: bool,
    /// `DRM_RENDER_ALLOW`: render-node files may use the ioctl.
    pub render_allow: bool,
}

/// The arguments of `SET_VERSION`; -1 means "do not care".
#[derive(Clone, Copy, Debug)]
pub struct SetVersion {
    /// Requested interface major version.
    pub di_major: i32,
    /// Requested interface minor version.
    pub di_minor: i32,
    /// Requested driver major version.
    pub dd_major: i32,
    /// Requested driver minor version.
    pub dd_minor: i32,
}

#[derive(Debug)]
struct MasterObj {
    id: MasterId,
    refs: u32,
    unique: bool,
}

#[derive(Debug)]
struct MagicEntry {
    magic: u32,
    file: CloneId,
    master: MasterId,
}

/// The state of one device. See the module documentation.
pub struct DeviceState {
    /// Indexed by clone id; slot 0 is never used.
    files: KVec<Option<DrmFile>>,
    open: u32,
    masters: KVec<MasterObj>,
    next_master: u64,
    current: Option<CloneId>,
    magics: KVec<MagicEntry>,
    next_magic: u32,
    if_version: i32,
}

impl Default for DeviceState {
    fn default() -> Self {
        Self::new()
    }
}

impl DeviceState {
    /// Creates the state of a device with no open files.
    pub const fn new() -> DeviceState {
        DeviceState {
            files: KVec::new(),
            open: 0,
            masters: KVec::new(),
            next_master: 1,
            current: None,
            magics: KVec::new(),
            next_magic: 1,
            if_version: 0,
        }
    }

    /// Returns the number of open files.
    pub fn open_files(&self) -> u32 {
        self.open
    }

    /// Returns the highest interface version set with `SET_VERSION`, as
    /// `major << 16 | minor`, 0 if none was.
    pub fn if_version(&self) -> i32 {
        self.if_version
    }

    /// Returns an open file.
    pub fn file(&self, c: CloneId) -> Option<&DrmFile> {
        self.files.get(c.index()).and_then(Option::as_ref)
    }

    /// Returns an open file for modification.
    pub fn file_mut(&mut self, c: CloneId) -> Option<&mut DrmFile> {
        self.files.get_mut(c.index()).and_then(Option::as_mut)
    }

    /// Reports whether `c` is the current master.
    pub fn is_current_master(&self, c: CloneId) -> bool {
        self.current == Some(c)
    }

    /// What the facade's `drm_file` of open file `c` mirrors
    /// ([`FileFlags`]).
    pub fn file_flags(&self, c: CloneId) -> Option<FileFlags> {
        let f = self.file(c)?;
        Some(FileFlags { caps: f.caps, is_master: self.is_current_master(c), was_master: f.was_master })
    }

    /// Registers a new open file and returns its clone id. Fails with
    /// `EMFILE` when all [`MAX_FILES`] clone ids are in use.
    pub fn open(&mut self, who: Opener, ctx: &Sleepable) -> Result<CloneId, Errno> {
        // A failed open leaves no file, master object or magic behind; at most
        // the table keeps an extra empty slot.
        let slot = match self.files.iter().skip(1).position(Option::is_none) {
            Some(i) => i + 1,
            None => {
                if self.files.len() > MAX_FILES as usize {
                    return Err(Errno::EMFILE);
                }
                if self.files.is_empty() {
                    self.files.push(None, ctx)?;
                }
                self.files.push(None, ctx)?;
                self.files.len() - 1
            }
        };
        let clone = CloneId::new(slot as u32).ok_or(Errno::EMFILE)?;
        if !matches!(self.files.get(slot), Some(None)) {
            // Unreachable: the slot was found empty or just pushed.
            return Err(Errno::EMFILE);
        }
        let mut file = DrmFile {
            kind: who.kind,
            authenticated: who.privileged,
            caps: ClientCaps::default(),
            pid: who.pid,
            master: None,
            owns_master: false,
            was_master: false,
            magic: 0,
        };
        if who.kind == NodeKind::Primary {
            match self.current.and_then(|m| self.file(m)).and_then(|f| f.master) {
                Some(id) => {
                    self.master_obj_mut(id).ok_or(Errno::EINVAL)?.refs += 1;
                    file.master = Some(id);
                }
                None => {
                    file.master = Some(self.new_master(ctx)?);
                    file.owns_master = true;
                    file.was_master = true;
                    file.authenticated = true;
                    self.current = Some(clone);
                }
            }
        }
        let master = file.master;
        match self.files.get_mut(slot) {
            Some(entry) => *entry = Some(file),
            None => {
                // Unreachable: the slot was checked above and nothing since
                // shrinks the table. Undo the master binding and fail.
                if self.current == Some(clone) {
                    self.current = None;
                }
                if let Some(id) = master {
                    self.unref_master(id);
                }
                return Err(Errno::EMFILE);
            }
        }
        self.open += 1;
        Ok(clone)
    }

    /// Forgets a closed file: drops its magic, its master status and its
    /// reference to the master object. Returns `false` if there was no such
    /// file.
    pub fn close(&mut self, c: CloneId) -> bool {
        let Some(file) = self.files.get_mut(c.index()).and_then(Option::take) else { return false };
        self.open = self.open.saturating_sub(1);
        self.remove_magic_of(c);
        if self.current == Some(c) {
            self.current = None;
        }
        if let Some(id) = file.master {
            self.unref_master(id);
        }
        true
    }

    /// Checks the permission flags of an ioctl for file `c`, in the order
    /// Linux's `drm_ioctl_permit` uses. Fails with `EACCES`, or `ENXIO` if
    /// the file does not exist. `privileged` is asked only for `ROOT_ONLY`
    /// ioctls, as Linux calls `capable()` only then, so that ordinary
    /// ioctls never trigger a privilege check (and its audit record).
    pub fn permit(&self, c: CloneId, perm: Perm, privileged: impl FnOnce() -> bool) -> Result<(), Errno> {
        let f = self.file(c).ok_or(Errno::ENXIO)?;
        if perm.root_only && !privileged() {
            return Err(Errno::EACCES);
        }
        if perm.auth && f.kind != NodeKind::Render && !f.authenticated {
            return Err(Errno::EACCES);
        }
        if perm.master && !self.is_current_master(c) {
            return Err(Errno::EACCES);
        }
        if !perm.render_allow && f.kind == NodeKind::Render {
            return Err(Errno::EACCES);
        }
        Ok(())
    }

    /// `GET_MAGIC`: returns the file's magic number, allocating one in its
    /// master's namespace on first use. As on Linux, the number stays with
    /// the file after `AUTH_MAGIC` has consumed it.
    pub fn get_magic(&mut self, c: CloneId, ctx: &Sleepable) -> Result<u32, Errno> {
        let f = self.file(c).ok_or(Errno::ENXIO)?;
        if f.magic != 0 {
            return Ok(f.magic);
        }
        let master = f.master.ok_or(Errno::EINVAL)?;
        let magic = self.unused_magic();
        self.magics.push(MagicEntry { magic, file: c, master }, ctx)?;
        self.next_magic = magic.wrapping_add(1);
        if let Some(f) = self.file_mut(c) {
            f.magic = magic;
        }
        Ok(magic)
    }

    /// `AUTH_MAGIC` by the master `c`: authenticates the file that holds
    /// `magic` in the same master namespace and consumes the entry. Fails
    /// with `EINVAL` for an unknown or consumed magic. The caller has
    /// checked that `c` is the current master.
    pub fn auth_magic(&mut self, c: CloneId, magic: u32) -> Result<(), Errno> {
        let master = self.file(c).and_then(|f| f.master).ok_or(Errno::EINVAL)?;
        let i = self.magics.iter().position(|e| e.magic == magic && e.master == master).ok_or(Errno::EINVAL)?;
        let entry = self.magics.swap_remove(i).ok_or(Errno::EINVAL)?;
        let target = self.file_mut(entry.file).ok_or(Errno::EINVAL)?;
        target.authenticated = true;
        Ok(())
    }

    /// `SET_MASTER` by file `c` of process `pid`.
    /// `privileged` is asked only when the file is not a former master used
    /// by its opening process, as in Linux's `drm_master_check_perm`.
    pub fn set_master(
        &mut self,
        c: CloneId,
        privileged: impl FnOnce() -> bool,
        pid: i32,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let f = self.file(c).ok_or(Errno::ENXIO)?;
        if !(f.was_master && f.pid == pid) && !privileged() {
            return Err(Errno::EACCES);
        }
        if self.current == Some(c) {
            return Ok(());
        }
        if self.current.is_some() {
            return Err(Errno::EBUSY);
        }
        let old = f.master.ok_or(Errno::EINVAL)?;
        if !f.owns_master {
            // A new master object, as Linux's drm_new_set_master. The
            // file's magic belonged to the old namespace and is dropped.
            let id = self.new_master(ctx)?;
            self.remove_magic_of(c);
            self.unref_master(old);
            if let Some(f) = self.file_mut(c) {
                f.master = Some(id);
                f.owns_master = true;
                f.authenticated = true;
                f.magic = 0;
            }
        }
        if let Some(f) = self.file_mut(c) {
            f.was_master = true;
        }
        self.current = Some(c);
        Ok(())
    }

    /// `DROP_MASTER` by file `c` of process `pid`.
    /// `privileged` is asked only when needed, as for [`Self::set_master`].
    pub fn drop_master(&mut self, c: CloneId, privileged: impl FnOnce() -> bool, pid: i32) -> Result<(), Errno> {
        let f = self.file(c).ok_or(Errno::ENXIO)?;
        if !(f.was_master && f.pid == pid) && !privileged() {
            return Err(Errno::EACCES);
        }
        if self.current != Some(c) {
            return Err(Errno::EINVAL);
        }
        self.current = None;
        Ok(())
    }

    /// Reports whether the bus ID is set in the namespace of file `c`'s
    /// master, which decides whether `GET_UNIQUE` returns it or an empty
    /// string.
    pub fn unique_set(&self, c: CloneId) -> bool {
        self.file(c).and_then(|f| f.master).and_then(|id| self.master_obj(id)).is_some_and(|m| m.unique)
    }

    /// `SET_UNIQUE` by the master `c`, after the caller has checked that
    /// the string is the device's bus ID. Fails with `EBUSY` if it is set
    /// already, as Linux did while the ioctl existed.
    pub fn set_unique(&mut self, c: CloneId) -> Result<(), Errno> {
        let id = self.file(c).and_then(|f| f.master).ok_or(Errno::EINVAL)?;
        let m = self.master_obj_mut(id).ok_or(Errno::EINVAL)?;
        if m.unique {
            return Err(Errno::EBUSY);
        }
        m.unique = true;
        Ok(())
    }

    /// `SET_VERSION` by the master `c`, with the driver's own version.
    /// Records the interface version and, for 1.1 and later, sets the bus
    /// ID in the master's namespace. Invalid versions fail with `EINVAL`;
    /// the caller reports the current versions back in every case.
    pub fn set_version(&mut self, c: CloneId, sv: SetVersion, drv_major: i32, drv_minor: i32) -> Result<(), Errno> {
        if sv.di_major != -1 {
            if sv.di_major != IF_MAJOR || sv.di_minor < 0 || sv.di_minor > IF_MINOR {
                return Err(Errno::EINVAL);
            }
            self.if_version = self.if_version.max((sv.di_major << 16) | sv.di_minor);
            if sv.di_minor >= 1 {
                let id = self.file(c).and_then(|f| f.master).ok_or(Errno::EINVAL)?;
                self.master_obj_mut(id).ok_or(Errno::EINVAL)?.unique = true;
            }
        }
        if sv.dd_major != -1 && (sv.dd_major != drv_major || sv.dd_minor < 0 || sv.dd_minor > drv_minor) {
            return Err(Errno::EINVAL);
        }
        Ok(())
    }

    fn new_master(&mut self, ctx: &Sleepable) -> Result<MasterId, Errno> {
        let id = MasterId(self.next_master);
        self.masters.push(MasterObj { id, refs: 1, unique: false }, ctx)?;
        self.next_master += 1;
        Ok(id)
    }

    fn master_obj(&self, id: MasterId) -> Option<&MasterObj> {
        self.masters.iter().find(|m| m.id == id)
    }

    fn master_obj_mut(&mut self, id: MasterId) -> Option<&mut MasterObj> {
        self.masters.iter_mut().find(|m| m.id == id)
    }

    fn unref_master(&mut self, id: MasterId) {
        let Some(i) = self.masters.iter().position(|m| m.id == id) else { return };
        let gone = match self.masters.get_mut(i) {
            Some(m) => {
                m.refs = m.refs.saturating_sub(1);
                m.refs == 0
            }
            None => false,
        };
        if gone {
            self.masters.swap_remove(i);
        }
    }

    fn remove_magic_of(&mut self, c: CloneId) {
        if let Some(i) = self.magics.iter().position(|e| e.file == c) {
            self.magics.swap_remove(i);
        }
    }

    /// The next non-zero magic number that no open file holds, whether
    /// still waiting in the table or already consumed by `AUTH_MAGIC` (the
    /// file keeps reporting it), so that numbers stay unique even after the
    /// counter wraps. At most [`MAX_FILES`] files exist, so at most that
    /// many candidates are skipped.
    fn unused_magic(&self) -> u32 {
        let mut m = self.next_magic;
        loop {
            if m != 0 && !self.files.iter().flatten().any(|f| f.magic == m) {
                return m;
            }
            m = m.wrapping_add(1);
        }
    }

    /// Checks the internal invariants; used by the tests after every step.
    #[cfg(test)]
    pub(crate) fn check(&self) {
        extern crate std;
        let files: std::vec::Vec<(CloneId, &DrmFile)> =
            self.files.iter().enumerate().filter_map(|(i, f)| Some((CloneId::new(i as u32)?, f.as_ref()?))).collect();
        assert!(self.files.first().is_none_or(Option::is_none), "slot 0 is never used");
        assert_eq!(files.len() as u32, self.open);
        for m in self.masters.iter() {
            let bound = files.iter().filter(|(_, f)| f.master == Some(m.id)).count() as u32;
            assert_eq!(bound, m.refs, "master refcount");
            assert!(m.refs > 0);
        }
        for (c, f) in &files {
            match f.kind {
                NodeKind::Render => assert!(f.master.is_none() && !f.owns_master && f.magic == 0),
                NodeKind::Primary => assert!(f.master.is_some_and(|id| self.master_obj(id).is_some())),
            }
            if f.owns_master {
                let owners = files.iter().filter(|(_, g)| g.owns_master && g.master == f.master).count();
                assert_eq!(owners, 1, "one owner per master object, file {c:?}");
            }
        }
        if let Some(c) = self.current {
            let f = self.file(c).expect("current master is open");
            assert!(f.owns_master && f.was_master && f.authenticated);
        }
        for (i, (_, f)) in files.iter().enumerate() {
            if f.magic != 0 {
                assert!(files[i + 1..].iter().all(|(_, g)| g.magic != f.magic), "magic numbers are unique");
            }
        }
        for (i, e) in self.magics.iter().enumerate() {
            assert_ne!(e.magic, 0);
            let f = self.file(e.file).expect("magic of an open file");
            assert_eq!(f.magic, e.magic);
            assert_eq!(f.master, Some(e.master));
            assert!(self.magics.iter().skip(i + 1).all(|o| o.magic != e.magic && o.file != e.file));
        }
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::sync::{Arc, Mutex};
    use std::vec::Vec;

    fn ctx() -> Sleepable {
        // SAFETY: host tests run in ordinary threads, which may block.
        unsafe { Sleepable::assume() }
    }

    fn user(kind: NodeKind, pid: i32) -> Opener {
        Opener { kind, privileged: false, pid }
    }
    fn root(kind: NodeKind, pid: i32) -> Opener {
        Opener { kind, privileged: true, pid }
    }

    const R0: Perm = Perm { auth: false, master: false, root_only: false, render_allow: true };
    const PRIMARY_ONLY: Perm = Perm { auth: false, master: false, root_only: false, render_allow: false };
    const AUTH_MASTER: Perm = Perm { auth: true, master: true, root_only: false, render_allow: false };
    const ROOT_AUTH_MASTER: Perm = Perm { auth: true, master: true, root_only: true, render_allow: false };

    #[test]
    fn first_primary_open_is_master() {
        let ctx = ctx();
        let mut s = DeviceState::new();
        let a = s.open(user(NodeKind::Primary, 10), &ctx).unwrap();
        let b = s.open(user(NodeKind::Primary, 11), &ctx).unwrap();
        let r = s.open(user(NodeKind::Render, 12), &ctx).unwrap();
        s.check();
        assert_eq!((a.get(), b.get(), r.get()), (1, 2, 3));
        assert!(s.is_current_master(a) && !s.is_current_master(b));
        assert!(s.file(a).unwrap().authenticated);
        assert!(!s.file(b).unwrap().authenticated);
        assert!(!s.file(r).unwrap().authenticated);
        assert_eq!(s.open_files(), 3);
        // Closing the master leaves the device without one; the next open
        // becomes master of a new object.
        assert!(s.close(a));
        assert!(!s.close(a));
        s.check();
        let c = s.open(user(NodeKind::Primary, 13), &ctx).unwrap();
        assert_eq!(c, a, "clone ids are reused");
        assert!(s.is_current_master(c));
        s.check();
    }

    #[test]
    fn magic_auth_round_trip() {
        let ctx = ctx();
        let mut s = DeviceState::new();
        let m = s.open(user(NodeKind::Primary, 1), &ctx).unwrap();
        let c = s.open(user(NodeKind::Primary, 2), &ctx).unwrap();
        let magic = s.get_magic(c, &ctx).unwrap();
        assert_ne!(magic, 0);
        assert_eq!(s.get_magic(c, &ctx), Ok(magic), "stable per file");
        s.check();
        assert_eq!(s.permit(c, AUTH_MASTER, || false), Err(Errno::EACCES), "a client is not master");
        assert_eq!(s.permit(m, AUTH_MASTER, || false), Ok(()));
        assert_eq!(s.permit(m, ROOT_AUTH_MASTER, || false), Err(Errno::EACCES));
        assert_eq!(s.auth_magic(m, magic ^ 0x5555), Err(Errno::EINVAL));
        assert_eq!(s.auth_magic(m, 0), Err(Errno::EINVAL));
        assert_eq!(s.auth_magic(m, magic), Ok(()));
        assert!(s.file(c).unwrap().authenticated);
        assert_eq!(s.auth_magic(m, magic), Err(Errno::EINVAL), "consumed");
        assert_eq!(s.get_magic(c, &ctx), Ok(magic));
        s.check();
        // Magic numbers are unique among waiting files.
        let d = s.open(user(NodeKind::Primary, 3), &ctx).unwrap();
        let e = s.open(user(NodeKind::Primary, 4), &ctx).unwrap();
        let md = s.get_magic(d, &ctx).unwrap();
        let me = s.get_magic(e, &ctx).unwrap();
        assert_ne!(md, me);
        assert!(s.close(d));
        s.check();
        assert_eq!(s.auth_magic(m, md), Err(Errno::EINVAL), "closed files lose their magic");
    }

    #[test]
    fn consumed_magic_is_not_reissued_after_wrap() {
        let ctx = ctx();
        let mut s = DeviceState::new();
        let m = s.open(user(NodeKind::Primary, 1), &ctx).unwrap();
        let b = s.open(user(NodeKind::Primary, 2), &ctx).unwrap();
        let mb = s.get_magic(b, &ctx).unwrap();
        assert_eq!(s.auth_magic(m, mb), Ok(()));
        // Pretend the counter has wrapped around to b's consumed number.
        s.next_magic = mb;
        let c = s.open(user(NodeKind::Primary, 3), &ctx).unwrap();
        let mc = s.get_magic(c, &ctx).unwrap();
        assert_ne!(mc, mb);
        // And the counter skips 0 when it wraps.
        s.next_magic = u32::MAX;
        let d = s.open(user(NodeKind::Primary, 4), &ctx).unwrap();
        let e = s.open(user(NodeKind::Primary, 5), &ctx).unwrap();
        assert_eq!(s.get_magic(d, &ctx), Ok(u32::MAX));
        let me = s.get_magic(e, &ctx).unwrap();
        assert!(me != 0 && me != mb && me != mc);
        s.check();
    }

    #[test]
    fn render_files_skip_auth_and_master() {
        let ctx = ctx();
        let mut s = DeviceState::new();
        let r = s.open(user(NodeKind::Render, 1), &ctx).unwrap();
        assert_eq!(s.permit(r, R0, || false), Ok(()));
        assert_eq!(s.permit(r, PRIMARY_ONLY, || true), Err(Errno::EACCES));
        assert_eq!(s.permit(r, Perm { auth: true, render_allow: true, ..R0 }, || false), Ok(()));
        assert_eq!(s.get_magic(r, &ctx), Err(Errno::EINVAL));
        assert_eq!(s.set_master(r, || true, 1, &ctx), Err(Errno::EINVAL));
        assert!(!s.unique_set(r));
        // A render open does not take the master role from a later card open.
        let p = s.open(user(NodeKind::Primary, 2), &ctx).unwrap();
        assert!(s.is_current_master(p));
        s.check();
    }

    #[test]
    fn master_hand_over() {
        let ctx = ctx();
        let mut s = DeviceState::new();
        let a = s.open(user(NodeKind::Primary, 1), &ctx).unwrap();
        let b = s.open(user(NodeKind::Primary, 2), &ctx).unwrap();
        let magic_b = s.get_magic(b, &ctx).unwrap();
        // Only the master itself, from the opening process, may drop.
        assert_eq!(s.drop_master(a, || false, 99), Err(Errno::EACCES));
        assert_eq!(s.drop_master(b, || false, 2), Err(Errno::EACCES));
        assert_eq!(s.drop_master(b, || true, 2), Err(Errno::EINVAL));
        assert_eq!(s.drop_master(a, || false, 1), Ok(()));
        assert!(!s.is_current_master(a));
        s.check();
        // An unprivileged file that never was master cannot take over.
        assert_eq!(s.set_master(b, || false, 2, &ctx), Err(Errno::EACCES));
        // A privileged one can, and gets a new master object: its old magic
        // is gone, and a is no longer in its namespace.
        assert_eq!(s.set_master(b, || true, 2, &ctx), Ok(()));
        assert!(s.is_current_master(b) && s.file(b).unwrap().authenticated);
        assert_eq!(s.file(b).unwrap().magic(), 0);
        s.check();
        assert_eq!(s.set_master(b, || false, 2, &ctx), Ok(()), "already master");
        assert_eq!(s.set_master(a, || false, 1, &ctx), Err(Errno::EBUSY));
        let magic_a = s.get_magic(a, &ctx).unwrap();
        assert_eq!(s.auth_magic(b, magic_a), Err(Errno::EINVAL), "other namespace");
        assert_ne!(magic_a, magic_b);
        // b drops, a (a former master, same process) takes back its object.
        assert_eq!(s.drop_master(b, || false, 2), Ok(()));
        assert_eq!(s.set_master(a, || false, 1, &ctx), Ok(()));
        assert!(s.is_current_master(a));
        s.check();
        // New files bind to a's object, so a can authenticate them.
        let c = s.open(user(NodeKind::Primary, 3), &ctx).unwrap();
        let mc = s.get_magic(c, &ctx).unwrap();
        assert_eq!(s.auth_magic(a, mc), Ok(()));
        s.check();
    }

    #[test]
    fn privilege_is_asked_only_when_needed() {
        let ctx = ctx();
        let mut s = DeviceState::new();
        let m = s.open(user(NodeKind::Primary, 1), &ctx).unwrap();
        let never = || -> bool { panic!("privilege checked for an unprivileged operation") };
        assert_eq!(s.permit(m, AUTH_MASTER, never), Ok(()));
        assert_eq!(s.permit(m, PRIMARY_ONLY, never), Ok(()));
        // A former master of the same process needs no privilege.
        assert_eq!(s.drop_master(m, never, 1), Ok(()));
        assert_eq!(s.set_master(m, never, 1, &ctx), Ok(()));
        // Only ROOT_ONLY and the master ioctls of other files ask.
        let asked = core::cell::Cell::new(0);
        let ask = || {
            asked.set(asked.get() + 1);
            false
        };
        assert_eq!(s.permit(m, ROOT_AUTH_MASTER, ask), Err(Errno::EACCES));
        assert_eq!(s.drop_master(m, ask, 2), Err(Errno::EACCES));
        assert_eq!(asked.get(), 2);
    }

    /// The master snapshot of `drm_file` (facade 2.0) through every
    /// transition: the first open, SET_MASTER, DROP_MASTER, the close of
    /// the master, and a later open that becomes master again.
    #[test]
    fn file_flags_track_master() {
        let ctx = ctx();
        let mut s = DeviceState::new();
        let flags = |s: &DeviceState, c| {
            let f: FileFlags = s.file_flags(c).unwrap();
            (f.is_master, f.was_master)
        };
        let a = s.open(user(NodeKind::Primary, 1), &ctx).unwrap();
        let b = s.open(user(NodeKind::Primary, 2), &ctx).unwrap();
        let r = s.open(user(NodeKind::Render, 3), &ctx).unwrap();
        assert_eq!(flags(&s, a), (true, true), "the first open is master");
        assert_eq!(flags(&s, b), (false, false));
        assert_eq!(flags(&s, r), (false, false));
        assert_eq!(s.drop_master(a, || false, 1), Ok(()));
        assert_eq!(flags(&s, a), (false, true));
        assert_eq!(s.set_master(b, || true, 2, &ctx), Ok(()));
        assert_eq!(flags(&s, b), (true, true));
        assert_eq!(flags(&s, a), (false, true));
        assert_eq!(s.set_master(a, || false, 1, &ctx), Err(Errno::EBUSY));
        assert_eq!(flags(&s, a), (false, true), "a refused SET_MASTER changes nothing");
        assert!(s.close(b));
        assert!(s.file_flags(b).is_none());
        assert_eq!(flags(&s, a), (false, true), "the master's close makes nobody master");
        let c = s.open(user(NodeKind::Primary, 4), &ctx).unwrap();
        assert_eq!(flags(&s, c), (true, true));
        // The client capabilities come along.
        let all = crate::driver::Features::MODESET.union(crate::driver::Features::ATOMIC);
        s.file_mut(c).unwrap().caps.set(all, uapi_atomic(), 1).unwrap();
        let f = s.file_flags(c).unwrap();
        assert_eq!(f.linux(), [false, true, true, true, false, false, true, true, false]);
        s.check();
    }

    fn uapi_atomic() -> u64 {
        u64::from(drm_uapi::DRM_CLIENT_CAP_ATOMIC)
    }

    /// FileFlags::store writes Linux's booleans in place and nothing else.
    #[test]
    fn file_flags_store_into_drm_file() {
        let mut fp = drm_facade_sys::drm_file { pid: 77, client_id: 0x1234, ..Default::default() };
        let f = FileFlags {
            caps: ClientCaps {
                stereo_3d: true,
                universal_planes: false,
                atomic: true,
                aspect_ratio: false,
                writeback_connectors: true,
            },
            is_master: true,
            was_master: false,
        };
        // SAFETY: a live structure on the stack.
        unsafe { f.store(&mut fp) };
        assert!(fp.stereo_allowed && !fp.universal_planes && fp.atomic && !fp.aspect_ratio_allowed);
        assert!(fp.writeback_connectors && !fp.plane_color_pipeline && !fp.was_master && fp.is_master);
        assert!(!fp.supports_virtualized_cursor_plane);
        assert_eq!((fp.pid, fp.client_id), (77, 0x1234));
        let none = FileFlags::default();
        // SAFETY: as above.
        unsafe { none.store(&mut fp) };
        assert_eq!(none.linux(), [false; 9]);
        assert!(!fp.stereo_allowed && !fp.atomic && !fp.writeback_connectors && !fp.is_master);
    }

    #[test]
    fn privileged_openers_are_authenticated() {
        let ctx = ctx();
        let mut s = DeviceState::new();
        let _m = s.open(user(NodeKind::Primary, 1), &ctx).unwrap();
        let r = s.open(root(NodeKind::Primary, 2), &ctx).unwrap();
        assert!(s.file(r).unwrap().authenticated);
        assert!(!s.is_current_master(r));
        s.check();
    }

    #[test]
    fn unique_and_version() {
        let ctx = ctx();
        let mut s = DeviceState::new();
        let m = s.open(user(NodeKind::Primary, 1), &ctx).unwrap();
        let c = s.open(user(NodeKind::Primary, 2), &ctx).unwrap();
        assert!(!s.unique_set(c));
        let v = |di_major, di_minor, dd_major, dd_minor| SetVersion { di_major, di_minor, dd_major, dd_minor };
        assert_eq!(s.set_version(m, v(2, 0, -1, -1), 1, 0), Err(Errno::EINVAL));
        assert_eq!(s.set_version(m, v(1, 5, -1, -1), 1, 0), Err(Errno::EINVAL));
        assert_eq!(s.set_version(m, v(-1, -1, 1, 1), 1, 0), Err(Errno::EINVAL));
        assert_eq!(s.set_version(m, v(-1, -1, 2, 0), 1, 0), Err(Errno::EINVAL));
        assert_eq!(s.set_version(m, v(1, 0, 1, 0), 1, 0), Ok(()));
        assert!(!s.unique_set(c), "1.0 does not set the bus ID");
        assert_eq!(s.if_version(), 1 << 16);
        assert_eq!(s.set_version(m, v(1, 4, -1, -1), 1, 0), Ok(()));
        assert_eq!(s.if_version(), (1 << 16) | 4);
        assert!(s.unique_set(c) && s.unique_set(m), "per master namespace");
        assert_eq!(s.set_unique(m), Err(Errno::EBUSY));
        // A new master object starts without a bus ID.
        assert!(s.close(m));
        let n = s.open(user(NodeKind::Primary, 3), &ctx).unwrap();
        assert!(!s.unique_set(n));
        assert!(s.unique_set(c), "c is still bound to the old object");
        assert_eq!(s.set_unique(n), Ok(()));
        assert!(s.unique_set(n));
        s.check();
    }

    #[test]
    fn clone_ids_run_out_cleanly() {
        let ctx = ctx();
        let mut s = DeviceState::new();
        let mut ids = Vec::new();
        for i in 0..MAX_FILES {
            ids.push(s.open(user(NodeKind::Render, i as i32), &ctx).unwrap());
        }
        assert_eq!(s.open(user(NodeKind::Primary, 0), &ctx), Err(Errno::EMFILE));
        s.check();
        assert!(s.close(ids[100]));
        assert_eq!(s.open(user(NodeKind::Primary, 0), &ctx), Ok(ids[100]));
        s.check();
    }

    /// The device keeps the state behind one mutex and performs each
    /// operation as one critical section. This drives the same operations
    /// from eight threads through a `std::sync::Mutex` and checks the
    /// invariants after every step: at most one current master, magic
    /// numbers unique, reference counts exact, nothing left behind once
    /// every file is closed.
    #[test]
    fn concurrent_open_close_auth_master() {
        let state = Arc::new(Mutex::new(DeviceState::new()));
        let threads: Vec<_> = (0..8)
            .map(|t| {
                let state = Arc::clone(&state);
                std::thread::spawn(move || {
                    let ctx = ctx();
                    let mut seed = 0x9e37_79b9_u32.wrapping_mul(t + 1);
                    let mut mine: Vec<CloneId> = Vec::new();
                    for _ in 0..4000 {
                        seed ^= seed << 13;
                        seed ^= seed >> 17;
                        seed ^= seed << 5;
                        let mut s = state.lock().unwrap();
                        let pick = |n: usize| (seed as usize >> 8) % n.max(1);
                        match seed % 8 {
                            0 | 1 => {
                                let kind = if seed & 0x100 != 0 { NodeKind::Render } else { NodeKind::Primary };
                                let who = Opener { kind, privileged: seed & 0x200 != 0, pid: t as i32 };
                                if let Ok(c) = s.open(who, &ctx) {
                                    mine.push(c);
                                }
                            }
                            2 if !mine.is_empty() => {
                                let c = mine.swap_remove(pick(mine.len()));
                                assert!(s.close(c));
                            }
                            3 if !mine.is_empty() => {
                                let _ = s.get_magic(mine[pick(mine.len())], &ctx);
                            }
                            4 if !mine.is_empty() => {
                                // Authenticate some waiting magic if we hold the master.
                                let c = mine[pick(mine.len())];
                                if s.permit(c, AUTH_MASTER, || false).is_ok() {
                                    let m = s.magics.first().map(|e| e.magic).unwrap_or(1);
                                    let _ = s.auth_magic(c, m);
                                }
                            }
                            5 if !mine.is_empty() => {
                                let _ = s.set_master(mine[pick(mine.len())], || seed & 0x400 != 0, t as i32, &ctx);
                            }
                            6 if !mine.is_empty() => {
                                let _ = s.drop_master(mine[pick(mine.len())], || false, t as i32);
                            }
                            _ => {}
                        }
                        s.check();
                    }
                    let mut s = state.lock().unwrap();
                    for c in mine {
                        assert!(s.close(c));
                    }
                    s.check();
                })
            })
            .collect();
        for t in threads {
            t.join().unwrap();
        }
        let s = state.lock().unwrap();
        assert_eq!(s.open_files(), 0);
        assert!(s.masters.is_empty() && s.magics.is_empty() && s.current.is_none());
    }
}
