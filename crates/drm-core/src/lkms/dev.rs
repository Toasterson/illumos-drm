// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The event queues of a Linux-KMS driver's device in the kernel: the lock
//! around [`LkmsEvents`], `read(2)`, `chpoll(9E)` and the wakeups, and
//! whether the device's mode setting runs.
//!
//! # Contexts and locks
//!
//! The KMS code calls the event functions of `drm_lkms.h` with its own
//! `dev->event_lock` held (a LinuxKPI spinlock: an adaptive `kmutex_t`),
//! from thread context and from the driver's interrupt handler, a
//! low-level one (below `LOCK_LEVEL`, an interrupt thread that may take
//! adaptive mutexes but must not sleep otherwise). So the event lock here
//! is an adaptive mutex too, created with the device's fence priority when
//! the driver gave one (`drm_dev_illumos_set_fence_priority`, the priority
//! of its interrupt handler), and the paths that take it from the event
//! functions neither allocate nor block nor call back into the KMS code.
//! The LinuxKPI layer gives none: it registers the device in
//! `devm_drm_dev_alloc()`, before the driver allocates its interrupts, and
//! the priority can only be set before registration. That is sound for the
//! low-level handlers the layer accepts (`request_irq()` refuses a
//! high-level vector): an adaptive mutex created without an interrupt
//! cookie may be taken by an interrupt thread below `LOCK_LEVEL`
//! (`mutex_init(9F)`); the cookie matters only for a spin mutex.
//! No path holds it while calling into C code, so the order is always the
//! KMS code's lock, then this one (a reader's `read(2)` holds neither
//! while it copies out). Pollers and readers are woken after it is
//! released: `pollwakeup(9F)` must not be called with a lock that
//! `chpoll(9E)` takes.
//!
//! The queues are allocated when a file opens (8 KiB: the ring and its
//! record), in thread context, and freed when it closes; the poll heads
//! live as long as the device, so a wakeup that races with a close finds
//! a valid head.

use alloc::vec::Vec;

use ddi::prelude::*;
use ddi::sys;

use super::events::{Delivery, FileQueue, LkmsEventStats, LkmsEvents, ReadStep};
use crate::driver::{Features, ModeConfig};
use crate::minor::{CloneId, MAX_FILES};

/// The event state of a Linux-KMS driver's device, and whether its mode
/// setting runs.
pub struct LkmsQueues {
    lock: Mutex<LkmsEvents>,
    cv: CondVar,
    polls: PollHeads,
    /// The features and mode configuration mode setting started with
    /// (`drm_dev_illumos_lkms_register()`); `None` until then. Thread
    /// context only.
    started: Mutex<Option<(Features, ModeConfig)>>,
}

impl LkmsQueues {
    /// The queues of a device with no open files. `pri` is the priority of
    /// the highest interrupt handler that delivers events, if any.
    pub fn new(pri: Option<IntrPriority>, ctx: &Sleepable) -> Result<LkmsQueues, Errno> {
        let events = LkmsEvents::new(ctx);
        let lock = match pri {
            Some(p) => Mutex::with_priority(events, p),
            None => Mutex::new(events),
        };
        Ok(LkmsQueues {
            lock,
            cv: CondVar::new(),
            polls: PollHeads::new(MAX_FILES as usize + 1, ctx)?,
            started: Mutex::new(None),
        })
    }

    /// Records that mode setting runs, with the device's features and the
    /// KMS code's mode configuration at the start.
    pub fn start(&self, features: Features, mode: ModeConfig) {
        *self.started.lock() = Some((features, mode));
    }

    /// The features and mode configuration mode setting started with, if
    /// it did.
    pub fn started(&self) -> Option<(Features, ModeConfig)> {
        *self.started.lock()
    }

    /// Whether mode setting runs.
    pub fn active(&self) -> bool {
        self.started.lock().is_some()
    }

    /// Records that the KMS code's `file_open` accepted file `c`.
    pub fn set_kms_open(&self, c: CloneId) {
        self.lock.lock().set_kms_open(c.get());
    }

    /// Whether the KMS code's `file_open` accepted file `c`.
    pub fn kms_open(&self, c: CloneId) -> bool {
        self.lock.lock().kms_open(c.get())
    }

    /// The statistics.
    pub fn stats(&self) -> LkmsEventStats {
        self.lock.lock().stats()
    }

    /// Gives a file being opened its queue. Thread context.
    pub fn open(&self, c: CloneId, master: bool, ctx: &Sleepable) -> Result<(), Errno> {
        let q = FileQueue::new(master, ctx);
        let r = self.lock.lock().open(c.get(), q);
        // A refused queue (the clone ID still has one, which the core's
        // order of release rules out) is freed here, without the lock.
        r.map_err(|_| Errno::EBUSY)
    }

    /// The first step of a close: the file's queued events go, and the
    /// KMS code's later deliveries for it are dropped.
    pub fn begin_close(&self, c: CloneId) {
        self.lock.lock().begin_close(c.get());
    }

    /// The last step of a close, after the KMS code's `file_close`: the
    /// queue is freed and its pollers detached. Thread context.
    pub fn close(&self, c: CloneId) {
        let q = self.lock.lock().close(c.get());
        drop(q);
        if let Some(h) = self.polls.get(c.index()) {
            h.clean();
        }
    }

    /// Updates the master snapshot of a file (the device-wide bound
    /// exempts the master).
    pub fn set_master(&self, c: CloneId, master: bool) {
        self.lock.lock().set_master(c.get(), master);
    }

    /// `drm_illumos_event_reserve`. Thread or low-level interrupt context.
    pub fn reserve(&self, c: CloneId, size: usize) -> Result<(), Errno> {
        self.lock.lock().reserve(c.get(), size)
    }

    /// `drm_illumos_event_unreserve`. Thread or low-level interrupt
    /// context.
    pub fn unreserve(&self, c: CloneId, size: usize) {
        self.lock.lock().unreserve(c.get(), size);
    }

    /// `drm_illumos_event_deliver` of the encoded event `ev`, then the
    /// wakeups. Thread or low-level interrupt context.
    pub fn deliver(&self, c: CloneId, ev: &[u8]) -> Delivery {
        let d = self.lock.lock().deliver(c.get(), ev);
        if d == Delivery::Queued {
            self.cv.notify_all();
            if let Some(h) = self.polls.get(c.index()) {
                h.wakeup(PollEvents::IN.union(PollEvents::RDNORM));
            }
        }
        d
    }

    /// `read(9E)`: whole events only, as Linux's `drm_read()`. Blocks for
    /// the first event unless the file is non-blocking (`EAGAIN`); a buffer
    /// too small for the next event reads what came before it, possibly
    /// nothing; a fault after events were copied returns those.
    pub fn read(&self, c: CloneId, uio: &mut Uio<'_>, ctx: &Sleepable) -> Result<(), Errno> {
        let nonblock = i32::from(uio.fmode()) & (sys::FNONBLOCK | sys::FNDELAY) != 0;
        let mut copied = 0usize;
        loop {
            let mut g = self.lock.lock();
            let r = match g.begin_read(c.get(), uio.resid()) {
                ReadStep::Event(r) => r,
                ReadStep::TooSmall => return Ok(()),
                ReadStep::Busy | ReadStep::Empty if copied > 0 => return Ok(()),
                ReadStep::Busy | ReadStep::Empty => {
                    if nonblock {
                        return Err(Errno::EAGAIN);
                    }
                    let (g2, interrupted) = self.cv.wait_sig(g, ctx);
                    drop(g2);
                    if interrupted {
                        return Err(Errno::EINTR);
                    }
                    continue;
                }
            };
            drop(g);
            // The event's bytes stay as they are while this reader holds
            // the file's read turn (deliveries write only the free part of
            // the ring, and the file cannot close during a read). An event
            // that wraps around the ring's end is joined first, so that it
            // goes out in one copy and a fault cannot leave half of it.
            let res = match r.pieces {
                [(p, n), (_, 0)] => {
                    // SAFETY: see above.
                    uio.copy_out(unsafe { core::slice::from_raw_parts(p, n) })
                }
                [(p0, n0), (p1, n1)] => {
                    let mut v: Vec<u8> = Vec::with_capacity(r.len);
                    // SAFETY: see above.
                    unsafe {
                        v.extend_from_slice(core::slice::from_raw_parts(p0, n0));
                        v.extend_from_slice(core::slice::from_raw_parts(p1, n1));
                    }
                    uio.copy_out(&v)
                }
            };
            let ok = res.is_ok();
            self.lock.lock().end_read(c.get(), ok.then_some(r.len));
            // Another reader of the file may be waiting for the turn.
            self.cv.notify_all();
            if !ok {
                return if copied == 0 { Err(Errno::EFAULT) } else { Ok(()) };
            }
            copied += r.len;
        }
    }

    /// `chpoll(9E)`: readable while events are queued.
    pub fn chpoll(&self, c: CloneId) -> Result<(PollEvents, PollRegistration<'_>), Errno> {
        let ready = if self.lock.lock().readable(c.get()) {
            PollEvents::IN.union(PollEvents::RDNORM)
        } else {
            PollEvents::NONE
        };
        let h = self.polls.get(c.index()).ok_or(Errno::ENXIO)?;
        Ok((ready, h.registration()))
    }
}
