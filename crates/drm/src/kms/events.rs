// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Vblank counters and the per-file event queues (`drm_vblank.c`,
//! `drm_file.c`).
//!
//! Each CRTC has a vblank counter and the timestamp of its last vblank
//! (`CLOCK_MONOTONIC`, from `gethrtime(9F)`), a flag for a page flip that
//! completes at the next vblank together with the events waiting for it,
//! and a list of events waiting for a vblank sequence (`WAIT_VBLANK` with
//! `_DRM_VBLANK_EVENT`, `CRTC_QUEUE_SEQUENCE`). Each open file has a queue
//! of delivered events that `read(2)` drains and a budget of 4096 bytes
//! for events that are queued or still pending, as in Linux
//! (`event_space`); an event that does not fit fails its ioctl with
//! `ENOMEM`.
//!
//! # Contexts
//!
//! Events are allocated by the ioctl that asks for them, in thread
//! context, and charged to the file's budget. Everything that happens
//! later — a vblank ([`EventCore::handle_vblank`]) completing flips and
//! waits, delivery to a queue — moves the already allocated events between
//! intrusive lists in O(1) per event and allocates nothing, so it is fit
//! for an interrupt handler once the lock around it has the interrupt's
//! priority; the kernel layer's lock does not yet, and its entry point
//! demands the `Sleepable` token (see `kms::dev`). Waits are kept sorted by
//! their sequence, so a vblank touches only the waits it completes.
//! Delivery reports the files that received something
//! as a [`WakeSet`], a fixed-size bit set, so that the caller can wake
//! pollers after releasing its lock (`pollwakeup(9F)` must not be called
//! with a lock that `chpoll(9E)` takes).
//!
//! When a file closes, its queued events and every event it still has
//! pending on a CRTC are unlinked and freed, so a flip or wait that
//! completes later finds nothing of it (Linux's `drm_events_release`).

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::ptr::NonNull;

use ddi::Errno;
use drm_uapi as uapi;

use crate::minor::MAX_FILES;

/// The byte budget of one file's events (Linux: 4096).
pub const EVENT_SPACE: usize = 4096;
/// The most events a device keeps queued or pending over all files; a
/// charge beyond it fails with `ENOMEM`. Linux bounds events per file only;
/// with up to 511 files that would be 65,000 events of locked memory.
pub const MAX_DEVICE_EVENTS: u64 = 8192;
/// The size of every event this core sends (`drm_event_vblank` and
/// `drm_event_crtc_sequence` are both 32 bytes).
pub const EVENT_SIZE: usize = 32;

const _: () = assert!(core::mem::size_of::<uapi::drm_event_vblank>() == EVENT_SIZE);
const _: () = assert!(core::mem::size_of::<uapi::drm_event_crtc_sequence>() == EVENT_SIZE);

/// What an event reports.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EventKind {
    /// `DRM_EVENT_VBLANK` (`WAIT_VBLANK` with `_DRM_VBLANK_EVENT`).
    Vblank {
        /// The caller's `signal` value.
        user_data: u64,
    },
    /// `DRM_EVENT_FLIP_COMPLETE` (page flips and atomic commits with
    /// `DRM_MODE_PAGE_FLIP_EVENT`).
    Flip {
        /// The caller's `user_data`.
        user_data: u64,
    },
    /// `DRM_EVENT_CRTC_SEQUENCE` (`CRTC_QUEUE_SEQUENCE`).
    Sequence {
        /// The caller's `user_data`.
        user_data: u64,
    },
}

/// One event, allocated by the ioctl that asks for it.
#[derive(Debug)]
pub struct Event {
    file: u32,
    kind: EventKind,
    crtc_id: u32,
    /// The vblank sequence a wait completes at.
    target: u64,
    data: [u8; EVENT_SIZE],
    /// The next event of the list this one is on; owned by that list.
    next: Option<NonNull<Event>>,
}

// SAFETY: `next` is a link that only the owning EventList follows; an
// event on no list owns nothing through it.
unsafe impl Send for Event {}

impl Event {
    /// Allocates an event for `file` (a clone ID) about the CRTC with ID
    /// `crtc_id`. Allocation may block; call it without the event lock.
    pub fn new(file: u32, kind: EventKind, crtc_id: u32, _ctx: &ddi::Sleepable) -> Box<Event> {
        Box::new(Event { file, kind, crtc_id, target: 0, data: [0; EVENT_SIZE], next: None })
    }

    /// The encoded event, as `read(2)` returns it.
    pub fn bytes(&self) -> &[u8] {
        &self.data
    }

    /// Fills in the sequence and timestamp and encodes the event.
    fn seal(&mut self, seq: u64, time_ns: i64) {
        let mut d = [0u8; EVENT_SIZE];
        let (typ, user) = match self.kind {
            EventKind::Vblank { user_data } => (uapi::DRM_EVENT_VBLANK, user_data),
            EventKind::Flip { user_data } => (uapi::DRM_EVENT_FLIP_COMPLETE, user_data),
            EventKind::Sequence { user_data } => (uapi::DRM_EVENT_CRTC_SEQUENCE, user_data),
        };
        let put = |d: &mut [u8; EVENT_SIZE], off: usize, b: &[u8]| {
            if let Some(dst) = d.get_mut(off..off + b.len()) {
                dst.copy_from_slice(b);
            }
        };
        put(&mut d, 0, &typ.to_ne_bytes());
        put(&mut d, 4, &(EVENT_SIZE as u32).to_ne_bytes());
        put(&mut d, 8, &user.to_ne_bytes());
        if matches!(self.kind, EventKind::Sequence { .. }) {
            put(&mut d, 16, &time_ns.to_ne_bytes());
            put(&mut d, 24, &seq.to_ne_bytes());
        } else {
            let t = time_ns.max(0) as u64;
            put(&mut d, 16, &((t / 1_000_000_000) as u32).to_ne_bytes());
            put(&mut d, 20, &(((t % 1_000_000_000) / 1000) as u32).to_ne_bytes());
            put(&mut d, 24, &(seq as u32).to_ne_bytes());
            put(&mut d, 28, &self.crtc_id.to_ne_bytes());
        }
        self.data = d;
    }
}

/// A singly linked list of events, linked through the events themselves,
/// with a tail pointer: appending, prepending and taking the first event
/// are O(1) and allocate nothing.
///
/// The list owns its events: each was a `Box<Event>` turned into a raw
/// pointer by [`EventList::push_back`] or [`EventList::push_front`] and
/// becomes one again in [`EventList::pop_front`]. `head` and `tail` are
/// both `None` or both point into the chain, `tail` at the last event.
#[derive(Debug, Default)]
pub struct EventList {
    head: Option<NonNull<Event>>,
    tail: Option<NonNull<Event>>,
    len: usize,
}

// SAFETY: the list owns its events exclusively (see above), and Event is
// Send.
unsafe impl Send for EventList {}

impl EventList {
    /// An empty list.
    pub const fn new() -> EventList {
        EventList { head: None, tail: None, len: 0 }
    }

    /// The number of events.
    pub fn len(&self) -> usize {
        self.len
    }

    /// Whether the list is empty.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    fn first(&self) -> Option<&Event> {
        // SAFETY: head points at an event the list owns; the shared borrow
        // of the list keeps it alive and unmodified.
        self.head.map(|p| unsafe { &*p.as_ptr() })
    }

    /// Appends an event.
    pub fn push_back(&mut self, mut e: Box<Event>) {
        e.next = None;
        let p = NonNull::from(Box::leak(e));
        match self.tail {
            // SAFETY: tail points at the last event, owned by this list.
            Some(t) => unsafe { (*t.as_ptr()).next = Some(p) },
            None => self.head = Some(p),
        }
        self.tail = Some(p);
        self.len += 1;
    }

    /// Prepends an event.
    pub fn push_front(&mut self, mut e: Box<Event>) {
        e.next = self.head;
        let p = NonNull::from(Box::leak(e));
        if self.tail.is_none() {
            self.tail = Some(p);
        }
        self.head = Some(p);
        self.len += 1;
    }

    /// Removes the first event.
    pub fn pop_front(&mut self) -> Option<Box<Event>> {
        let p = self.head?;
        // SAFETY: p came from Box::leak in a push and is owned by this list;
        // it is unlinked here, so it is turned back into a Box once.
        let mut e = unsafe { Box::from_raw(p.as_ptr()) };
        self.head = e.next.take();
        if self.head.is_none() {
            self.tail = None;
        }
        self.len -= 1;
        Some(e)
    }

    /// Moves every event for which `pred` holds to `out`, keeping the order
    /// of both; one pass.
    fn extract(&mut self, mut pred: impl FnMut(&Event) -> bool, out: &mut EventList) {
        let mut rest = EventList::new();
        while let Some(e) = self.pop_front() {
            if pred(&e) { out.push_back(e) } else { rest.push_back(e) }
        }
        *self = rest;
    }

    /// Inserts an event after every event with a target not above its own,
    /// keeping the list sorted by target. O(n) in the ioctl that queues it.
    fn insert_sorted(&mut self, e: Box<Event>) {
        if self.tail.is_none_or(|t| {
            // SAFETY: tail points at an event this list owns.
            unsafe { (*t.as_ptr()).target <= e.target }
        }) {
            self.push_back(e);
            return;
        }
        let mut before = EventList::new();
        while self.first().is_some_and(|f| f.target <= e.target) {
            if let Some(x) = self.pop_front() {
                before.push_back(x);
            }
        }
        before.push_back(e);
        while let Some(x) = self.pop_front() {
            before.push_back(x);
        }
        *self = before;
    }

    /// Moves the leading events whose target is at most `seq` to `out`:
    /// the waits a sorted list completes at vblank `seq`. O(completed).
    fn take_due(&mut self, seq: u64, out: &mut EventList) {
        while self.first().is_some_and(|f| f.target <= seq) {
            if let Some(e) = self.pop_front() {
                out.push_back(e);
            }
        }
    }
}

impl Drop for EventList {
    fn drop(&mut self) {
        // Iteratively, so that a long list cannot exhaust the kernel stack.
        while self.pop_front().is_some() {}
    }
}

/// A set of files (clone IDs), without allocation.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WakeSet([u64; 8]);

const _: () = assert!(MAX_FILES < 512);

impl WakeSet {
    /// Adds a file.
    pub fn add(&mut self, file: u32) {
        if let Some(w) = self.0.get_mut((file / 64) as usize) {
            *w |= 1 << (file % 64);
        }
    }
    /// Adds every file of another set.
    pub fn merge(&mut self, o: WakeSet) {
        for (a, b) in self.0.iter_mut().zip(o.0) {
            *a |= b;
        }
    }
    /// Whether the set is empty.
    pub fn is_empty(&self) -> bool {
        self.0.iter().all(|w| *w == 0)
    }
    /// The files in the set.
    pub fn iter(&self) -> impl Iterator<Item = u32> + '_ {
        (0u32..512).filter(|f| self.0.get((f / 64) as usize).is_some_and(|w| w & (1 << (f % 64)) != 0))
    }
}

#[derive(Debug, Default)]
struct FileEvents {
    open: bool,
    /// A `read(2)` of this file is copying an event out.
    reading: bool,
    /// Bytes of the budget still free.
    space: usize,
    queue: EventList,
}

/// The vblank state of one CRTC.
#[derive(Debug, Default)]
struct CrtcVblank {
    /// The CRTC is active and vblanks arrive.
    enabled: bool,
    count: u64,
    time_ns: i64,
    /// The length of a frame of the current mode, 0 if unknown.
    frame_ns: u64,
    /// A flip is armed and completes at the next vblank.
    flip_pending: bool,
    flip_events: EventList,
    waiters: EventList,
}

/// Statistics of the event core.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EventStats {
    /// Events queued or pending in all files.
    pub outstanding: u64,
    /// Events delivered so far.
    pub delivered: u64,
    /// Events freed because their file closed first.
    pub discarded: u64,
    /// Vblanks so far, all CRTCs.
    pub vblanks: u64,
    /// Flips completed so far.
    pub flips: u64,
}

/// The vblank counters and event queues of a device. See the module
/// documentation.
#[derive(Debug)]
pub struct EventCore {
    files: Vec<FileEvents>,
    crtcs: Vec<CrtcVblank>,
    crtc_ids: Vec<u32>,
    stats: EventStats,
}

impl EventCore {
    /// Creates the state for CRTCs with the given IDs.
    pub fn new(crtc_ids: &[u32], _ctx: &ddi::Sleepable) -> EventCore {
        EventCore {
            files: (0..=MAX_FILES).map(|_| FileEvents::default()).collect(),
            crtcs: crtc_ids.iter().map(|_| CrtcVblank::default()).collect(),
            crtc_ids: crtc_ids.to_vec(),
            stats: EventStats::default(),
        }
    }

    /// The statistics.
    pub fn stats(&self) -> EventStats {
        self.stats
    }

    /// Registers an opened file.
    pub fn open(&mut self, file: u32) {
        if let Some(f) = self.files.get_mut(file as usize) {
            *f = FileEvents { open: true, reading: false, space: EVENT_SPACE, queue: EventList::new() };
        }
    }

    /// Forgets a closed file: its queued events and every event it has
    /// pending on a CRTC are unlinked. They are returned so that the caller
    /// frees them after dropping its lock.
    pub fn close(&mut self, file: u32) -> EventList {
        let mut gone = EventList::new();
        for c in &mut self.crtcs {
            c.flip_events.extract(|e| e.file == file, &mut gone);
            c.waiters.extract(|e| e.file == file, &mut gone);
        }
        if let Some(f) = self.files.get_mut(file as usize) {
            while let Some(e) = f.queue.pop_front() {
                gone.push_back(e);
            }
            f.open = false;
            f.space = 0;
        }
        self.stats.outstanding = self.stats.outstanding.saturating_sub(gone.len() as u64);
        self.stats.discarded += gone.len() as u64;
        gone
    }

    /// Charges `n` events to a file's budget: `ENOMEM` if they do not fit
    /// (Linux's `drm_event_reserve_init`).
    pub fn charge(&mut self, file: u32, n: usize) -> Result<(), Errno> {
        self.charge_as(file, n, false)
    }

    /// [`EventCore::charge`], for the current master when `master` is set:
    /// it is exempt from [`MAX_DEVICE_EVENTS`] (its own 4096 bytes still
    /// apply), so that other clients cannot starve the display server's
    /// flip events.
    pub fn charge_as(&mut self, file: u32, n: usize, master: bool) -> Result<(), Errno> {
        let f = self.files.get_mut(file as usize).filter(|f| f.open).ok_or(Errno::ENXIO)?;
        let bytes = n.checked_mul(EVENT_SIZE).ok_or(Errno::ENOMEM)?;
        let device_full = !master && self.stats.outstanding.saturating_add(n as u64) > MAX_DEVICE_EVENTS;
        if f.space < bytes || device_full {
            return Err(Errno::ENOMEM);
        }
        f.space -= bytes;
        self.stats.outstanding += n as u64;
        Ok(())
    }

    /// Returns charged budget of events that were never sent (a commit
    /// that failed after the charge).
    pub fn uncharge(&mut self, file: u32, n: usize) {
        if let Some(f) = self.files.get_mut(file as usize).filter(|f| f.open) {
            f.space = (f.space + n * EVENT_SIZE).min(EVENT_SPACE);
        }
        self.stats.outstanding = self.stats.outstanding.saturating_sub(n as u64);
    }

    fn crtc_index(&self, crtc_id: u32) -> Option<usize> {
        self.crtc_ids.iter().position(|&c| c == crtc_id)
    }

    /// Whether vblanks arrive on CRTC `crtc`.
    pub fn enabled(&self, crtc: usize) -> bool {
        self.crtcs.get(crtc).is_some_and(|c| c.enabled)
    }

    /// The counter and the time of the last vblank of CRTC `crtc`.
    pub fn count(&self, crtc: usize) -> (u64, i64) {
        self.crtcs.get(crtc).map_or((0, 0), |c| (c.count, c.time_ns))
    }

    /// The frame length of CRTC `crtc` in ns (0 if unknown).
    pub fn frame_ns(&self, crtc: usize) -> u64 {
        self.crtcs.get(crtc).map_or(0, |c| c.frame_ns)
    }

    /// Whether a flip is armed on any CRTC of `mask`.
    pub fn flip_pending(&self, mask: u32) -> bool {
        self.crtcs.iter().enumerate().any(|(i, c)| mask & (1 << i) != 0 && c.flip_pending)
    }

    /// Delivers one event now, with the sequence and time given.
    fn deliver(&mut self, mut e: Box<Event>, seq: u64, time_ns: i64, wake: &mut WakeSet) {
        e.seal(seq, time_ns);
        self.stats.delivered += 1;
        let file = e.file;
        match self.files.get_mut(file as usize) {
            Some(f) if f.open => {
                f.queue.push_back(e);
                wake.add(file);
            }
            // Only events of open files are ever linked anywhere (close
            // unlinks them), so this is a safety net: free it.
            _ => {
                self.stats.outstanding = self.stats.outstanding.saturating_sub(1);
                self.stats.discarded += 1;
                drop(e);
            }
        }
    }

    /// Turns vblanks of CRTC `crtc` on or off, for a CRTC that became
    /// active or inactive. `frame_ns` is the frame length of the new mode
    /// and `now` the current time. Turning them off completes the armed
    /// flip and every waiting event with the last count, as Linux's
    /// `drm_crtc_vblank_off` does.
    pub fn set_enabled(&mut self, crtc: usize, on: bool, frame_ns: u64, now: i64) -> WakeSet {
        let mut wake = WakeSet::default();
        let Some(c) = self.crtcs.get_mut(crtc) else { return wake };
        c.frame_ns = frame_ns;
        if on {
            if !c.enabled {
                c.enabled = true;
                c.time_ns = now;
            }
            return wake;
        }
        c.enabled = false;
        let (count, time) = (c.count, c.time_ns);
        let mut done = EventList::new();
        c.flip_events.extract(|_| true, &mut done);
        c.waiters.extract(|_| true, &mut done);
        let flip = core::mem::take(&mut c.flip_pending);
        if flip {
            self.stats.flips += 1;
        }
        while let Some(e) = done.pop_front() {
            self.deliver(e, count, time, &mut wake);
        }
        wake
    }

    /// Records a vblank of CRTC `crtc` at `time_ns`: the counter advances,
    /// the armed flip completes, and waits that reached their sequence are
    /// delivered. Allocates nothing; callable from interrupt context.
    pub fn handle_vblank(&mut self, crtc: usize, time_ns: i64) -> WakeSet {
        let mut wake = WakeSet::default();
        let Some(c) = self.crtcs.get_mut(crtc).filter(|c| c.enabled) else { return wake };
        c.count = c.count.wrapping_add(1);
        c.time_ns = time_ns;
        let count = c.count;
        let mut done = EventList::new();
        if core::mem::take(&mut c.flip_pending) {
            c.flip_events.extract(|_| true, &mut done);
            self.stats.flips += 1;
        }
        c.waiters.take_due(count, &mut done);
        self.stats.vblanks += 1;
        while let Some(e) = done.pop_front() {
            self.deliver(e, count, time_ns, &mut wake);
        }
        wake
    }

    /// Arms a flip on CRTC `crtc` that completes at its next vblank, with
    /// the events that report it (already charged).
    pub fn arm_flip(&mut self, crtc: usize, mut events: EventList) {
        let Some(c) = self.crtcs.get_mut(crtc) else { return };
        c.flip_pending = true;
        while let Some(e) = events.pop_front() {
            c.flip_events.push_back(e);
        }
    }

    /// Sends flip events at once, for a CRTC that the update turned off.
    pub fn send_now(&mut self, crtc: usize, mut events: EventList) -> WakeSet {
        let mut wake = WakeSet::default();
        let (count, time) = self.count(crtc);
        while let Some(e) = events.pop_front() {
            self.deliver(e, count, time, &mut wake);
        }
        wake
    }

    /// Queues an event for the vblank with sequence `target` on CRTC
    /// `crtc`, or delivers it at once if that vblank has passed (Linux's
    /// `drm_queue_vblank_event`). `EINVAL` if vblanks are off.
    pub fn queue_wait(&mut self, crtc: usize, target: u64, mut e: Box<Event>) -> Result<WakeSet, (Box<Event>, Errno)> {
        let Some(c) = self.crtcs.get_mut(crtc).filter(|c| c.enabled) else { return Err((e, Errno::EINVAL)) };
        let mut wake = WakeSet::default();
        if c.count >= target {
            let (count, time) = (c.count, c.time_ns);
            self.deliver(e, count, time, &mut wake);
        } else {
            e.target = target;
            c.waiters.insert_sorted(e);
        }
        Ok(wake)
    }

    /// The CRTC index of a CRTC ID.
    pub fn crtc_of(&self, crtc_id: u32) -> Option<usize> {
        self.crtc_index(crtc_id)
    }

    /// Starts a `read(2)` of a file: `false` while another reader of the
    /// same file is copying an event out (the caller waits and retries).
    pub fn begin_read(&mut self, file: u32) -> bool {
        match self.files.get_mut(file as usize) {
            Some(f) if !f.reading => {
                f.reading = true;
                true
            }
            _ => false,
        }
    }

    /// Ends what [`EventCore::begin_read`] started.
    pub fn end_read(&mut self, file: u32) {
        if let Some(f) = self.files.get_mut(file as usize) {
            f.reading = false;
        }
    }

    /// Whether a file has events to read.
    pub fn readable(&self, file: u32) -> bool {
        self.files.get(file as usize).is_some_and(|f| !f.queue.is_empty())
    }

    /// Takes the first queued event of a file if it fits in `room` bytes,
    /// crediting its budget; `put_back` undoes this if copying it out
    /// fails.
    pub fn pop(&mut self, file: u32, room: usize) -> Option<Box<Event>> {
        let f = self.files.get_mut(file as usize)?;
        if f.queue.first()?.data.len() > room {
            return None;
        }
        let e = f.queue.pop_front()?;
        f.space = (f.space + EVENT_SIZE).min(EVENT_SPACE);
        self.stats.outstanding = self.stats.outstanding.saturating_sub(1);
        Some(e)
    }

    /// Returns an event that [`EventCore::pop`] took to the front of the
    /// queue.
    pub fn put_back(&mut self, e: Box<Event>) {
        let file = e.file;
        match self.files.get_mut(file as usize) {
            Some(f) if f.open => {
                f.space = f.space.saturating_sub(EVENT_SIZE);
                f.queue.push_front(e);
                self.stats.outstanding += 1;
            }
            _ => drop(e),
        }
    }
}

/// Widens a 32-bit vblank sequence from userland to the 64-bit counter,
/// choosing the value nearest to `now` (Linux's `widen_32_to_64`).
pub fn widen_32_to_64(req: u32, now: u64) -> u64 {
    now.wrapping_add(req.wrapping_sub(now as u32) as i32 as i64 as u64)
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;

    fn ctx() -> ddi::Sleepable {
        // SAFETY: host tests run in an ordinary thread.
        unsafe { ddi::Sleepable::assume() }
    }

    fn flip(file: u32, user: u64) -> Box<Event> {
        Event::new(file, EventKind::Flip { user_data: user }, 42, &ctx())
    }

    fn read_all(ev: &mut EventCore, file: u32) -> std::vec::Vec<[u8; 32]> {
        let mut v = std::vec::Vec::new();
        while let Some(e) = ev.pop(file, 64) {
            v.push(e.data);
        }
        v
    }

    fn u32_at(d: &[u8; 32], o: usize) -> u32 {
        u32::from_ne_bytes(d[o..o + 4].try_into().unwrap())
    }

    #[test]
    fn flip_completes_at_next_vblank() {
        let mut ev = EventCore::new(&[42], &ctx());
        ev.open(3);
        assert!(ev.set_enabled(0, true, 16_666_666, 1_000).is_empty());
        ev.charge(3, 1).unwrap();
        let mut l = EventList::new();
        l.push_back(flip(3, 0xabcd));
        ev.arm_flip(0, l);
        assert!(ev.flip_pending(1));
        assert!(!ev.readable(3));
        let w = ev.handle_vblank(0, 2_500_000_000);
        assert_eq!(w.iter().collect::<std::vec::Vec<_>>(), [3]);
        assert!(!ev.flip_pending(1));
        let got = read_all(&mut ev, 3);
        assert_eq!(got.len(), 1);
        let d = got[0];
        assert_eq!(u32_at(&d, 0), uapi::DRM_EVENT_FLIP_COMPLETE);
        assert_eq!(u32_at(&d, 4), 32);
        assert_eq!(u64::from_ne_bytes(d[8..16].try_into().unwrap()), 0xabcd);
        assert_eq!((u32_at(&d, 16), u32_at(&d, 20)), (2, 500_000));
        assert_eq!(u32_at(&d, 24), 1);
        assert_eq!(u32_at(&d, 28), 42);
        assert_eq!(ev.stats().outstanding, 0);
    }

    #[test]
    fn budget_and_close() {
        let mut ev = EventCore::new(&[42], &ctx());
        ev.open(1);
        ev.set_enabled(0, true, 1, 0);
        assert_eq!(ev.charge(1, EVENT_SPACE / EVENT_SIZE + 1), Err(Errno::ENOMEM));
        ev.charge(1, EVENT_SPACE / EVENT_SIZE).unwrap();
        assert_eq!(ev.charge(1, 1), Err(Errno::ENOMEM));
        ev.uncharge(1, 3);
        // Three waits: at sequence 1, 2 and 5.
        for t in [1, 2, 5] {
            let e = Event::new(1, EventKind::Vblank { user_data: t }, 42, &ctx());
            assert!(ev.queue_wait(0, t, e).is_ok());
        }
        ev.handle_vblank(0, 10);
        ev.handle_vblank(0, 20);
        assert_eq!(read_all(&mut ev, 1).len(), 2);
        // The file closes with one wait pending; the later vblank finds
        // nothing of it.
        let gone = ev.close(1);
        assert_eq!(gone.len(), 1);
        drop(gone);
        for t in 0..5 {
            ev.handle_vblank(0, 30 + t);
        }
        assert!(!ev.readable(1));
        assert_eq!(ev.stats().discarded, 1);
        assert_eq!(ev.charge(1, 1), Err(Errno::ENXIO));
    }

    #[test]
    fn waits_passed_and_disabled() {
        let mut ev = EventCore::new(&[7, 8], &ctx());
        ev.open(2);
        ev.charge(2, 3).unwrap();
        let e = Event::new(2, EventKind::Sequence { user_data: 9 }, 8, &ctx());
        assert_eq!(ev.queue_wait(1, 0, e).err().map(|x| x.1), Some(Errno::EINVAL), "vblank off");
        ev.set_enabled(1, true, 1, 100);
        ev.handle_vblank(1, 200);
        let e = Event::new(2, EventKind::Sequence { user_data: 9 }, 8, &ctx());
        let w = ev.queue_wait(1, 1, e).unwrap();
        assert!(!w.is_empty(), "sequence 1 has passed: delivered at once");
        let d = read_all(&mut ev, 2)[0];
        assert_eq!(u32_at(&d, 0), uapi::DRM_EVENT_CRTC_SEQUENCE);
        assert_eq!(i64::from_ne_bytes(d[16..24].try_into().unwrap()), 200);
        assert_eq!(u64::from_ne_bytes(d[24..32].try_into().unwrap()), 1);
        // A flip on a CRTC that turns off completes with the old count.
        let mut l = EventList::new();
        l.push_back(flip(2, 1));
        ev.arm_flip(1, l);
        let w = ev.set_enabled(1, false, 0, 300);
        assert!(!w.is_empty());
        assert_eq!(read_all(&mut ev, 2).len(), 1);
        assert_eq!(ev.handle_vblank(1, 400), WakeSet::default(), "no vblanks while off");
        assert_eq!(ev.count(1), (1, 200));
    }

    #[test]
    fn read_whole_events_only_and_put_back() {
        let mut ev = EventCore::new(&[1], &ctx());
        ev.open(5);
        ev.set_enabled(0, true, 1, 0);
        ev.charge(5, 2).unwrap();
        let mut l = EventList::new();
        l.push_back(flip(5, 1));
        l.push_back(flip(5, 2));
        ev.arm_flip(0, l);
        ev.handle_vblank(0, 1);
        assert!(ev.pop(5, 31).is_none(), "a buffer smaller than an event gets nothing");
        let e = ev.pop(5, 32).unwrap();
        ev.put_back(e);
        let got = read_all(&mut ev, 5);
        assert_eq!(got.len(), 2);
        assert_eq!(got[0][8], 1, "order kept after put_back");
    }

    #[test]
    fn sorted_waits_and_list_invariants() {
        let mut ev = EventCore::new(&[1], &ctx());
        ev.open(4);
        ev.set_enabled(0, true, 1, 0);
        ev.charge(4, 5).unwrap();
        for t in [5u64, 2, 9, 2, 3] {
            let e = Event::new(4, EventKind::Vblank { user_data: t }, 1, &ctx());
            assert!(ev.queue_wait(0, t, e).is_ok());
        }
        let mut seen = std::vec::Vec::new();
        for i in 1..=9 {
            ev.handle_vblank(0, i);
            while let Some(e) = ev.pop(4, 64) {
                seen.push((u64::from_ne_bytes(e.data[8..16].try_into().unwrap()), u32_at(&e.data, 24)));
            }
        }
        assert_eq!(seen, [(2, 2), (2, 2), (3, 3), (5, 5), (9, 9)], "each at its vblank, in target order");
        // The list keeps its tail across emptying and refilling.
        let mut l = EventList::new();
        l.push_front(flip(4, 1));
        assert!(l.pop_front().is_some() && l.is_empty());
        l.push_back(flip(4, 2));
        l.push_front(flip(4, 1));
        l.push_back(flip(4, 3));
        let order: std::vec::Vec<u8> = core::iter::from_fn(|| l.pop_front())
            .map(|e| match e.kind {
                EventKind::Flip { user_data } => user_data as u8,
                _ => 0,
            })
            .collect();
        assert_eq!(order, [1, 2, 3]);
    }

    #[test]
    fn device_cap() {
        let mut ev = EventCore::new(&[1], &ctx());
        let per_file = EVENT_SPACE / EVENT_SIZE;
        let files = (MAX_DEVICE_EVENTS as usize).div_ceil(per_file) + 1;
        let mut total = 0;
        for f in 1..=files as u32 {
            ev.open(f);
            while ev.charge(f, 1).is_ok() {
                total += 1;
            }
        }
        assert_eq!(total, MAX_DEVICE_EVENTS as usize);
        // The master still gets its own budget.
        let m = files as u32 + 1;
        ev.open(m);
        assert_eq!(ev.charge(m, 1), Err(Errno::ENOMEM));
        assert_eq!(ev.charge_as(m, 1, true), Ok(()));
    }

    #[test]
    fn widen() {
        assert_eq!(widen_32_to_64(5, 3), 5);
        assert_eq!(widen_32_to_64(1, 0x1_ffff_fff0), 0x2_0000_0001);
        assert_eq!(widen_32_to_64(0xffff_fff0, 0x2_0000_0001), 0x1_ffff_fff0);
    }

    #[test]
    fn wake_set() {
        let mut w = WakeSet::default();
        w.add(1);
        w.add(511);
        let mut o = WakeSet::default();
        o.add(64);
        w.merge(o);
        assert_eq!(w.iter().collect::<std::vec::Vec<_>>(), [1, 64, 511]);
    }
}
