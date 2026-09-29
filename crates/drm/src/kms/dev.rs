// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The mode-setting layer in the kernel: locks, argument copies, GEM
//! lookups, events through `read(2)` and `poll(2)`, and vblanks.
//!
//! # Locks
//!
//! [`KmsCore`] is behind one mutex, the mode-setting lock: every
//! mode-setting ioctl that reads or changes objects holds it for its whole
//! run, including the driver's hooks and, for blocking commits, the wait
//! for the flip. The vblank counters and event queues ([`EventCore`]) are
//! behind a second mutex, the event lock, which is always taken after the
//! mode-setting lock or alone, and which the vblank path takes alone. GEM
//! lookups take the GEM locks under the mode-setting lock; GEM never takes
//! the mode-setting lock. No lock is held while copying from or to user
//! memory except the mode-setting lock (the copies of an ioctl's arrays).
//!
//! # Vblanks and events
//!
//! A driver reports each vblank with [`KmsEvents::vblank`] (through its [`VblankHandle`]), from thread
//! context, a task queue, a level-0 `ddi_periodic(9F)` callback or a
//! `timeout(9F)` callback; the event lock is an adaptive mutex without an
//! interrupt priority, so hardware interrupt handlers must pass vblanks on
//! to a soft interrupt that is registered with a matching lock priority
//! (not provided yet; R4's virtio-gpu uses a software vblank like
//! `rdrmnull`'s, [`super::soft::SoftVblank`]). Waking pollers with
//! `pollwakeup(9F)` happens after the event lock is released, on a
//! [`PollHeads`] array that lives as long as the device, so a wakeup that
//! races with a close finds a valid head.
//!
//! Flip completion: a commit arms a flip on every active CRTC it touches
//! after the driver's hooks have run; the next vblank completes it and
//! sends its events. A flip is therefore never reported before the
//! driver saw the new state, but may be reported one frame late if a vblank
//! fell between the hooks and the arming. A non-blocking commit on a CRTC
//! whose flip is still pending fails with `EBUSY`; a blocking one waits
//! for it first, and waits for its own flip before returning (both waits
//! are bounded by one second).

use alloc::boxed::Box;
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::mem::size_of;
use core::sync::atomic::{AtomicU64, Ordering};
use core::time::Duration;

use ddi::prelude::*;
use ddi::sys;
use drm_uapi as uapi;

use super::atomic::release_state;
use super::core::{KmsCore, MAX_BLOB, release_old};
use super::events::{Event, EventCore, EventKind, EventList, WakeSet, widen_32_to_64};
use super::formats;
use super::ioctl::KmsOp;
use super::legacy::SetCrtc;
use super::modes::DisplayMode;
use super::objects::{FbRequest, ObjKind};
use super::props::{Prop, PropKind};
use super::state::{AtomicState, CommitFlags, FbBacking, KmsDriver};
use crate::caps::{ClientCaps, EOPNOTSUPP};
use crate::gem::{GemCore, GemObject, GemRef};
use crate::karc::KArc;
use crate::minor::{CloneId, MAX_FILES};
use crate::objects::ObjRef;
use crate::uaccess::{Plain, UserArg, copy_in_bytes, copy_out_bytes};

// SAFETY: integer fields only (see the bindings in drm-uapi); the union
// drm_wait_vblank has two integer-only members of the same size.
unsafe impl Plain for uapi::drm_mode_card_res {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_crtc {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_cursor {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_cursor2 {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_crtc_lut {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_get_encoder {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_get_connector {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_get_property {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_connector_set_property {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_get_blob {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_fb_cmd {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_fb_cmd2 {}
// SAFETY: as above.
unsafe impl Plain for u32 {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_crtc_page_flip {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_fb_dirty_cmd {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_get_plane_res {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_get_plane {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_set_plane {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_obj_get_properties {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_obj_set_property {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_atomic {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_create_blob {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_mode_destroy_blob {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_wait_vblank {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_crtc_get_sequence {}
// SAFETY: as above.
unsafe impl Plain for uapi::drm_crtc_queue_sequence {}

/// The most objects one `MODE_ATOMIC` call may name, and the most
/// properties in all. Linux has no limit; the arrays are copied into
/// kernel memory here, so they are bounded (a desktop commit sets well
/// under a hundred).
pub const MAX_ATOMIC_OBJS: usize = 1024;
/// See [`MAX_ATOMIC_OBJS`].
pub const MAX_ATOMIC_PROPS: usize = 16384;

/// How long a blocking commit waits for a pending flip, and a blocking
/// `WAIT_VBLANK` for its vblank (Linux: 3 s for the latter).
const FLIP_WAIT: Duration = Duration::from_secs(1);
const VBLANK_WAIT: Duration = Duration::from_secs(3);

impl<O: GemObject> FbBacking for GemRef<O> {
    fn size(&self) -> u64 {
        GemRef::size(self) as u64
    }
    fn release(self, ctx: &Sleepable) {
        self.put(ctx);
    }
}

ddi::named_kstat! {
    /// The `kms` kstat of a DRM device (`kstat -m <driver> -n kms`).
    pub struct KmsKstat {
        /// Framebuffers alive.
        pub framebuffers: u64,
        /// Property blobs alive (including the IN_FORMATS and EDID blobs).
        pub blobs: u64,
        /// Events queued or pending in all files.
        pub events_outstanding: u64,
        /// Events delivered so far.
        pub events_delivered: u64,
        /// Events freed because their file closed first.
        pub events_discarded: u64,
        /// Vblanks so far.
        pub vblanks: u64,
        /// Flips completed so far.
        pub flips: u64,
        /// Commits so far (atomic and legacy, test-only excluded).
        pub commits: u64,
    }
}

/// The vblank and event side of a device, shared with the driver's vblank
/// source through [`VblankHandle`].
pub struct KmsEvents {
    lock: Mutex<EventCore>,
    cv: CondVar,
    polls: PollHeads,
}

/// What a driver keeps to report vblanks: a counted reference to the
/// device's event state, valid as long as the driver holds it.
pub type VblankHandle = Arc<KmsEvents>;

fn now_ns() -> i64 {
    // SAFETY: gethrtime(9F) may be called from any context.
    unsafe { sys::gethrtime() }
}

impl KmsEvents {
    /// Reports a vblank of CRTC `crtc` (an index) that happened at
    /// `time_ns` (`gethrtime(9F)`): the counter advances, an armed flip
    /// completes, due waits are delivered, pollers and waiters wake. See
    /// the module documentation for the contexts it may be called from.
    pub fn vblank(&self, crtc: usize, time_ns: i64) {
        let w = self.lock.lock().handle_vblank(crtc, time_ns);
        self.wake(w);
    }

    /// The current time as vblank timestamps use it.
    pub fn now(&self) -> i64 {
        now_ns()
    }

    fn wake(&self, w: WakeSet) {
        self.cv.notify_all();
        for f in w.iter() {
            if let Some(h) = self.polls.get(f as usize) {
                h.wakeup(PollEvents::IN.union(PollEvents::RDNORM));
            }
        }
    }
}

/// The client a mode-setting ioctl runs for.
pub struct Caller<'a> {
    /// Its file.
    pub file: CloneId,
    /// Its client capabilities.
    pub caps: ClientCaps,
    /// Whether it is the current master.
    pub master: bool,
    /// Whether its credentials hold `PRIV_SYS_DEVICES` (asked lazily).
    pub privileged: &'a dyn Fn() -> bool,
}

/// The mode-setting state of a device in the kernel.
pub struct Kms<O: GemObject> {
    core: Mutex<KmsCore<GemRef<O>>>,
    ev: Arc<KmsEvents>,
    commits: AtomicU64,
    kstat: Option<TypedKstat<KmsKstat>>,
    atomic: bool,
    modifiers: bool,
}

type Core<O> = KmsCore<GemRef<O>>;
type Drv<'a, O> = &'a dyn KmsDriver<GemRef<O>>;

impl<O: GemObject> Kms<O> {
    /// Sets up the mode objects of a device from its driver and creates
    /// the `kms` kstat. `atomic` and `modifiers` say whether the driver has
    /// `Features::ATOMIC` and accepts `ADDFB2` modifiers.
    pub(crate) fn new(
        dev: DevInfo,
        drv: Drv<'_, O>,
        atomic: bool,
        modifiers: bool,
        ctx: &Sleepable,
    ) -> Result<Kms<O>, Errno> {
        let core = KmsCore::new(drv, ctx)?;
        let events = EventCore::new(core.crtc_ids(), ctx);
        let ev = Arc::new(KmsEvents {
            lock: Mutex::new(events),
            cv: CondVar::new(),
            polls: PollHeads::new(MAX_FILES as usize + 1, ctx),
        });
        let kstat = TypedKstat::<KmsKstat>::new(
            ddi::KstatId { module: dev.driver_name(), instance: dev.instance(), name: c"kms", class: c"misc" },
            ctx,
        );
        if kstat.is_err() {
            dev.warn(format_args!("cannot create the kms kstat; statistics are unavailable"));
        }
        let k = Kms { core: Mutex::new(core), ev, commits: AtomicU64::new(0), kstat: kstat.ok(), atomic, modifiers };
        k.publish();
        Ok(k)
    }

    /// The handle a driver keeps to report vblanks.
    pub fn vblank_handle(&self) -> VblankHandle {
        self.ev.clone()
    }

    fn publish(&self) {
        let Some(k) = &self.kstat else { return };
        let (fbs, blobs) = self.core.lock().counts();
        let s = self.ev.lock.lock().stats();
        let mut g = k.lock();
        g.framebuffers.set(fbs as u64);
        g.blobs.set(blobs as u64);
        g.events_outstanding.set(s.outstanding);
        g.events_delivered.set(s.delivered);
        g.events_discarded.set(s.discarded);
        g.vblanks.set(s.vblanks);
        g.flips.set(s.flips);
        g.commits.set(self.commits.load(Ordering::Relaxed));
    }

    /// Registers an opened file.
    pub(crate) fn file_open(&self, c: CloneId, ctx: &Sleepable) {
        self.core.lock().file_open(c.get(), ctx);
        self.ev.lock.lock().open(c.get());
    }

    /// Forgets a closed file: its pending and queued events, its pollers,
    /// its framebuffers (turning off what shows them) and its blobs.
    pub(crate) fn file_close(&self, c: CloneId, drv: Drv<'_, O>, ctx: &Sleepable) {
        let gone = self.ev.lock.lock().close(c.get());
        drop(gone);
        if let Some(h) = self.ev.polls.get(c.index()) {
            h.clean();
        }
        let mut core = self.core.lock();
        if core.file_close(c.get(), drv, ctx).is_err() {
            // Turning off never fails; nothing to undo.
        }
        self.settle(&mut core);
        drop(core);
        self.publish();
    }

    /// Releases everything when the device detaches, after the driver has
    /// stopped its vblank source and dropped its framebuffer references.
    pub(crate) fn teardown(&self, ctx: &Sleepable) {
        let mut core = self.core.lock();
        core.teardown(ctx);
        drop(core);
        self.publish();
    }

    /// Applies the vblank transitions of the last commits.
    fn settle(&self, core: &mut Core<O>) {
        let t = core.take_transitions();
        if t.is_empty() {
            return;
        }
        let now = now_ns();
        let mut wake = WakeSet::default();
        {
            let mut ev = self.ev.lock.lock();
            for x in t {
                wake.merge(ev.set_enabled(x.crtc, x.on, x.frame_ns, now));
            }
        }
        self.ev.wake(wake);
    }

    // --- read(2) and poll(2) -------------------------------------------------

    /// `read(9E)`: whole events only; blocks for the first one unless the
    /// file is non-blocking (`EAGAIN`); a buffer too small for the next
    /// event reads 0 bytes (Linux's `drm_read`).
    pub(crate) fn read(&self, c: CloneId, uio: &mut Uio<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let nonblock = i32::from(uio.fmode()) & (sys::FNONBLOCK | sys::FNDELAY) != 0;
        let mut copied = 0usize;
        loop {
            let mut g = self.ev.lock.lock();
            match g.pop(c.get(), uio.resid()) {
                Some(e) => {
                    drop(g);
                    match uio.copy_out(e.bytes()) {
                        Ok(_) => copied += e.bytes().len(),
                        Err(_) => {
                            self.ev.lock.lock().put_back(e);
                            return if copied == 0 { Err(Errno::EFAULT) } else { Ok(()) };
                        }
                    }
                }
                None => {
                    if copied > 0 || g.readable(c.get()) {
                        return Ok(());
                    }
                    if nonblock {
                        return Err(Errno::EAGAIN);
                    }
                    let (g, interrupted) = self.ev.cv.wait_sig(g, ctx);
                    drop(g);
                    if interrupted {
                        return Err(Errno::EINTR);
                    }
                }
            }
        }
    }

    /// `chpoll(9E)`: readable while events are queued.
    pub(crate) fn chpoll(&self, c: CloneId) -> Result<(PollEvents, PollRegistration), Errno> {
        let ready = if self.ev.lock.lock().readable(c.get()) {
            PollEvents::IN.union(PollEvents::RDNORM)
        } else {
            PollEvents::NONE
        };
        let h = self.ev.polls.get(c.index()).ok_or(Errno::ENXIO)?;
        Ok((ready, h.registration()))
    }

    // --- the commit path -----------------------------------------------------

    /// Checks and commits an update: see the module documentation. `event`
    /// is the user data of the flip events the caller asked for.
    #[allow(clippy::too_many_arguments, reason = "the parts of a commit")]
    fn run_commit(
        &self,
        core: &mut Core<O>,
        mut st: AtomicState<GemRef<O>>,
        file: CloneId,
        event: Option<u64>,
        test_only: bool,
        drv: Drv<'_, O>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let event_mask = if event.is_some() { st.crtc_mask() } else { 0 };
        if let Err(e) = core.check(&mut st, event_mask, drv) {
            release_state(st, ctx);
            core.sweep(ctx);
            return Err(e);
        }
        if test_only {
            release_state(st, ctx);
            core.sweep(ctx);
            return Ok(());
        }
        let affected = st.crtc_mask();
        let flags = st.flags;
        if !flags.cursor
            && let Err(e) = self.stall(affected, flags.nonblock, ctx)
        {
            release_state(st, ctx);
            core.sweep(ctx);
            return Err(e);
        }
        // Allocate and charge the events before the commit, so that a full
        // budget fails the ioctl without changing anything.
        let mut events: Vec<(usize, Box<Event>)> = Vec::new();
        if let Some(user_data) = event {
            for (i, id) in core.crtc_ids().iter().enumerate() {
                if event_mask & (1 << i) != 0 {
                    events.push((i, Event::new(file.get(), EventKind::Flip { user_data }, *id, ctx)));
                }
            }
            if let Err(e) = self.ev.lock.lock().charge(file.get(), events.len()) {
                drop(events);
                release_state(st, ctx);
                core.sweep(ctx);
                return Err(e);
            }
        }
        let done = core.commit(st, drv, ctx);
        let transitions = core.take_transitions();
        let now = now_ns();
        let mut wake = WakeSet::default();
        let mut armed = 0u32;
        {
            let mut ev = self.ev.lock.lock();
            for t in transitions {
                wake.merge(ev.set_enabled(t.crtc, t.on, t.frame_ns, now));
            }
            for i in 0..core.crtc_ids().len() {
                if affected & (1 << i) == 0 {
                    continue;
                }
                let mut list = EventList::new();
                let mut rest = Vec::new();
                for (c, e) in events.drain(..) {
                    if c == i { list.push_back(e) } else { rest.push((c, e)) }
                }
                events = rest;
                if done.flip_mask & (1 << i) != 0 && !flags.cursor {
                    ev.arm_flip(i, list);
                    armed |= 1 << i;
                } else {
                    wake.merge(ev.send_now(i, list));
                }
            }
        }
        self.ev.wake(wake);
        release_old(done.old, ctx);
        core.sweep(ctx);
        self.commits.fetch_add(1, Ordering::Relaxed);
        if !flags.nonblock && armed != 0 {
            // A timeout here only means the flip is late; the commit stands.
            let _ = self.wait_flips(armed, ctx);
        }
        Ok(())
    }

    /// Waits until no flip is pending on `mask`, or refuses with `EBUSY`
    /// for a non-blocking caller.
    fn stall(&self, mask: u32, nonblock: bool, ctx: &Sleepable) -> Result<(), Errno> {
        if !self.ev.lock.lock().flip_pending(mask) {
            return Ok(());
        }
        if nonblock {
            return Err(Errno::EBUSY);
        }
        self.wait_flips(mask, ctx)
    }

    fn wait_flips(&self, mask: u32, ctx: &Sleepable) -> Result<(), Errno> {
        let start = HrTime::now();
        let mut g = self.ev.lock.lock();
        while g.flip_pending(mask) {
            let left = FLIP_WAIT.saturating_sub(start.elapsed());
            if left.is_zero() {
                return Err(Errno::EBUSY);
            }
            let (g2, r) = self.ev.cv.wait_timeout_sig(g, left, ctx);
            g = g2;
            if r.interrupted() {
                return Err(Errno::EINTR);
            }
        }
        Ok(())
    }

    // --- ioctls --------------------------------------------------------------

    /// Runs a mode-setting ioctl whose permission the device has checked.
    pub(crate) fn ioctl(
        &self,
        op: KmsOp,
        who: &Caller<'_>,
        arg: &IoctlArg<'_>,
        gem: Option<&KArc<GemCore<O>>>,
        drv: Drv<'_, O>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let r = self.dispatch(op, who, arg, gem, drv, ctx);
        self.publish();
        r
    }

    fn dispatch(
        &self,
        op: KmsOp,
        who: &Caller<'_>,
        arg: &IoctlArg<'_>,
        gem: Option<&KArc<GemCore<O>>>,
        drv: Drv<'_, O>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let file = who.file;
        match op {
            // Linux keeps these as no-ops for old userland.
            KmsOp::ModesetCtl | KmsOp::AttachMode | KmsOp::DetachMode => Ok(()),
            KmsOp::WaitVblank => self.wait_vblank(file, arg, ctx),
            KmsOp::CrtcGetSequence => {
                let mut a = UserArg::<uapi::drm_crtc_get_sequence>::read(arg)?;
                let (i, enable) = {
                    let core = self.core.lock();
                    let i = core.crtc_ids().iter().position(|&c| c == a.get().crtc_id).ok_or(Errno::ENOENT)?;
                    (i, core.cur.crtcs.get(i).is_some_and(|s| s.enable()))
                };
                let ev = self.ev.lock.lock();
                if !ev.enabled(i) {
                    return Err(Errno::EINVAL);
                }
                let (seq, t) = ev.count(i);
                drop(ev);
                // SAFETY: only field assignments below.
                let m = unsafe { a.get_mut() };
                m.active = u32::from(enable);
                m.sequence = seq;
                m.sequence_ns = t;
                a.write(arg)
            }
            KmsOp::CrtcQueueSequence => {
                let mut a = UserArg::<uapi::drm_crtc_queue_sequence>::read(arg)?;
                let v = *a.get();
                if v.flags & !(uapi::DRM_CRTC_SEQUENCE_RELATIVE | uapi::DRM_CRTC_SEQUENCE_NEXT_ON_MISS) != 0 {
                    return Err(Errno::EINVAL);
                }
                let i = self.core.lock().crtc_ids().iter().position(|&c| c == v.crtc_id).ok_or(Errno::ENOENT)?;
                let e = Event::new(file.get(), EventKind::Sequence { user_data: v.user_data }, v.crtc_id, ctx);
                let mut ev = self.ev.lock.lock();
                if !ev.enabled(i) {
                    return Err(Errno::EINVAL);
                }
                let (seq, _) = ev.count(i);
                let mut req = if v.flags & uapi::DRM_CRTC_SEQUENCE_RELATIVE != 0 {
                    seq.wrapping_add(v.sequence)
                } else {
                    v.sequence
                };
                if v.flags & uapi::DRM_CRTC_SEQUENCE_NEXT_ON_MISS != 0 && seq >= req {
                    req = seq + 1;
                }
                ev.charge(file.get(), 1)?;
                let w = match ev.queue_wait(i, req, e) {
                    Ok(w) => w,
                    Err((_e, err)) => {
                        ev.uncharge(file.get(), 1);
                        return Err(err);
                    }
                };
                drop(ev);
                self.ev.wake(w);
                // SAFETY: a field assignment.
                unsafe { a.get_mut() }.sequence = req;
                a.write(arg)
            }
            KmsOp::GetResources => {
                let mut a = UserArg::<uapi::drm_mode_card_res>::read(arg)?;
                let r = self.core.lock().resources(file.get());
                let v = *a.get();
                copy_ids(arg, v.fb_id_ptr, &r.fbs, v.count_fbs)?;
                copy_ids(arg, v.crtc_id_ptr, &r.crtcs, v.count_crtcs)?;
                copy_ids(arg, v.connector_id_ptr, &r.connectors, v.count_connectors)?;
                copy_ids(arg, v.encoder_id_ptr, &r.encoders, v.count_encoders)?;
                // SAFETY: only field assignments below.
                let m = unsafe { a.get_mut() };
                m.count_fbs = r.fbs.len() as u32;
                m.count_crtcs = r.crtcs.len() as u32;
                m.count_connectors = r.connectors.len() as u32;
                m.count_encoders = r.encoders.len() as u32;
                [m.min_width, m.max_width, m.min_height, m.max_height] = r.limits;
                a.write(arg)
            }
            KmsOp::GetPlaneResources => {
                let mut a = UserArg::<uapi::drm_mode_get_plane_res>::read(arg)?;
                let planes = self.core.lock().plane_resources(who.caps.universal_planes);
                let v = *a.get();
                copy_ids(arg, v.plane_id_ptr, &planes, v.count_planes)?;
                // SAFETY: a field assignment.
                unsafe { a.get_mut() }.count_planes = planes.len() as u32;
                a.write(arg)
            }
            KmsOp::GetCrtc => {
                let mut a = UserArg::<uapi::drm_mode_crtc>::read(arg)?;
                let info = self.core.lock().get_crtc(a.get().crtc_id)?;
                // SAFETY: only field assignments below.
                let m = unsafe { a.get_mut() };
                m.fb_id = info.fb_id;
                m.x = info.x;
                m.y = info.y;
                m.gamma_size = info.gamma_size;
                match info.mode {
                    Some(md) => {
                        m.mode = md.to_umode(who.caps.aspect_ratio);
                        m.mode_valid = 1;
                    }
                    None => {
                        m.mode = uapi::drm_mode_modeinfo::default();
                        m.mode_valid = 0;
                    }
                }
                a.write(arg)
            }
            KmsOp::SetCrtc => self.setcrtc(who, arg, drv, ctx),
            KmsOp::GetEncoder => {
                let mut a = UserArg::<uapi::drm_mode_get_encoder>::read(arg)?;
                let e = self.core.lock().get_encoder(a.get().encoder_id)?;
                // SAFETY: only field assignments below.
                let m = unsafe { a.get_mut() };
                m.encoder_type = e.typ;
                m.crtc_id = e.crtc_id;
                m.possible_crtcs = e.possible_crtcs;
                m.possible_clones = e.possible_clones;
                a.write(arg)
            }
            KmsOp::GetConnector => {
                let mut a = UserArg::<uapi::drm_mode_get_connector>::read(arg)?;
                let v = *a.get();
                let probe = v.count_modes == 0 && who.master;
                let c = self.core.lock().get_connector(
                    v.connector_id,
                    probe,
                    who.caps.aspect_ratio,
                    who.caps.atomic,
                    drv,
                    ctx,
                )?;
                if !c.encoders.is_empty() && v.count_encoders as usize >= c.encoders.len() {
                    copy_ids(arg, v.encoders_ptr, &c.encoders, v.count_encoders)?;
                }
                if !c.modes.is_empty() && v.count_modes as usize >= c.modes.len() {
                    copy_modes(arg, v.modes_ptr, &c.modes)?;
                }
                copy_props(arg, v.props_ptr, v.prop_values_ptr, &c.props, v.count_props)?;
                // SAFETY: only field assignments below.
                let m = unsafe { a.get_mut() };
                m.count_encoders = c.encoders.len() as u32;
                m.count_modes = c.modes.len() as u32;
                m.count_props = c.props.len() as u32;
                m.encoder_id = c.encoder_id;
                m.connector_type = c.typ;
                m.connector_type_id = c.type_id;
                m.connection = c.status;
                m.mm_width = c.width_mm;
                m.mm_height = c.height_mm;
                // DRM_MODE_SUBPIXEL_UNKNOWN.
                m.subpixel = 1;
                a.write(arg)
            }
            KmsOp::GetProperty => {
                let mut a = UserArg::<uapi::drm_mode_get_property>::read(arg)?;
                let def = self.core.lock().get_property(a.get().prop_id)?;
                let v = *a.get();
                let values: Vec<u64> = def.values().collect();
                let n = values.len().min(v.count_values as usize);
                copy_out_bytes(arg, v.values_ptr as usize, as_bytes(values.get(..n).unwrap_or(&[])))?;
                let enums = def.enums();
                if matches!(def.kind, PropKind::Enum(_) | PropKind::Bitmask(_)) {
                    let n = enums.len().min(v.count_enum_blobs as usize);
                    let mut buf = Vec::with_capacity(n * size_of::<uapi::drm_mode_property_enum>());
                    for (val, name) in enums.iter().take(n) {
                        buf.extend_from_slice(&val.to_ne_bytes());
                        let mut nm = [0u8; uapi::DRM_PROP_NAME_LEN as usize];
                        let b = name.as_bytes();
                        let l = b.len().min(nm.len() - 1);
                        nm.get_mut(..l).unwrap_or(&mut []).copy_from_slice(b.get(..l).unwrap_or(&[]));
                        buf.extend_from_slice(&nm);
                    }
                    copy_out_bytes(arg, v.enum_blob_ptr as usize, &buf)?;
                }
                // SAFETY: only field assignments below.
                let m = unsafe { a.get_mut() };
                let mut nm = [0 as core::ffi::c_char; uapi::DRM_PROP_NAME_LEN as usize];
                for (d, s) in nm.iter_mut().zip(def.name.bytes().take(uapi::DRM_PROP_NAME_LEN as usize - 1)) {
                    *d = s as core::ffi::c_char;
                }
                m.name = nm;
                m.flags = def.flags();
                m.count_values = values.len() as u32;
                match def.kind {
                    PropKind::Enum(l) | PropKind::Bitmask(l) => m.count_enum_blobs = l.len() as u32,
                    PropKind::Blob => m.count_enum_blobs = 0,
                    _ => {}
                }
                a.write(arg)
            }
            KmsOp::GetPropBlob => {
                let mut a = UserArg::<uapi::drm_mode_get_blob>::read(arg)?;
                let blob = self.core.lock().blob(a.get().blob_id).cloned().ok_or(Errno::ENOENT)?;
                let v = *a.get();
                if v.length as usize == blob.data.len() {
                    copy_out_bytes(arg, v.data as usize, &blob.data)?;
                }
                // SAFETY: a field assignment.
                unsafe { a.get_mut() }.length = blob.data.len() as u32;
                a.write(arg)
            }
            KmsOp::CreatePropBlob => {
                let mut a = UserArg::<uapi::drm_mode_create_blob>::read(arg)?;
                let v = *a.get();
                let len = v.length as usize;
                if len == 0 {
                    return Err(Errno::EINVAL);
                }
                if len > MAX_BLOB {
                    return Err(Errno::ENOMEM);
                }
                let mut data = alloc::vec![0u8; len];
                copy_in_bytes(arg, v.data as usize, &mut data)?;
                let id = self.core.lock().create_blob(file.get(), data, ctx)?;
                // SAFETY: a field assignment.
                unsafe { a.get_mut() }.blob_id = id;
                a.write(arg)
            }
            KmsOp::DestroyPropBlob => {
                let a = UserArg::<uapi::drm_mode_destroy_blob>::read(arg)?;
                self.core.lock().destroy_blob(file.get(), a.get().blob_id, ctx)
            }
            KmsOp::GetFb => {
                let mut a = UserArg::<uapi::drm_mode_fb_cmd>::read(arg)?;
                let fb = self.core.lock().fb(a.get().fb_id).cloned().ok_or(Errno::ENOENT)?;
                let info = fb.info().ok_or(Errno::EINVAL)?;
                if info.planes() > 1 {
                    return Err(Errno::EINVAL);
                }
                let handle = match (&fb.objs[0], gem) {
                    (Some(o), Some(g)) if who.master || (who.privileged)() => g.handle_create(file, o.clone(), ctx)?,
                    _ => 0,
                };
                // SAFETY: only field assignments below.
                let m = unsafe { a.get_mut() };
                m.width = fb.width;
                m.height = fb.height;
                m.pitch = fb.pitches[0];
                m.bpp = info.bpp();
                m.depth = u32::from(info.depth);
                m.handle = handle;
                drop(fb);
                self.core.lock().sweep(ctx);
                a.write(arg)
            }
            KmsOp::GetFb2 => {
                let mut a = UserArg::<uapi::drm_mode_fb_cmd2>::read(arg)?;
                let fb = self.core.lock().fb(a.get().fb_id).cloned().ok_or(Errno::ENOENT)?;
                let n = fb.info().map_or(1, |i| i.planes());
                let mut handles = [0u32; 4];
                if who.master || (who.privileged)() {
                    for i in 0..n {
                        let (Some(o), Some(g)) = (&fb.objs[i], gem) else { continue };
                        let prev = (0..i).find(|&j| fb.objs[j].as_ref().is_some_and(|p| p.same(o)));
                        handles[i] = match prev {
                            Some(j) => handles[j],
                            None => g.handle_create(file, o.clone(), ctx)?,
                        };
                    }
                }
                // SAFETY: only field assignments below.
                let m = unsafe { a.get_mut() };
                m.width = fb.width;
                m.height = fb.height;
                m.pixel_format = fb.format;
                m.flags = fb.flags;
                for (i, h) in handles.iter().enumerate() {
                    let used = i < n;
                    m.handles[i] = *h;
                    m.pitches[i] = if used { fb.pitches[i] } else { 0 };
                    m.offsets[i] = if used { fb.offsets[i] } else { 0 };
                    m.modifier[i] = if used && fb.flags & uapi::DRM_MODE_FB_MODIFIERS != 0 { fb.modifier } else { 0 };
                }
                drop(fb);
                self.core.lock().sweep(ctx);
                a.write(arg)
            }
            KmsOp::AddFb => {
                let mut a = UserArg::<uapi::drm_mode_fb_cmd>::read(arg)?;
                let v = *a.get();
                let format = formats::legacy_format(v.bpp, v.depth).ok_or(Errno::EINVAL)?;
                let r = FbRequest {
                    width: v.width,
                    height: v.height,
                    format,
                    flags: 0,
                    handles: [v.handle, 0, 0, 0],
                    pitches: [v.pitch, 0, 0, 0],
                    offsets: [0; 4],
                    modifiers: [0; 4],
                };
                let id = self.add_fb(file, &r, gem, drv, ctx)?;
                // SAFETY: a field assignment.
                unsafe { a.get_mut() }.fb_id = id;
                a.write(arg)
            }
            KmsOp::AddFb2 => {
                let mut a = UserArg::<uapi::drm_mode_fb_cmd2>::read(arg)?;
                let v = *a.get();
                if v.flags & uapi::DRM_MODE_FB_MODIFIERS != 0 && !self.modifiers {
                    return Err(Errno::EINVAL);
                }
                let r = FbRequest {
                    width: v.width,
                    height: v.height,
                    format: v.pixel_format,
                    flags: v.flags,
                    handles: v.handles,
                    pitches: v.pitches,
                    offsets: v.offsets,
                    modifiers: v.modifier,
                };
                let id = self.add_fb(file, &r, gem, drv, ctx)?;
                // SAFETY: a field assignment.
                unsafe { a.get_mut() }.fb_id = id;
                a.write(arg)
            }
            KmsOp::RmFb => {
                let a = UserArg::<u32>::read(arg)?;
                let mut core = self.core.lock();
                let r = core.rm_fb(file.get(), *a.get(), drv, ctx);
                self.settle(&mut core);
                r
            }
            KmsOp::PageFlip => {
                let a = UserArg::<uapi::drm_mode_crtc_page_flip>::read(arg)?;
                let v = *a.get();
                if v.flags & !uapi::DRM_MODE_PAGE_FLIP_FLAGS != 0
                    || v.flags & uapi::DRM_MODE_PAGE_FLIP_ASYNC != 0
                    || v.flags & uapi::DRM_MODE_PAGE_FLIP_TARGET != 0
                {
                    return Err(Errno::EINVAL);
                }
                let mut core = self.core.lock();
                let st = core.page_flip(v.crtc_id, v.fb_id)?;
                let event = (v.flags & uapi::DRM_MODE_PAGE_FLIP_EVENT != 0).then_some(v.user_data);
                self.run_commit(&mut core, st, file, event, false, drv, ctx)
            }
            KmsOp::DirtyFb => {
                let a = UserArg::<uapi::drm_mode_fb_dirty_cmd>::read(arg)?;
                let v = *a.get();
                self.core.lock().fb(v.fb_id).ok_or(Errno::ENOENT)?;
                if v.num_clips > uapi::DRM_MODE_FB_DIRTY_MAX_CLIPS
                    || v.flags & !uapi::DRM_MODE_FB_DIRTY_FLAGS != 0
                    || (v.flags & uapi::DRM_MODE_FB_DIRTY_ANNOTATE_COPY != 0 && v.num_clips % 2 != 0)
                    || (v.num_clips == 0) != (v.clips_ptr == 0)
                {
                    return Err(Errno::EINVAL);
                }
                // No driver of this core needs damage reports: scanout reads
                // the framebuffer itself. Linux answers ENOSYS then.
                Err(ENOSYS)
            }
            KmsOp::GetPlane => {
                let mut a = UserArg::<uapi::drm_mode_get_plane>::read(arg)?;
                let p = self.core.lock().get_plane(a.get().plane_id)?;
                let v = *a.get();
                if !p.formats.is_empty() && v.count_format_types as usize >= p.formats.len() {
                    copy_ids(arg, v.format_type_ptr, &p.formats, v.count_format_types)?;
                }
                // SAFETY: only field assignments below.
                let m = unsafe { a.get_mut() };
                m.crtc_id = p.crtc_id;
                m.fb_id = p.fb_id;
                m.possible_crtcs = p.possible_crtcs;
                m.gamma_size = 0;
                m.count_format_types = p.formats.len() as u32;
                a.write(arg)
            }
            KmsOp::SetPlane => {
                let a = UserArg::<uapi::drm_mode_set_plane>::read(arg)?;
                let v = *a.get();
                let mut core = self.core.lock();
                let st = core.setplane(
                    v.plane_id,
                    v.crtc_id,
                    v.fb_id,
                    (v.crtc_x, v.crtc_y, v.crtc_w, v.crtc_h),
                    (v.src_x, v.src_y, v.src_w, v.src_h),
                    drv,
                )?;
                self.run_commit(&mut core, st, file, None, false, drv, ctx)
            }
            KmsOp::ObjGetProperties => {
                let mut a = UserArg::<uapi::drm_mode_obj_get_properties>::read(arg)?;
                let v = *a.get();
                let props = self.core.lock().obj_props(v.obj_id, v.obj_type, who.caps.atomic)?;
                copy_props(arg, v.props_ptr, v.prop_values_ptr, &props, v.count_props)?;
                // SAFETY: a field assignment.
                unsafe { a.get_mut() }.count_props = props.len() as u32;
                a.write(arg)
            }
            KmsOp::ObjSetProperty => {
                let a = UserArg::<uapi::drm_mode_obj_set_property>::read(arg)?;
                let v = *a.get();
                self.set_property(who, v.obj_id, v.obj_type, v.prop_id, v.value, drv, ctx)
            }
            KmsOp::SetProperty => {
                let a = UserArg::<uapi::drm_mode_connector_set_property>::read(arg)?;
                let v = *a.get();
                self.set_property(who, v.connector_id, uapi::DRM_MODE_OBJECT_CONNECTOR, v.prop_id, v.value, drv, ctx)
            }
            KmsOp::Cursor => {
                let a = UserArg::<uapi::drm_mode_cursor>::read(arg)?;
                let v = *a.get();
                self.cursor(file, v.flags, v.crtc_id, (v.x, v.y), (v.width, v.height, v.handle), (0, 0), gem, drv, ctx)
            }
            KmsOp::Cursor2 => {
                let a = UserArg::<uapi::drm_mode_cursor2>::read(arg)?;
                let v = *a.get();
                let bo = (v.width, v.height, v.handle);
                self.cursor(file, v.flags, v.crtc_id, (v.x, v.y), bo, (v.hot_x, v.hot_y), gem, drv, ctx)
            }
            KmsOp::Atomic => self.atomic(who, arg, drv, ctx),
            KmsOp::GetGamma => {
                let a = UserArg::<uapi::drm_mode_crtc_lut>::read(arg)?;
                let v = *a.get();
                let (r, g, b) = {
                    let core = self.core.lock();
                    let (r, g, b) = core.get_gamma(v.crtc_id, v.gamma_size)?;
                    (r.to_vec(), g.to_vec(), b.to_vec())
                };
                copy_out_bytes(arg, v.red as usize, as_bytes(&r))?;
                copy_out_bytes(arg, v.green as usize, as_bytes(&g))?;
                copy_out_bytes(arg, v.blue as usize, as_bytes(&b))
            }
            KmsOp::SetGamma => {
                let a = UserArg::<uapi::drm_mode_crtc_lut>::read(arg)?;
                let v = *a.get();
                // Validate the size first, so that it bounds the copies.
                self.core.lock().get_gamma(v.crtc_id, v.gamma_size)?;
                let n = v.gamma_size as usize;
                let mut lut = alloc::vec![0u16; 3 * n];
                for (i, p) in [v.red, v.green, v.blue].into_iter().enumerate() {
                    let dst = lut.get_mut(i * n..(i + 1) * n).ok_or(Errno::EINVAL)?;
                    copy_in_bytes(arg, p as usize, as_bytes_mut(dst))?;
                }
                self.core.lock().set_gamma(v.crtc_id, v.gamma_size, &lut, drv)
            }
        }
    }

    fn add_fb(
        &self,
        file: CloneId,
        r: &FbRequest,
        gem: Option<&KArc<GemCore<O>>>,
        drv: Drv<'_, O>,
        ctx: &Sleepable,
    ) -> Result<u32, Errno> {
        let mut core = self.core.lock();
        let n = core.check_fb(r, drv)?;
        let g = gem.ok_or(Errno::ENOENT)?;
        let mut objs: [Option<GemRef<O>>; 4] = [None, None, None, None];
        for i in 0..n {
            match g.lookup(file, r.handles[i]) {
                Ok(o) => objs[i] = Some(o),
                Err(e) => {
                    objs.into_iter().flatten().for_each(|o| o.put(ctx));
                    return Err(e);
                }
            }
        }
        Ok(core.add_fb(Some(file.get()), r, objs, (0, 0), ctx)?.id)
    }

    fn setcrtc(&self, who: &Caller<'_>, arg: &IoctlArg<'_>, drv: Drv<'_, O>, ctx: &Sleepable) -> Result<(), Errno> {
        let a = UserArg::<uapi::drm_mode_crtc>::read(arg)?;
        let v = *a.get();
        if v.x & 0xffff_0000 != 0 || v.y & 0xffff_0000 != 0 {
            return Err(Errno::ERANGE);
        }
        let mode = if v.mode_valid != 0 {
            if !who.caps.aspect_ratio && v.mode.flags & uapi::DRM_MODE_FLAG_PIC_AR_MASK != 0 {
                return Err(Errno::EINVAL);
            }
            Some(DisplayMode::from_umode(&v.mode)?)
        } else {
            None
        };
        let mut core = self.core.lock();
        let n = v.count_connectors as usize;
        if n > core.layout().connectors.len() {
            return Err(Errno::EINVAL);
        }
        let mut conns = alloc::vec![0u32; n];
        copy_in_bytes(arg, v.set_connectors_ptr as usize, as_bytes_mut(&mut conns))?;
        let req = SetCrtc { crtc_id: v.crtc_id, fb_id: v.fb_id, x: v.x, y: v.y, mode, connectors: &conns };
        let st = match core.setcrtc(&req, drv, ctx) {
            Ok(st) => st,
            Err(e) => {
                core.sweep(ctx);
                return Err(e);
            }
        };
        self.run_commit(&mut core, st, who.file, None, false, drv, ctx)
    }

    #[allow(clippy::too_many_arguments, reason = "the fields of the property ioctls")]
    fn set_property(
        &self,
        who: &Caller<'_>,
        obj_id: u32,
        obj_type: u32,
        prop_id: u32,
        value: u64,
        drv: Drv<'_, O>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        let mut core = self.core.lock();
        let kind = core.find(obj_id).ok_or(Errno::ENOENT)?;
        if obj_type != 0 && kind.code() != obj_type {
            return Err(Errno::ENOENT);
        }
        super::core::props_of(kind).ok_or(Errno::EINVAL)?;
        let st = if prop_id == core.prop_id(Prop::Dpms) {
            let ObjKind::Connector(c) = kind else { return Err(Errno::EINVAL) };
            let def = Prop::Dpms.def();
            if !def.value_ok(value) {
                return Err(Errno::EINVAL);
            }
            core.dpms(c, value)?
        } else {
            let mut st = core.begin(CommitFlags { allow_modeset: true, ..CommitFlags::default() });
            if let Err(e) = core.set_prop(&mut st, kind, prop_id, value, Errno::EINVAL) {
                release_state(st, ctx);
                core.sweep(ctx);
                return Err(e);
            }
            st
        };
        self.run_commit(&mut core, st, who.file, None, false, drv, ctx)
    }

    #[allow(clippy::too_many_arguments, reason = "the fields of the cursor ioctls")]
    fn cursor(
        &self,
        file: CloneId,
        flags: u32,
        crtc_id: u32,
        pos: (i32, i32),
        bo: (u32, u32, u32),
        hot: (i32, i32),
        gem: Option<&KArc<GemCore<O>>>,
        drv: Drv<'_, O>,
        ctx: &Sleepable,
    ) -> Result<(), Errno> {
        if flags == 0 || flags & !uapi::DRM_MODE_CURSOR_FLAGS != 0 {
            return Err(Errno::EINVAL);
        }
        let mut core = self.core.lock();
        core.cursor_plane(crtc_id)?;
        let image = if flags & uapi::DRM_MODE_CURSOR_BO != 0 {
            let (w, h, handle) = bo;
            if handle == 0 {
                Some(None)
            } else {
                let r = FbRequest {
                    width: w,
                    height: h,
                    format: formats::ARGB8888,
                    flags: 0,
                    handles: [handle, 0, 0, 0],
                    pitches: [w.saturating_mul(4), 0, 0, 0],
                    offsets: [0; 4],
                    modifiers: [0; 4],
                };
                core.check_fb(&r, drv)?;
                let g = gem.ok_or(Errno::ENOENT)?;
                let o = g.lookup(file, handle)?;
                Some(Some(core.add_fb(None, &r, [Some(o), None, None, None], hot, ctx)?))
            }
        } else {
            None
        };
        let moving = flags & uapi::DRM_MODE_CURSOR_MOVE != 0;
        let st = match core.cursor(crtc_id, image, moving.then_some(pos)) {
            Ok(st) => st,
            Err(e) => {
                core.sweep(ctx);
                return Err(e);
            }
        };
        self.run_commit(&mut core, st, file, None, false, drv, ctx)?;
        if moving {
            core.set_cursor_pos(crtc_id, pos);
        }
        Ok(())
    }

    fn atomic(&self, who: &Caller<'_>, arg: &IoctlArg<'_>, drv: Drv<'_, O>, ctx: &Sleepable) -> Result<(), Errno> {
        let a = UserArg::<uapi::drm_mode_atomic>::read(arg)?;
        let v = *a.get();
        if !self.atomic {
            return Err(EOPNOTSUPP);
        }
        if !who.caps.atomic {
            return Err(Errno::EINVAL);
        }
        let f = v.flags;
        if f & !uapi::DRM_MODE_ATOMIC_FLAGS != 0 || v.reserved != 0 || f & uapi::DRM_MODE_PAGE_FLIP_ASYNC != 0 {
            return Err(Errno::EINVAL);
        }
        let test_only = f & uapi::DRM_MODE_ATOMIC_TEST_ONLY != 0;
        let want_event = f & uapi::DRM_MODE_PAGE_FLIP_EVENT != 0;
        if test_only && want_event {
            return Err(Errno::EINVAL);
        }
        let nobj = v.count_objs as usize;
        if nobj > MAX_ATOMIC_OBJS {
            return Err(Errno::EINVAL);
        }
        let mut objs = alloc::vec![0u32; nobj];
        let mut counts = alloc::vec![0u32; nobj];
        copy_in_bytes(arg, v.objs_ptr as usize, as_bytes_mut(&mut objs))?;
        copy_in_bytes(arg, v.count_props_ptr as usize, as_bytes_mut(&mut counts))?;
        let total = counts.iter().try_fold(0usize, |s, c| s.checked_add(*c as usize)).ok_or(Errno::EINVAL)?;
        if total > MAX_ATOMIC_PROPS {
            return Err(Errno::EINVAL);
        }
        let mut props = alloc::vec![0u32; total];
        let mut values = alloc::vec![0u64; total];
        copy_in_bytes(arg, v.props_ptr as usize, as_bytes_mut(&mut props))?;
        copy_in_bytes(arg, v.prop_values_ptr as usize, as_bytes_mut(&mut values))?;
        let mut core = self.core.lock();
        let flags = CommitFlags {
            allow_modeset: f & uapi::DRM_MODE_ATOMIC_ALLOW_MODESET != 0,
            nonblock: f & uapi::DRM_MODE_ATOMIC_NONBLOCK != 0,
            cursor: false,
        };
        let mut st = core.begin(flags);
        let mut k = 0usize;
        for (obj, count) in objs.iter().zip(counts.iter()) {
            let r = core.atomic_object(*obj).and_then(|kind| {
                for _ in 0..*count {
                    let (p, val) = (props.get(k).copied().unwrap_or(0), values.get(k).copied().unwrap_or(0));
                    k += 1;
                    core.set_prop(&mut st, kind, p, val, Errno::ENOENT)?;
                }
                Ok(())
            });
            if let Err(e) = r {
                release_state(st, ctx);
                core.sweep(ctx);
                return Err(e);
            }
        }
        self.run_commit(&mut core, st, who.file, want_event.then_some(v.user_data), test_only, drv, ctx)
    }

    fn wait_vblank(&self, file: CloneId, arg: &IoctlArg<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        const TYPES: u32 = uapi::_DRM_VBLANK_ABSOLUTE | uapi::_DRM_VBLANK_RELATIVE;
        const FLAGS: u32 = uapi::_DRM_VBLANK_EVENT
            | uapi::_DRM_VBLANK_SIGNAL
            | uapi::_DRM_VBLANK_SECONDARY
            | uapi::_DRM_VBLANK_NEXTONMISS;
        let mut a = UserArg::<uapi::drm_wait_vblank>::read(arg)?;
        // SAFETY: both members of the union are plain integers; the request
        // is what userland wrote.
        let mut req = unsafe { a.get().request };
        if req.type_ & uapi::_DRM_VBLANK_SIGNAL != 0
            || req.type_ & !(TYPES | FLAGS | uapi::_DRM_VBLANK_HIGH_CRTC_MASK) != 0
        {
            return Err(Errno::EINVAL);
        }
        let flags = req.type_ & FLAGS;
        let high = req.type_ & uapi::_DRM_VBLANK_HIGH_CRTC_MASK;
        let pipe = if high != 0 {
            (high >> 1) as usize
        } else if flags & uapi::_DRM_VBLANK_SECONDARY != 0 {
            1
        } else {
            0
        };
        let crtc_id = self.core.lock().crtc_ids().get(pipe).copied().ok_or(Errno::EINVAL)?;
        let event = (flags & uapi::_DRM_VBLANK_EVENT != 0)
            .then(|| Event::new(file.get(), EventKind::Vblank { user_data: req.signal }, crtc_id, ctx));
        let mut ev = self.ev.lock.lock();
        if !ev.enabled(pipe) {
            return Err(Errno::EINVAL);
        }
        let (seq, _) = ev.count(pipe);
        let mut target = if req.type_ & uapi::_DRM_VBLANK_RELATIVE != 0 {
            let t = seq.wrapping_add(u64::from(req.sequence));
            req.sequence = t as u32;
            req.type_ &= !uapi::_DRM_VBLANK_RELATIVE;
            t
        } else {
            widen_32_to_64(req.sequence, seq)
        };
        if flags & uapi::_DRM_VBLANK_NEXTONMISS != 0 && seq >= target {
            target = seq + 1;
            req.type_ &= !uapi::_DRM_VBLANK_NEXTONMISS;
            req.sequence = target as u32;
        }
        if let Some(e) = event {
            ev.charge(file.get(), 1)?;
            let passed = seq >= target;
            let w = match ev.queue_wait(pipe, target, e) {
                Ok(w) => w,
                Err((_e, err)) => {
                    ev.uncharge(file.get(), 1);
                    return Err(err);
                }
            };
            drop(ev);
            self.ev.wake(w);
            // The reply overlays the request; like Linux, only type and
            // sequence change for an event request.
            // SAFETY: both members are plain integers.
            let mut reply = unsafe { a.get().reply };
            reply.type_ = req.type_;
            reply.sequence = if passed { seq as u32 } else { target as u32 };
            // SAFETY: a whole-member assignment of a union of plain
            // integers of the same size: every byte is written.
            unsafe { a.get_mut() }.reply = reply;
            return a.write(arg);
        }
        let mut result = Ok(());
        if target != seq {
            let start = HrTime::now();
            while ev.enabled(pipe) && ev.count(pipe).0 < target {
                let left = VBLANK_WAIT.saturating_sub(start.elapsed());
                if left.is_zero() {
                    result = Err(Errno::EBUSY);
                    break;
                }
                let (g, r) = self.ev.cv.wait_timeout_sig(ev, left, ctx);
                ev = g;
                if r.interrupted() {
                    return Err(Errno::EINTR);
                }
            }
        }
        let (count, t) = ev.count(pipe);
        drop(ev);
        // SAFETY: both members are plain integers.
        let mut reply = unsafe { a.get().reply };
        reply.type_ = req.type_;
        reply.sequence = count as u32;
        let t = t.max(0) as u64;
        reply.tval_sec = (t / 1_000_000_000) as core::ffi::c_long;
        reply.tval_usec = ((t % 1_000_000_000) / 1000) as core::ffi::c_long;
        // SAFETY: as above.
        unsafe { a.get_mut() }.reply = reply;
        a.write(arg)?;
        result
    }
}

/// `ENOSYS`, which `ddi::Errno` has no constant for.
const ENOSYS: Errno = match Errno::new(sys::errno::ENOSYS) {
    Some(e) => e,
    None => Errno::EINVAL,
};

fn as_bytes<T: Copy>(v: &[T]) -> &[u8] {
    // SAFETY: plain integers without padding; the byte view covers exactly
    // the slice.
    unsafe { core::slice::from_raw_parts(v.as_ptr().cast(), core::mem::size_of_val(v)) }
}

fn as_bytes_mut<T: Copy>(v: &mut [T]) -> &mut [u8] {
    // SAFETY: as above; every byte pattern is a valid integer.
    unsafe { core::slice::from_raw_parts_mut(v.as_mut_ptr().cast(), core::mem::size_of_val(v)) }
}

/// Copies at most `room` IDs to a user array (the partial copy of Linux's
/// `GETRESOURCES` and friends).
fn copy_ids(arg: &IoctlArg<'_>, ptr: u64, ids: &[u32], room: u32) -> Result<(), Errno> {
    let n = ids.len().min(room as usize);
    copy_out_bytes(arg, ptr as usize, as_bytes(ids.get(..n).unwrap_or(&[])))
}

fn copy_modes(arg: &IoctlArg<'_>, ptr: u64, modes: &[uapi::drm_mode_modeinfo]) -> Result<(), Errno> {
    // drm_mode_modeinfo has no padding (asserted in objects.rs).
    let bytes =
        // SAFETY: the structures consist of integers without padding.
        unsafe { core::slice::from_raw_parts(modes.as_ptr().cast::<u8>(), core::mem::size_of_val(modes)) };
    copy_out_bytes(arg, ptr as usize, bytes)
}

/// Copies at most `room` property IDs and values.
fn copy_props(arg: &IoctlArg<'_>, ids_ptr: u64, vals_ptr: u64, props: &[(u32, u64)], room: u32) -> Result<(), Errno> {
    let n = props.len().min(room as usize);
    let ids: Vec<u32> = props.iter().take(n).map(|p| p.0).collect();
    let vals: Vec<u64> = props.iter().take(n).map(|p| p.1).collect();
    copy_out_bytes(arg, ids_ptr as usize, as_bytes(&ids))?;
    copy_out_bytes(arg, vals_ptr as usize, as_bytes(&vals))
}
