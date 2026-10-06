// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Facade 2.0: mode setting by Linux's KMS code carried in C
//! (`include/drm/drm_lkms.h`, `misc/drm_kms`).
//!
//! A C driver whose table has `illumos_lkms` is a Linux-KMS driver. Its
//! device registers as any other (`drm_dev_register()`) and gets the
//! files' event queues; mode setting starts later, when the driver calls
//! `drm_dev_illumos_lkms_register()` with `DRIVER_MODESET` in
//! `drm_device.driver_features` (the LinuxKPI layer does where i915 calls
//! Linux's `drm_dev_register()`, once its mode configuration exists).
//! Until then the device answers as one without mode setting
//! ([`caps_view`]). The core keeps what it keeps for every device (the
//! ioctl entry, argument copies, permission flags, master and
//! authentication, client capabilities, `GET_CAP`, files, GEM, PRIME,
//! sync objects) and hands the rest to the KMS code:
//!
//! * `drm_dev_illumos_lkms_register()` takes the `GET_CAP` limits from the
//!   KMS code's `mode_config` and calls `register_dev`; `unregister_dev`
//!   runs once, if that succeeded, before the minor nodes go
//!   ([`Registration`]);
//! * every command of the core's mode-setting table
//!   ([`crate::kms::ioctl`]) goes, after the core's permission check, to
//!   the KMS code's `ioctl` hook with the kernel copy of its argument
//!   ([`forward_ioctl`]), for files the KMS code opened;
//! * a file opened once mode setting runs opens with the KMS code's
//!   `file_open` before the driver's `open` and closes with its
//!   `file_close` before the core releases the file's GEM handles and the
//!   driver's `postclose` runs; `lastclose` follows the close of the
//!   device's last open file ([`open_file`], [`close_file`]);
//! * the KMS code delivers vblank and flip events into the core's per-file
//!   queues, which `read(2)` and `chpoll(9E)` serve ([`events`], and
//!   `lkms::dev` in the kernel).
//!
//! The sequencing here is plain code over the [`LkmsHooks`] trait, which
//! `misc/drm_core` implements over the C table and the host tests with a
//! recording mock.

use ddi::{Errno, Sleepable};

use crate::driver::{Features, ModeConfig};
use crate::ioctl::SizePlan;

#[cfg(not(test))]
pub mod dev;
pub mod events;

/// The hooks of a Linux KMS implementation (`struct
/// drm_illumos_lkms_funcs`), as the core sequences them; `file` is the
/// file's clone ID. Every hook runs in thread context without core locks
/// held. The `ioctl` hook is the driver's ([`crate::Driver::lkms_ioctl`]),
/// since it needs the ioctl in progress.
pub trait LkmsHooks: Sync {
    /// `file_open`: after the core set the file up, before the driver's
    /// `open`.
    fn file_open(&self, file: u32, ctx: &Sleepable) -> Result<(), Errno>;
    /// `file_close`: before the core releases the file's GEM handles and
    /// calls the driver's `postclose`.
    fn file_close(&self, file: u32, ctx: &Sleepable);
    /// `lastclose`: after the device's last open file closed.
    fn lastclose(&self, ctx: &Sleepable);
    /// `register_dev`: in `drm_dev_illumos_lkms_register()`, after
    /// `mode_config`.
    fn register_dev(&self, ctx: &Sleepable) -> Result<(), Errno>;
    /// `unregister_dev`: in a `drm_dev_unregister()` that is going to
    /// succeed, before the minor nodes go.
    fn unregister_dev(&self, ctx: &Sleepable);
    /// `mode_config`: the limits `GET_CAP` reports.
    fn mode_config(&self, ctx: &Sleepable) -> ModeConfig;
}

/// An ioctl argument the core copies with Linux's size rule
/// (`uaccess::CoreArg` in the kernel).
pub trait StructArg {
    /// The call's size plan.
    fn size_plan(&self) -> SizePlan;
    /// Fills the kernel structure at `dst`, `len` zero bytes, from the
    /// caller.
    ///
    /// # Safety
    /// `dst` must be valid for `len` bytes of writes.
    unsafe fn copy_in(&self, dst: *mut u8, len: usize) -> Result<(), Errno>;
    /// Copies the kernel structure at `src`, `len` initialised bytes,
    /// back to the caller, as far as the command copies out.
    ///
    /// # Safety
    /// `src` must be valid for `len` bytes of reads, all initialised.
    unsafe fn copy_out(&self, src: *const u8, len: usize) -> Result<(), Errno>;
}

/// The largest kernel structure of a command: the size field of an
/// illumos command is eight bits.
pub const ARG_MAX: usize = 256;

/// The kernel copy of an argument, aligned for any field.
#[repr(C, align(8))]
struct ArgBuf([u8; ARG_MAX]);

/// The first of Linux's kernel-internal error numbers (`ERESTARTSYS`, 512,
/// and up), which never reach user space on Linux. The KMS code maps them
/// to illumos numbers (`lkpi_errno()`) before it returns.
pub const LINUX_INTERNAL_ERRNO: i32 = 512;

/// An errno from a hook's negative result; 0 and positive values are
/// success, as for the facade's ioctl handlers. A kernel-internal Linux
/// number that got through (-512 to -4095, Linux's `MAX_ERRNO`) is no
/// errno user space knows: `EIO` (the caller warns, see
/// [`is_linux_internal`]). A value below -4095 is no errno: `EINVAL`.
pub fn from_neg(r: i32) -> Result<(), Errno> {
    match r {
        0.. => Ok(()),
        _ if is_linux_internal(r) => Err(Errno::EIO),
        -4095..0 => Err(Errno::new(-r).unwrap_or(Errno::EINVAL)),
        _ => Err(Errno::EINVAL),
    }
}

/// Whether a hook's result is one of Linux's kernel-internal error
/// numbers, which [`from_neg`] turns into `EIO`.
pub fn is_linux_internal(r: i32) -> bool {
    (-4095..=-LINUX_INTERNAL_ERRNO).contains(&r)
}

/// Runs a mode-setting command of a Linux-KMS device: the core's
/// permission check (`permit`), the kernel copy of the argument (the
/// table's size, the caller's structure zero-extended or its tail kept,
/// [`SizePlan`]), the KMS code's handler (`call`, with the command's
/// number and the copy), whose negative errno fails the ioctl, and the
/// copy back for a command that writes. Nothing is copied back after a
/// failure, as for the core's own handlers.
pub fn forward_ioctl<A: StructArg + ?Sized>(
    nr: u32,
    arg: &A,
    permit: impl FnOnce() -> Result<(), Errno>,
    call: impl FnOnce(u32, &mut [u8]) -> i32,
) -> Result<(), Errno> {
    permit()?;
    let k = arg.size_plan().kernel;
    let mut buf = ArgBuf([0; ARG_MAX]);
    let data = buf.0.get_mut(..k).ok_or(Errno::EINVAL)?;
    // SAFETY: k bytes of the local buffer.
    unsafe { arg.copy_in(data.as_mut_ptr(), k)? };
    from_neg(call(nr, data))?;
    // SAFETY: k initialised bytes.
    unsafe { arg.copy_out(data.as_ptr(), k) }
}

/// `open(9E)` of a Linux-KMS device's file, once the core has set it up:
/// the KMS code's `file_open`, then the driver's `open`
/// (`driver_open`). If the driver's open fails, `file_close` undoes the
/// KMS code's; either failure fails the open.
pub fn open_file(
    h: &dyn LkmsHooks,
    file: u32,
    driver_open: impl FnOnce() -> Result<(), Errno>,
    ctx: &Sleepable,
) -> Result<(), Errno> {
    h.file_open(file, ctx)?;
    if let Err(e) = driver_open() {
        h.file_close(file, ctx);
        return Err(e);
    }
    Ok(())
}

/// `close(9E)` of a file: with the hooks of a device whose mode setting
/// runs (`h`), `file_close` first if the KMS code opened the file
/// (`opened`; Linux's `drm_file_free()` releases framebuffers and user
/// blobs, which hold GEM references, before the handles), then the core's
/// release (`release`: GEM handles, sync objects, the driver's
/// `postclose`, the file's state; it answers whether no file of the device
/// is open any more), then `lastclose` if that was the last file. Without
/// hooks only the release.
pub fn close_file(h: Option<&dyn LkmsHooks>, opened: bool, file: u32, release: impl FnOnce() -> bool, ctx: &Sleepable) {
    if let (Some(h), true) = (h, opened) {
        h.file_close(file, ctx);
    }
    let last = release();
    if let (Some(h), true) = (h, last) {
        h.lastclose(ctx);
    }
}

/// What `GET_CAP` and `SET_CLIENT_CAP` see of a device: its features and
/// mode configuration (`info`), or, for a Linux-KMS driver's device, the
/// features and configuration `drm_dev_illumos_lkms_register()` started
/// mode setting with (`started`), and until then the device's features
/// without `MODESET` and `ATOMIC` and the default configuration, as for a
/// device without mode setting.
pub fn caps_view(
    info: (Features, ModeConfig),
    lkms: bool,
    started: Option<(Features, ModeConfig)>,
) -> (Features, ModeConfig) {
    match (lkms, started) {
        (false, _) => info,
        (true, Some(s)) => s,
        (true, None) => (info.0.without(Features::MODESET.union(Features::ATOMIC)), ModeConfig::DEFAULT),
    }
}

/// Whether a Linux-KMS device's `register_dev` succeeded and
/// `unregister_dev` is still owed.
#[derive(Debug, Default)]
pub struct Registration {
    registered: bool,
}

impl Registration {
    /// Not registered.
    pub const fn new() -> Registration {
        Registration { registered: false }
    }

    /// Whether `unregister_dev` is owed.
    pub fn is_registered(&self) -> bool {
        self.registered
    }

    /// The hooks of `drm_dev_illumos_lkms_register()` on a registered
    /// device: the KMS code's `mode_config` (returned: the `GET_CAP`
    /// limits), then `register_dev`, whose error is the result (and
    /// nothing is owed then). `EBUSY` if `register_dev` succeeded before.
    pub fn start(&mut self, h: &dyn LkmsHooks, ctx: &Sleepable) -> Result<ModeConfig, Errno> {
        if self.registered {
            return Err(Errno::EBUSY);
        }
        let m = h.mode_config(ctx);
        h.register_dev(ctx)?;
        self.registered = true;
        Ok(m)
    }

    /// `unregister_dev`, once: in the first `drm_dev_unregister()` that
    /// will succeed (or the unplug that precedes the last put), never
    /// without a successful `register_dev`.
    pub fn unregister(&mut self, h: &dyn LkmsHooks, ctx: &Sleepable) {
        if core::mem::take(&mut self.registered) {
            h.unregister_dev(ctx);
        }
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use crate::ioctl::{SizePlan, assemble_out};
    use crate::kms::ioctl::{KmsOp, TABLE as KMS_TABLE, lookup};
    use crate::minor::NodeKind;
    use crate::state::{DeviceState, Opener};
    use drm_uapi as uapi;
    use drm_uapi::ioc;
    use std::cell::RefCell;
    use std::format;
    use std::string::String;
    use std::vec::Vec;

    fn ctx() -> Sleepable {
        // SAFETY: host tests run in an ordinary thread.
        unsafe { Sleepable::assume() }
    }

    /// A Linux KMS implementation that records its calls.
    #[derive(Default)]
    struct Mock {
        log: std::sync::Mutex<Vec<String>>,
        fail_open: bool,
        fail_register: bool,
    }

    impl Mock {
        fn note(&self, s: impl Into<String>) {
            self.log.lock().unwrap().push(s.into());
        }
        fn log(&self) -> Vec<String> {
            core::mem::take(&mut *self.log.lock().unwrap())
        }
    }

    impl LkmsHooks for Mock {
        fn file_open(&self, file: u32, _: &Sleepable) -> Result<(), Errno> {
            self.note(format!("file_open {file}"));
            if self.fail_open { Err(Errno::ENOMEM) } else { Ok(()) }
        }
        fn file_close(&self, file: u32, _: &Sleepable) {
            self.note(format!("file_close {file}"));
        }
        fn lastclose(&self, _: &Sleepable) {
            self.note("lastclose");
        }
        fn register_dev(&self, _: &Sleepable) -> Result<(), Errno> {
            self.note("register_dev");
            if self.fail_register { Err(Errno::ENODEV) } else { Ok(()) }
        }
        fn unregister_dev(&self, _: &Sleepable) {
            self.note("unregister_dev");
        }
        fn mode_config(&self, _: &Sleepable) -> ModeConfig {
            self.note("mode_config");
            ModeConfig { cursor_width: 256, async_page_flip: true, ..ModeConfig::DEFAULT }
        }
    }

    /// A caller's structure in memory, copied as `uaccess::CoreArg` does
    /// (Linux's `drm_ioctl()` size rule).
    struct UserMem {
        plan: SizePlan,
        bytes: RefCell<Vec<u8>>,
        copied_out: RefCell<bool>,
    }

    impl UserMem {
        fn new(table_cmd: u32, cmd: u32, fill: impl Fn(usize) -> u8) -> UserMem {
            let plan = SizePlan::new(table_cmd, cmd);
            UserMem { plan, bytes: RefCell::new((0..plan.user).map(fill).collect()), copied_out: RefCell::new(false) }
        }
    }

    impl StructArg for UserMem {
        fn size_plan(&self) -> SizePlan {
            self.plan
        }
        unsafe fn copy_in(&self, dst: *mut u8, len: usize) -> Result<(), Errno> {
            assert_eq!(len, self.plan.kernel);
            let n = self.plan.copy_in();
            // SAFETY: the caller's contract; n <= len.
            unsafe { core::ptr::copy_nonoverlapping(self.bytes.borrow().as_ptr(), dst, n) };
            Ok(())
        }
        unsafe fn copy_out(&self, src: *const u8, len: usize) -> Result<(), Errno> {
            // SAFETY: the caller's contract.
            let k = unsafe { core::slice::from_raw_parts(src, len) };
            let p = self.plan;
            let tail = self.bytes.borrow()[p.kernel.min(p.user)..][..p.tail_in()].to_vec();
            let mut out = [0u8; 512];
            let n = assemble_out(&p, k, &tail, &mut out).ok_or(Errno::EINVAL)?;
            self.bytes.borrow_mut()[..n].copy_from_slice(&out[..n]);
            *self.copied_out.borrow_mut() = true;
            Ok(())
        }
    }

    /// The device side of the dispatch: the core's permission check on
    /// the device state, then the forward.
    fn dispatch(
        s: &DeviceState,
        c: crate::minor::CloneId,
        cmd: u32,
        arg: &UserMem,
        handler: &dyn Fn(u32, &mut [u8]) -> i32,
    ) -> Result<(), Errno> {
        let e = lookup(cmd).ok_or(Errno::ENOTTY)?;
        forward_ioctl(ioc::ioc_nr(e.cmd) as u32, arg, || s.permit(c, e.perm, || false), |nr, d| handler(nr, d))
    }

    #[test]
    fn forward_checks_permission_first_and_copies_back() {
        let ctx = ctx();
        let mut s = DeviceState::new();
        let master = s.open(Opener { kind: NodeKind::Primary, privileged: false, pid: 1 }, &ctx).unwrap();
        let other = s.open(Opener { kind: NodeKind::Primary, privileged: false, pid: 2 }, &ctx).unwrap();
        let render = s.open(Opener { kind: NodeKind::Render, privileged: false, pid: 3 }, &ctx).unwrap();
        let calls = RefCell::new(Vec::<(u32, usize)>::new());
        let handler = |nr: u32, d: &mut [u8]| {
            calls.borrow_mut().push((nr, d.len()));
            d.fill(0x5a);
            0
        };
        // A master-only command from a file that is not master, and any
        // mode-setting command from a render node: refused before the KMS
        // code sees anything, and nothing is copied back.
        let atomic = uapi::DRM_IOCTL_MODE_ATOMIC;
        let a = UserMem::new(atomic, atomic, |i| i as u8);
        assert_eq!(dispatch(&s, other, atomic, &a, &handler), Err(Errno::EACCES));
        let getres = uapi::DRM_IOCTL_MODE_GETRESOURCES;
        let g = UserMem::new(getres, getres, |_| 1);
        assert_eq!(dispatch(&s, render, getres, &g, &handler), Err(Errno::EACCES));
        assert!(calls.borrow().is_empty() && !*a.copied_out.borrow() && !*g.copied_out.borrow());
        // The master's command reaches the handler with its number and the
        // table's size, and the result comes back.
        assert_eq!(dispatch(&s, master, atomic, &a, &handler), Ok(()));
        assert_eq!(calls.borrow().as_slice(), [(ioc::ioc_nr(atomic) as u32, ioc::ioc_size(atomic))]);
        assert!(a.bytes.borrow().iter().all(|b| *b == 0x5a));
        // Any file may GETRESOURCES.
        assert_eq!(dispatch(&s, other, getres, &g, &handler), Ok(()));
    }

    #[test]
    fn forward_follows_the_size_rule() {
        let ctx = ctx();
        let mut s = DeviceState::new();
        let m = s.open(Opener { kind: NodeKind::Primary, privileged: false, pid: 1 }, &ctx).unwrap();
        let cmd = uapi::DRM_IOCTL_MODE_GETCRTC;
        let k = ioc::ioc_size(cmd);
        // A shorter structure: zero-extended in, truncated out.
        let short = ioc::with_size(cmd, k - 8);
        let a = UserMem::new(cmd, short, |_| 0xee);
        let seen = RefCell::new(Vec::new());
        let h = |_: u32, d: &mut [u8]| {
            *seen.borrow_mut() = d.to_vec();
            d.fill(0x11);
            0
        };
        assert_eq!(dispatch(&s, m, short, &a, &h), Ok(()));
        let v = seen.borrow().clone();
        assert_eq!(v.len(), k);
        assert!(v[..k - 8].iter().all(|b| *b == 0xee) && v[k - 8..].iter().all(|b| *b == 0));
        assert_eq!(a.bytes.borrow().len(), k - 8);
        assert!(a.bytes.borrow().iter().all(|b| *b == 0x11));
        // A longer structure: the tail is kept, never shown to the handler.
        let long = ioc::with_size(cmd, k + 8);
        let a = UserMem::new(cmd, long, |i| i as u8);
        assert_eq!(dispatch(&s, m, long, &a, &h), Ok(()));
        assert_eq!(seen.borrow().len(), k);
        let b = a.bytes.borrow();
        assert!(b[..k].iter().all(|x| *x == 0x11));
        assert_eq!(&b[k..], &(k..k + 8).map(|i| i as u8).collect::<Vec<_>>()[..]);
    }

    #[test]
    fn forward_errors() {
        let ctx = ctx();
        let mut s = DeviceState::new();
        let m = s.open(Opener { kind: NodeKind::Primary, privileged: false, pid: 1 }, &ctx).unwrap();
        let cmd = uapi::DRM_IOCTL_MODE_GETPROPBLOB;
        // The KMS code's errno is the ioctl's; nothing is copied back.
        // An interrupted handler: EINTR (the C side's lkpi_errno() of
        // ERESTARTSYS); a leaked ERESTARTSYS itself: EIO.
        for (r, want) in [
            (-25, Err(Errno::ENOTTY)),
            (-22, Err(Errno::EINVAL)),
            (-2, Err(Errno::ENOENT)),
            (-(Errno::EINTR.get()), Err(Errno::EINTR)),
            (-512, Err(Errno::EIO)),
            (3, Ok(())),
        ] {
            let a = UserMem::new(cmd, cmd, |_| 9);
            let h = |_: u32, d: &mut [u8]| {
                d.fill(0);
                r
            };
            assert_eq!(dispatch(&s, m, cmd, &a, &h), want, "result {r}");
            assert_eq!(*a.copied_out.borrow(), want.is_ok());
        }
        // A command that only writes (no IOC_OUT): nothing comes back.
        let ctl = uapi::DRM_IOCTL_MODESET_CTL;
        assert_eq!(lookup(ctl).map(|e| e.op), Some(KmsOp::ModesetCtl));
        assert_eq!(ioc::ioc_dir(ctl), ioc::IOC_IN);
        let a = UserMem::new(ctl, ctl, |_| 4);
        let zero = |_: u32, d: &mut [u8]| {
            d.fill(0);
            0
        };
        assert_eq!(dispatch(&s, m, ctl, &a, &zero), Ok(()));
        assert!(a.bytes.borrow().iter().all(|b| *b == 4));
        // Every command of the table has a kernel structure that fits.
        for e in KMS_TABLE {
            assert!(ioc::ioc_size(e.cmd) <= ARG_MAX, "{:?}", e.op);
        }
    }

    #[test]
    fn open_order_and_undo() {
        let ctx = ctx();
        let h = Mock::default();
        let order = RefCell::new(Vec::new());
        assert_eq!(
            open_file(
                &h,
                3,
                || {
                    order.borrow_mut().push("driver open");
                    Ok(())
                },
                &ctx
            ),
            Ok(())
        );
        assert_eq!(h.log(), ["file_open 3"]);
        // The driver's open fails: the KMS code's open is undone.
        assert_eq!(open_file(&h, 4, || Err(Errno::EIO), &ctx), Err(Errno::EIO));
        assert_eq!(h.log(), ["file_open 4", "file_close 4"]);
        // The KMS code's open fails: the driver's open never runs.
        let h = Mock { fail_open: true, ..Mock::default() };
        let ran = RefCell::new(false);
        assert_eq!(
            open_file(
                &h,
                5,
                || {
                    *ran.borrow_mut() = true;
                    Ok(())
                },
                &ctx
            ),
            Err(Errno::ENOMEM)
        );
        assert!(!*ran.borrow());
        assert_eq!(h.log(), ["file_open 5"]);
        assert_eq!(order.borrow().as_slice(), ["driver open"]);
    }

    #[test]
    fn close_order_and_lastclose() {
        let ctx = ctx();
        let h = Mock::default();
        // Two files; the KMS code closes each before the core's release,
        // and lastclose follows the second.
        let mut open = 2;
        for f in [1u32, 2] {
            close_file(
                Some(&h),
                true,
                f,
                || {
                    h.note(format!("release {f}"));
                    open -= 1;
                    open == 0
                },
                &ctx,
            );
        }
        assert_eq!(h.log(), ["file_close 1", "release 1", "file_close 2", "release 2", "lastclose"]);
        // A file opened before mode setting started: no file_close, but
        // lastclose still follows the last file.
        close_file(Some(&h), false, 3, || true, &ctx);
        assert_eq!(h.log(), ["lastclose"]);
        // Without hooks (an R3 or render-only device, or before mode
        // setting started), only the release.
        let mut ran = false;
        close_file(
            None,
            true,
            1,
            || {
                ran = true;
                true
            },
            &ctx,
        );
        assert!(ran);
        assert!(h.log().is_empty());
    }

    #[test]
    fn start_and_unregister_order() {
        let ctx = ctx();
        let h = Mock::default();
        let mut reg = Registration::new();
        let m = reg.start(&h, &ctx);
        assert_eq!(m.map(|m| (m.cursor_width, m.async_page_flip)), Ok((256, true)));
        assert!(reg.is_registered());
        assert_eq!(h.log(), ["mode_config", "register_dev"]);
        // A second start is refused without a hook.
        assert_eq!(reg.start(&h, &ctx), Err(Errno::EBUSY));
        assert!(h.log().is_empty());
        // unregister_dev runs once: the unplug, then the last put's
        // unregistration finds nothing owed.
        reg.unregister(&h, &ctx);
        reg.unregister(&h, &ctx);
        assert_eq!(h.log(), ["unregister_dev"]);
        assert!(!reg.is_registered());
    }

    #[test]
    fn start_failure_owes_nothing() {
        let ctx = ctx();
        let h = Mock { fail_register: true, ..Mock::default() };
        let mut reg = Registration::new();
        assert_eq!(reg.start(&h, &ctx), Err(Errno::ENODEV));
        assert_eq!(h.log(), ["mode_config", "register_dev"]);
        assert!(!reg.is_registered());
        reg.unregister(&h, &ctx);
        assert!(h.log().is_empty(), "no unregister_dev without register_dev");
    }

    #[test]
    fn caps_before_and_after_start() {
        use crate::caps::{EOPNOTSUPP, get_cap};
        let dev = Features::GEM.union(Features::MODESET).union(Features::ATOMIC).union(Features::DUMB);
        let info = (dev, ModeConfig { cursor_width: 128, ..ModeConfig::DEFAULT });
        let started = (dev, ModeConfig { cursor_width: 256, async_page_flip: true, ..ModeConfig::DEFAULT });
        // Not a Linux-KMS driver: the device's own view.
        assert_eq!(caps_view(info, false, None), info);
        // Before drm_dev_illumos_lkms_register(): as a device without mode
        // setting (GEM and PRIME stay).
        let (f, m) = caps_view(info, true, None);
        assert!(!f.contains(Features::MODESET) && !f.contains(Features::ATOMIC) && f.contains(Features::GEM));
        assert_eq!(m, ModeConfig::DEFAULT);
        assert_eq!(get_cap(f, &m, u64::from(uapi::DRM_CAP_CURSOR_WIDTH)), Err(EOPNOTSUPP));
        assert_eq!(get_cap(f, &m, u64::from(uapi::DRM_CAP_PRIME)), Ok(3));
        let mut c = crate::caps::ClientCaps::default();
        assert_eq!(c.set(f, u64::from(uapi::DRM_CLIENT_CAP_ATOMIC), 1), Err(EOPNOTSUPP));
        // Afterwards: what it started with.
        let (f, m) = caps_view(info, true, Some(started));
        assert_eq!(get_cap(f, &m, u64::from(uapi::DRM_CAP_CURSOR_WIDTH)), Ok(256));
        assert_eq!(get_cap(f, &m, u64::from(uapi::DRM_CAP_ASYNC_PAGE_FLIP)), Ok(1));
        assert_eq!(c.set(f, u64::from(uapi::DRM_CLIENT_CAP_ATOMIC), 1), Ok(()));
    }

    #[test]
    fn negative_results() {
        assert_eq!(from_neg(0), Ok(()));
        assert_eq!(from_neg(1), Ok(()));
        assert_eq!(from_neg(-25), Err(Errno::ENOTTY));
        // The KMS code maps ERESTARTSYS (512) to EINTR (lkpi_errno());
        // one that got through is refused as EIO, as is every other
        // kernel-internal number up to MAX_ERRNO.
        assert_eq!(from_neg(-(Errno::EINTR.get())), Err(Errno::EINTR));
        assert_eq!(from_neg(-511).map_err(|e| e.get()), Err(511));
        for r in [-512, -513, -517, -524, -4095] {
            assert!(is_linux_internal(r));
            assert_eq!(from_neg(r), Err(Errno::EIO), "result {r}");
        }
        assert!(!is_linux_internal(-511) && !is_linux_internal(-4096) && !is_linux_internal(0));
        assert_eq!(from_neg(-4096), Err(Errno::EINVAL));
        assert_eq!(from_neg(i32::MIN), Err(Errno::EINVAL));
    }
}
