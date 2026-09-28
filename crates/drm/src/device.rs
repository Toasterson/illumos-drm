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

use ddi::prelude::*;
use ddi::sys;
use drm_uapi as uapi;

use crate::caps;
use crate::driver::{BUS_ID_MAX, BusId, Driver, Features};
use crate::ioctl::{self, Op};
use crate::minor::{self, CloneId, MAX_DEVICES, NodeKind};
use crate::state::{DeviceState, IF_MAJOR, IF_MINOR, Opener, SetVersion};
use crate::uaccess::{UserArg, copy_in_bytes, copy_out_bytes};

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
    // Dropping these removes the minor nodes.
    _card: MinorNode,
    _render: Option<MinorNode>,
}

impl<D: Driver> Device<D> {
    /// Registers a device for the driver instance `dev`: creates the
    /// primary node `card<N>` and, if the driver has [`Features::RENDER`],
    /// the render node `renderD<128+N>`, where `N` is the instance number.
    /// Both nodes have the node type `ddi_display:drm`
    /// (`DDI_NT_DISPLAY_DRM`), for which devfsadm creates the `/dev/dri`
    /// links (see `packaging/devlink.tab`).
    pub fn attach(dev: DevInfo, driver: D, _ctx: &Sleepable) -> Result<Device<D>, DdiError> {
        let Some(index) = u32::try_from(dev.instance().0).ok().filter(|i| *i < MAX_DEVICES) else {
            dev.warn(format_args!(
                "instance {} is above the {MAX_DEVICES} DRM devices a driver can have",
                dev.instance().0
            ));
            return Err(DdiError);
        };
        let mut bus_id = BusId::new();
        if driver.bus_id(&dev, &mut bus_id).is_err() {
            dev.warn(format_args!("bus ID does not fit in {BUS_ID_MAX} bytes"));
            return Err(DdiError);
        }
        let card = Self::node(&dev, NodeKind::Primary, index)?;
        let render = if D::FEATURES.contains(Features::RENDER) {
            Some(Self::node(&dev, NodeKind::Render, index)?)
        } else {
            None
        };
        dev.report();
        Ok(Device {
            driver,
            dev,
            index,
            bus_id,
            state: Mutex::new(DeviceState::new()),
            has_render: render.is_some(),
            _card: card,
            _render: render,
        })
    }

    fn node(dev: &DevInfo, kind: NodeKind, index: u32) -> Result<MinorNode, DdiError> {
        let name = minor::node_name(kind, index).ok_or(DdiError)?;
        let m = minor::node_minor(kind, index).ok_or(DdiError)?;
        dev.create_minor_node(name, MinorKind::Char, Minor(m), NodeType::Custom(sys::DDI_NT_DISPLAY_DRM))
    }

    /// Unregisters the device. Refuses, and hands the device back, while
    /// files are open.
    #[allow(clippy::result_large_err, reason = "the shape of ddi::Driver::detach, which hands the instance back")]
    pub fn detach(self, _ctx: &Sleepable) -> Result<(), (Device<D>, DdiError)> {
        let open = self.state.lock().open_files();
        if open != 0 {
            self.dev.warn(format_args!("detach refused: {open} files open"));
            return Err((self, DdiError));
        }
        Ok(())
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
        arg: &IoctlArg<'_>,
    ) -> Result<(), Errno> {
        let mut v = UserArg::<uapi::drm_version>::read(arg)?;
        self.state.lock().permit(c, perm, privileged)?;
        // SAFETY: only field assignments below.
        let a = unsafe { v.get_mut() };
        a.version_major = D::MAJOR;
        a.version_minor = D::MINOR;
        a.version_patchlevel = D::PATCHLEVEL;
        copy_field(arg, &mut a.name_len, a.name as usize, D::NAME.to_bytes())?;
        copy_field(arg, &mut a.date_len, a.date as usize, D::DATE.to_bytes())?;
        copy_field(arg, &mut a.desc_len, a.desc as usize, D::DESC.to_bytes())?;
        v.write(arg)
    }

    fn get_unique(
        &self,
        c: CloneId,
        perm: crate::state::Perm,
        privileged: &dyn Fn() -> bool,
        arg: &IoctlArg<'_>,
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
        arg: &IoctlArg<'_>,
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
fn copy_field(arg: &IoctlArg<'_>, len: &mut usize, addr: usize, value: &[u8]) -> Result<(), Errno> {
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
        let d = minor::decode(minor.0).ok_or(Errno::ENXIO)?;
        if d.index != self.index || d.clone.is_some() || (d.kind == NodeKind::Render && !self.has_render) {
            return Err(Errno::ENXIO);
        }
        let who = Opener { kind: d.kind, privileged: cred.has_sys_devices_priv(), pid: caller_pid() };
        let mut s = self.state.lock();
        let clone = s.open(who, ctx)?;
        match minor::file_minor(d.kind, self.index, clone) {
            Some(m) => Ok(Some(Minor(m))),
            None => {
                s.close(clone);
                Err(Errno::ENXIO)
            }
        }
    }

    fn close(
        &self,
        minor: Minor,
        _flags: OpenFlags,
        _otyp: OpenType,
        _cred: &Cred<'_>,
        _ctx: &Sleepable,
    ) -> Result<()> {
        let d = minor::decode(minor.0).ok_or(Errno::ENXIO)?;
        // A node minor without a clone id was never handed out by
        // clone_open, so there is no file to forget.
        if let Some(c) = d.clone {
            self.state.lock().close(c);
        }
        Ok(())
    }

    fn ioctl(&self, minor: Minor, cmd: IoctlCmd, arg: IoctlArg<'_>, cred: &Cred<'_>, ctx: &Sleepable) -> Result<i32> {
        // Only the LP64 ABI exists. A 32-bit caller gets ENOTSUP for every
        // core command (recognised by group and number, since its structure
        // sizes differ) instead of a misread structure, and ENOTTY for
        // anything else, as any caller would.
        if arg.model() == DataModel::Ilp32 {
            return Err(if ioctl::is_core_nr(cmd.0 as u32) { Errno::ENOTSUP } else { Errno::ENOTTY });
        }
        let entry = ioctl::lookup(cmd.0 as u32).ok_or(Errno::ENOTTY)?;
        let c = minor::decode(minor.0).and_then(|d| d.clone).ok_or(Errno::ENXIO)?;
        // drv_priv(9F) is only called when an ioctl's permission needs it.
        let privileged = &|| cred.has_sys_devices_priv();
        let perm = entry.perm;
        match entry.op {
            Op::Version => self.version(c, perm, privileged, &arg)?,
            Op::GetUnique => self.get_unique(c, perm, privileged, &arg)?,
            Op::SetUnique => self.set_unique(c, perm, privileged, &arg)?,
            Op::GetMagic => {
                let magic = {
                    let mut s = self.state.lock();
                    s.permit(c, perm, privileged)?;
                    s.get_magic(c, ctx)?
                };
                let mut a = UserArg::<uapi::drm_auth>::zeroed();
                // SAFETY: a field assignment.
                unsafe { a.get_mut() }.magic = magic;
                a.write(&arg)?;
            }
            Op::AuthMagic => {
                let a = UserArg::<uapi::drm_auth>::read(&arg)?;
                let mut s = self.state.lock();
                s.permit(c, perm, privileged)?;
                s.auth_magic(c, a.get().magic)?;
            }
            Op::GetClient => {
                let mut cl = UserArg::<uapi::drm_client>::read(&arg)?;
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
                cl.write(&arg)?;
            }
            Op::SetVersion => {
                let mut v = UserArg::<uapi::drm_set_version>::read(&arg)?;
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
                    s.set_version(c, sv, D::MAJOR, D::MINOR)
                };
                // The versions go back to the caller whether or not the
                // request was acceptable; libdrm reads them in both cases.
                // SAFETY: only field assignments below.
                let a = unsafe { v.get_mut() };
                a.drm_di_major = IF_MAJOR;
                a.drm_di_minor = IF_MINOR;
                a.drm_dd_major = D::MAJOR;
                a.drm_dd_minor = D::MINOR;
                v.write(&arg)?;
                r?;
            }
            Op::GetCap => {
                let mut g = UserArg::<uapi::drm_get_cap>::read(&arg)?;
                self.state.lock().permit(c, perm, privileged)?;
                let value = caps::get_cap(D::FEATURES, &D::MODE_CONFIG, g.get().capability)?;
                // SAFETY: a field assignment.
                unsafe { g.get_mut() }.value = value;
                g.write(&arg)?;
            }
            Op::SetClientCap => {
                let a = UserArg::<uapi::drm_set_client_cap>::read(&arg)?;
                let mut s = self.state.lock();
                s.permit(c, perm, privileged)?;
                let f = s.file_mut(c).ok_or(Errno::ENXIO)?;
                f.caps.set(D::FEATURES, a.get().capability, a.get().value)?;
            }
            Op::SetMaster => {
                let mut s = self.state.lock();
                s.permit(c, perm, privileged)?;
                s.set_master(c, privileged, caller_pid(), ctx)?;
            }
            Op::DropMaster => {
                let mut s = self.state.lock();
                s.permit(c, perm, privileged)?;
                s.drop_master(c, privileged, caller_pid())?;
            }
        }
        Ok(0)
    }
}
