// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The event queues of a Linux-KMS device's files (facade 2.0,
//! `drm_illumos_event_reserve`, `_unreserve` and `_deliver` of
//! `drm_lkms.h`).
//!
//! The Linux KMS code builds its events (`struct drm_pending_event`) itself
//! and hands the core the encoded `struct drm_event` when it sends one.
//! Each file has Linux's budget of [`EVENT_SPACE`] bytes (`event_space`):
//! reserving charges it, unreserving gives it back, delivering moves the
//! reserved bytes into the file's read queue, and `read(2)` gives them back
//! when it has copied the event out. Delivered plus reserved bytes never
//! exceed the budget, so the read queue is a byte ring of [`EVENT_SPACE`]
//! bytes allocated when the file opens: delivery only copies, in any
//! context, and never allocates. As for R3, the bytes charged by all files
//! of a device that are not the current master are bounded as well
//! ([`MAX_DEVICE_BYTES`]).
//!
//! A file that is closing ([`LkmsEvents::begin_close`]) loses its queued
//! events at once; later reservations fail with `ENOMEM` and later
//! deliveries are dropped, their space given back, until the file is gone
//! ([`LkmsEvents::close`]).
//!
//! Everything here is plain data behind the caller's lock (`lkms::dev`
//! in the kernel); a reader copies an event out without the lock while it
//! holds the file's read turn ([`LkmsEvents::begin_read`]), during which
//! deliveries only write the free part of the ring.

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::ptr::NonNull;

use ddi::{Errno, Sleepable};

use crate::kms::events::{EVENT_SIZE, EVENT_SPACE, MAX_DEVICE_EVENTS};
use crate::minor::MAX_FILES;

/// The bytes all files of a device that are not its current master may
/// have charged together, reserved or queued: R3's bound
/// ([`MAX_DEVICE_EVENTS`] events of 32 bytes), 256 KiB.
pub const MAX_DEVICE_BYTES: usize = MAX_DEVICE_EVENTS as usize * EVENT_SIZE;

/// The size of `struct drm_event`, the header every event starts with
/// (`type`, then `length`, both `u32`).
pub const HEADER: usize = 8;

/// A ring of [`EVENT_SPACE`] bytes. The buffer is reached through a raw
/// pointer only, so that a reader may copy the bytes of the first event
/// while the owner of the lock appends to the free part.
struct Ring {
    buf: NonNull<u8>,
    head: usize,
    len: usize,
}

// SAFETY: the ring owns its buffer exclusively; see the type's comment for
// the one shared use, which the read turn orders.
unsafe impl Send for Ring {}

impl Ring {
    fn new(_ctx: &Sleepable) -> Ring {
        // Zeroed and allocated in place, not built on the stack.
        let b: Box<[u8]> = alloc::vec![0u8; EVENT_SPACE].into_boxed_slice();
        let p = Box::into_raw(b) as *mut u8;
        // Box::into_raw never returns null.
        Ring { buf: NonNull::new(p).unwrap_or(NonNull::dangling()), head: 0, len: 0 }
    }

    fn free(&self) -> usize {
        EVENT_SPACE - self.len
    }

    /// Appends `src`, which must fit in [`Ring::free`].
    fn push(&mut self, src: &[u8]) {
        let n = src.len().min(self.free());
        let tail = (self.head + self.len) % EVENT_SPACE;
        let first = n.min(EVENT_SPACE - tail);
        // SAFETY: tail + first <= EVENT_SPACE and n - first < head (the
        // free part of the ring); src has n bytes. Nothing reads the free
        // part.
        unsafe {
            core::ptr::copy_nonoverlapping(src.as_ptr(), self.buf.as_ptr().add(tail), first);
            core::ptr::copy_nonoverlapping(src.as_ptr().add(first), self.buf.as_ptr(), n - first);
        }
        self.len += n;
    }

    /// The byte at offset `off` from the head.
    fn byte(&self, off: usize) -> u8 {
        // SAFETY: the index is reduced into the buffer.
        unsafe { *self.buf.as_ptr().add((self.head + off) % EVENT_SPACE) }
    }

    /// The `u32` at offset `off` from the head, in native byte order.
    fn u32_at(&self, off: usize) -> u32 {
        u32::from_ne_bytes([self.byte(off), self.byte(off + 1), self.byte(off + 2), self.byte(off + 3)])
    }

    /// The first `n` bytes as at most two pieces of the buffer.
    fn pieces(&self, n: usize) -> [(*const u8, usize); 2] {
        let n = n.min(self.len);
        let first = n.min(EVENT_SPACE - self.head);
        // SAFETY: head < EVENT_SPACE, so both pointers are in the buffer.
        unsafe { [(self.buf.as_ptr().add(self.head), first), (self.buf.as_ptr(), n - first)] }
    }

    fn pop(&mut self, n: usize) {
        let n = n.min(self.len);
        self.head = (self.head + n) % EVENT_SPACE;
        self.len -= n;
        if self.len == 0 {
            self.head = 0;
        }
    }

    fn clear(&mut self) {
        self.head = 0;
        self.len = 0;
    }
}

impl Drop for Ring {
    fn drop(&mut self) {
        let s = core::ptr::slice_from_raw_parts_mut(self.buf.as_ptr(), EVENT_SPACE);
        // SAFETY: the buffer came from Box::into_raw of a boxed slice of
        // EVENT_SPACE bytes in new, and is freed once.
        drop(unsafe { Box::from_raw(s) });
    }
}

/// The event state of one open file.
pub struct FileQueue {
    ring: Ring,
    /// Bytes reserved and not yet delivered or given back.
    reserved: usize,
    closing: bool,
    /// A `read(2)` of the file holds the first event (see
    /// [`LkmsEvents::begin_read`]).
    reading: bool,
    /// The file is the current master, which the device-wide bound
    /// exempts (a snapshot the core keeps current).
    master: bool,
}

impl FileQueue {
    /// Allocates the queue of a file that is being opened. Allocation may
    /// block: call it without the event lock.
    pub fn new(master: bool, ctx: &Sleepable) -> Box<FileQueue> {
        Box::new(FileQueue { ring: Ring::new(ctx), reserved: 0, closing: false, reading: false, master })
    }

    fn charged(&self) -> usize {
        self.reserved + self.ring.len
    }
}

/// What became of a delivered event.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Delivery {
    /// Queued: wake the file's readers and pollers.
    Queued,
    /// Dropped because the file is closing or gone; its space is given
    /// back.
    Dropped,
    /// Refused: not an event of this size (shorter than its header, a
    /// length field that differs from its size, more than the file had
    /// reserved). Nothing changed.
    Refused,
}

/// The first event of a file, held for a reader.
#[derive(Clone, Copy, Debug)]
pub struct EventRef {
    /// The event's bytes, in at most two pieces (the second empty unless
    /// the event wraps around the ring's end). Valid until
    /// [`LkmsEvents::end_read`] of the file; nothing writes them meanwhile.
    pub pieces: [(*const u8, usize); 2],
    /// The event's length.
    pub len: usize,
}

/// What a reader finds.
#[derive(Clone, Copy, Debug)]
pub enum ReadStep {
    /// Another reader of the file holds the first event: wait and retry.
    Busy,
    /// No event is queued (or the file is unknown).
    Empty,
    /// The first event is longer than the reader's buffer.
    TooSmall,
    /// The first event, held for this reader until
    /// [`LkmsEvents::end_read`].
    Event(EventRef),
}

/// Statistics of a device's Linux-KMS events.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LkmsEventStats {
    /// Bytes charged by every file, reserved or queued.
    pub charged: usize,
    /// Bytes charged by files that are not the current master, which
    /// [`MAX_DEVICE_BYTES`] bounds.
    pub charged_others: usize,
    /// Events queued so far.
    pub delivered: u64,
    /// Events dropped because their file was closing or gone, and events
    /// queued for a file that closed before reading them.
    pub dropped: u64,
    /// Deliveries refused as malformed.
    pub refused: u64,
}

/// The event queues of one Linux-KMS device, indexed by clone ID. See the
/// module documentation.
pub struct LkmsEvents {
    files: Vec<Option<Box<FileQueue>>>,
    stats: LkmsEventStats,
}

impl LkmsEvents {
    /// The state of a device without open files.
    pub fn new(_ctx: &Sleepable) -> LkmsEvents {
        LkmsEvents { files: (0..=MAX_FILES).map(|_| None).collect(), stats: LkmsEventStats::default() }
    }

    /// The statistics.
    pub fn stats(&self) -> LkmsEventStats {
        self.stats
    }

    fn file(&self, file: u32) -> Option<&FileQueue> {
        self.files.get(file as usize).and_then(|f| f.as_deref())
    }

    fn file_mut(&mut self, file: u32) -> Option<&mut FileQueue> {
        self.files.get_mut(file as usize).and_then(|f| f.as_deref_mut())
    }

    /// Installs the queue of a file being opened. Gives the queue back if
    /// the clone ID is out of range or still has one (neither happens: the
    /// core frees a file's queue before its clone ID).
    pub fn open(&mut self, file: u32, q: Box<FileQueue>) -> Result<(), Box<FileQueue>> {
        match self.files.get_mut(file as usize) {
            Some(slot @ None) => {
                *slot = Some(q);
                Ok(())
            }
            _ => Err(q),
        }
    }

    /// Updates the master snapshot of a file.
    pub fn set_master(&mut self, file: u32, master: bool) {
        let Some(f) = self.files.get_mut(file as usize).and_then(|f| f.as_deref_mut()) else { return };
        if f.master != master {
            let n = f.charged();
            f.master = master;
            if master {
                self.stats.charged_others = self.stats.charged_others.saturating_sub(n);
            } else {
                self.stats.charged_others += n;
            }
        }
    }

    fn uncharge(&mut self, n: usize, master: bool) {
        self.stats.charged = self.stats.charged.saturating_sub(n);
        if !master {
            self.stats.charged_others = self.stats.charged_others.saturating_sub(n);
        }
    }

    /// Starts the close of a file: its queued events are dropped, and from
    /// now on reservations fail and deliveries are dropped.
    pub fn begin_close(&mut self, file: u32) {
        let Some(f) = self.file_mut(file) else { return };
        f.closing = true;
        let (n, master) = (f.ring.len, f.master);
        let events = count_events(&f.ring);
        f.ring.clear();
        self.stats.dropped += events;
        self.uncharge(n, master);
    }

    /// Removes the queue of a closed file and gives back what it still
    /// had charged (reservations its KMS code never delivered or gave
    /// back). The caller frees the queue after dropping its lock.
    pub fn close(&mut self, file: u32) -> Option<Box<FileQueue>> {
        let q = self.files.get_mut(file as usize)?.take()?;
        self.stats.dropped += count_events(&q.ring);
        self.uncharge(q.charged(), q.master);
        Some(q)
    }

    /// `drm_illumos_event_reserve`: charges `size` bytes to the file's
    /// budget. `ENOMEM` if they do not fit in it, in the device's bound
    /// (unless the file is the current master), or the file is closing;
    /// `EINVAL` for an empty event or an unknown file.
    pub fn reserve(&mut self, file: u32, size: usize) -> Result<(), Errno> {
        if size == 0 {
            return Err(Errno::EINVAL);
        }
        let others = self.stats.charged_others;
        let f = self.file_mut(file).ok_or(Errno::EINVAL)?;
        if f.closing || size > EVENT_SPACE - f.charged() {
            return Err(Errno::ENOMEM);
        }
        if !f.master && others.saturating_add(size) > MAX_DEVICE_BYTES {
            return Err(Errno::ENOMEM);
        }
        f.reserved += size;
        let master = f.master;
        self.stats.charged += size;
        if !master {
            self.stats.charged_others += size;
        }
        Ok(())
    }

    /// `drm_illumos_event_unreserve`: gives back `size` bytes reserved for
    /// an event that is not delivered (at most what the file has
    /// reserved).
    pub fn unreserve(&mut self, file: u32, size: usize) {
        let Some(f) = self.file_mut(file) else { return };
        let n = size.min(f.reserved);
        f.reserved -= n;
        let master = f.master;
        self.uncharge(n, master);
    }

    /// `drm_illumos_event_deliver`: appends the encoded event `ev` (a
    /// `struct drm_event` header and its payload) to the file's queue. Its
    /// reserved space becomes queued space, which `read(2)` gives back.
    /// Never allocates.
    pub fn deliver(&mut self, file: u32, ev: &[u8]) -> Delivery {
        let len = ev.len();
        let header_len = ev.get(4..8).and_then(|b| b.try_into().ok()).map(u32::from_ne_bytes);
        let Some(f) = self.files.get_mut(file as usize).and_then(|f| f.as_deref_mut()) else {
            self.stats.dropped += 1;
            return Delivery::Dropped;
        };
        if !(HEADER..=EVENT_SPACE).contains(&len) || header_len != Some(len as u32) {
            self.stats.refused += 1;
            return Delivery::Refused;
        }
        if f.closing {
            let n = len.min(f.reserved);
            f.reserved -= n;
            let master = f.master;
            self.uncharge(n, master);
            self.stats.dropped += 1;
            return Delivery::Dropped;
        }
        if len > f.reserved {
            self.stats.refused += 1;
            return Delivery::Refused;
        }
        // Fits: reserved plus queued never exceed the ring.
        f.reserved -= len;
        f.ring.push(ev);
        self.stats.delivered += 1;
        Delivery::Queued
    }

    /// Whether the file has an event to read.
    pub fn readable(&self, file: u32) -> bool {
        self.file(file).is_some_and(|f| f.ring.len > 0)
    }

    /// Takes the file's read turn and holds its first event for a reader
    /// whose buffer has `room` bytes left. Only [`ReadStep::Event`] takes
    /// the turn; [`LkmsEvents::end_read`] gives it back.
    pub fn begin_read(&mut self, file: u32, room: usize) -> ReadStep {
        let Some(f) = self.file_mut(file) else { return ReadStep::Empty };
        if f.reading {
            return ReadStep::Busy;
        }
        if f.ring.len < HEADER {
            return ReadStep::Empty;
        }
        let len = f.ring.u32_at(4) as usize;
        if len > room {
            return ReadStep::TooSmall;
        }
        f.reading = true;
        ReadStep::Event(EventRef { pieces: f.ring.pieces(len), len })
    }

    /// Gives the read turn back: with `Some(len)` the event held was
    /// copied out and leaves the queue, its space back in the budget;
    /// with `None` it stays first.
    pub fn end_read(&mut self, file: u32, consumed: Option<usize>) {
        let Some(f) = self.file_mut(file) else { return };
        f.reading = false;
        let Some(n) = consumed else { return };
        let n = n.min(f.ring.len);
        f.ring.pop(n);
        let master = f.master;
        self.uncharge(n, master);
    }
}

/// The number of events in a ring (each starts with its length).
fn count_events(r: &Ring) -> u64 {
    let (mut off, mut n) = (0usize, 0u64);
    while off + HEADER <= r.len {
        let len = r.u32_at(off + 4) as usize;
        if len < HEADER {
            break;
        }
        off += len;
        n += 1;
    }
    n
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::vec;
    use std::vec::Vec;

    fn ctx() -> Sleepable {
        // SAFETY: host tests run in an ordinary thread.
        unsafe { Sleepable::assume() }
    }

    /// An encoded event of `len` bytes: type, length, then `fill`.
    fn ev(typ: u32, len: usize, fill: u8) -> Vec<u8> {
        let mut v = vec![fill; len];
        v[0..4].copy_from_slice(&typ.to_ne_bytes());
        v[4..8].copy_from_slice(&(len as u32).to_ne_bytes());
        v
    }

    fn opened(files: &[u32]) -> LkmsEvents {
        let mut e = LkmsEvents::new(&ctx());
        for &f in files {
            assert!(e.open(f, FileQueue::new(false, &ctx())).is_ok());
        }
        e
    }

    /// Reads as `drm_read()` does into a buffer of `room` bytes: whole
    /// events only, a too-small buffer gets nothing.
    fn read(e: &mut LkmsEvents, file: u32, mut room: usize) -> Vec<Vec<u8>> {
        let mut out = Vec::new();
        loop {
            match e.begin_read(file, room) {
                ReadStep::Event(r) => {
                    let mut b = Vec::new();
                    for (p, n) in r.pieces {
                        // SAFETY: the pieces are valid while the turn is held.
                        b.extend_from_slice(unsafe { core::slice::from_raw_parts(p, n) });
                    }
                    assert_eq!(b.len(), r.len);
                    e.end_read(file, Some(r.len));
                    room -= r.len;
                    out.push(b);
                }
                ReadStep::Busy => panic!("no other reader"),
                ReadStep::Empty | ReadStep::TooSmall => return out,
            }
        }
    }

    #[test]
    fn budget_reserve_unreserve_deliver_read() {
        let mut e = opened(&[1]);
        assert_eq!(e.reserve(1, 0), Err(Errno::EINVAL));
        assert_eq!(e.reserve(1, EVENT_SPACE + 1), Err(Errno::ENOMEM));
        assert_eq!(e.reserve(2, 32), Err(Errno::EINVAL), "unknown file");
        // The whole budget, in 128 events of 32 bytes.
        for _ in 0..EVENT_SPACE / 32 {
            e.reserve(1, 32).unwrap();
        }
        assert_eq!(e.reserve(1, 1), Err(Errno::ENOMEM));
        e.unreserve(1, 32);
        e.reserve(1, 24).unwrap();
        assert_eq!(e.reserve(1, 9), Err(Errno::ENOMEM));
        assert_eq!(e.stats().charged, EVENT_SPACE - 8);
        // Delivered events keep their space charged until read.
        assert_eq!(e.deliver(1, &ev(1, 32, 0xa1)), Delivery::Queued);
        assert_eq!(e.deliver(1, &ev(2, 24, 0xa2)), Delivery::Queued);
        assert!(e.readable(1));
        assert_eq!(e.reserve(1, 9), Err(Errno::ENOMEM));
        let got = read(&mut e, 1, 4096);
        assert_eq!(got, [ev(1, 32, 0xa1), ev(2, 24, 0xa2)]);
        assert!(!e.readable(1));
        e.reserve(1, 9 + 56 - 8).unwrap();
        // Giving back more than was reserved gives back what there is.
        e.unreserve(1, usize::MAX);
        assert_eq!(e.stats().charged, 0);
        assert_eq!(e.stats().delivered, 2);
    }

    #[test]
    fn malformed_and_unreserved_deliveries_are_refused() {
        let mut e = opened(&[3]);
        e.reserve(3, 32).unwrap();
        assert_eq!(e.deliver(3, &ev(1, 64, 0)), Delivery::Refused, "more than reserved");
        assert_eq!(e.deliver(3, &[0u8; 4]), Delivery::Refused, "shorter than the header");
        let mut bad = ev(1, 32, 0);
        bad[4] = 40;
        assert_eq!(e.deliver(3, &bad), Delivery::Refused, "length field differs");
        assert_eq!(e.deliver(3, &vec![0u8; EVENT_SPACE + 8]), Delivery::Refused);
        assert_eq!(e.stats().refused, 4);
        assert!(!e.readable(3));
        assert_eq!(e.deliver(3, &ev(1, 32, 7)), Delivery::Queued);
        assert_eq!(e.deliver(9, &ev(1, 32, 7)), Delivery::Dropped, "unknown file");
    }

    #[test]
    fn partial_reads_return_whole_events() {
        let mut e = opened(&[4]);
        for (i, len) in [32usize, 48, 32].into_iter().enumerate() {
            e.reserve(4, len).unwrap();
            assert_eq!(e.deliver(4, &ev(i as u32, len, i as u8)), Delivery::Queued);
        }
        // Too small for the first event: nothing, and nothing consumed.
        assert!(matches!(e.begin_read(4, 31), ReadStep::TooSmall));
        // Room for one and a half: one.
        assert_eq!(read(&mut e, 4, 32 + 40), [ev(0, 32, 0)]);
        // A failed copy keeps the event first.
        let ReadStep::Event(r) = e.begin_read(4, 100) else { panic!() };
        assert_eq!(r.len, 48);
        assert!(matches!(e.begin_read(4, 100), ReadStep::Busy), "one reader at a time");
        e.end_read(4, None);
        assert_eq!(read(&mut e, 4, 4096), [ev(1, 48, 1), ev(2, 32, 2)]);
        assert!(matches!(e.begin_read(4, 4096), ReadStep::Empty));
        assert_eq!(e.stats().charged, 0);
    }

    #[test]
    fn ring_wraps_around() {
        let mut e = opened(&[5]);
        // Leave the head near the end of the ring, then wrap events of
        // odd sizes around it many times.
        let mut seq = 0u8;
        for round in 0..200usize {
            let len = HEADER + (round * 37) % 600;
            let n = 1 + round % 5;
            let mut sent = Vec::new();
            for _ in 0..n {
                e.reserve(5, len).unwrap();
                let x = ev(round as u32, len, seq);
                seq = seq.wrapping_add(1);
                assert_eq!(e.deliver(5, &x), Delivery::Queued);
                sent.push(x);
            }
            assert_eq!(read(&mut e, 5, usize::MAX), sent, "round {round}");
        }
        assert_eq!(e.stats().charged, 0);
    }

    #[test]
    fn closing_file_drops_events_and_gives_back_space() {
        let mut e = opened(&[6, 7]);
        e.reserve(6, 32).unwrap();
        e.reserve(6, 32).unwrap();
        e.reserve(6, 64).unwrap();
        assert_eq!(e.deliver(6, &ev(1, 32, 1)), Delivery::Queued);
        e.reserve(7, 32).unwrap();
        e.begin_close(6);
        assert!(!e.readable(6), "queued events are dropped at once");
        assert_eq!(e.reserve(6, 8), Err(Errno::ENOMEM), "no reservations while closing");
        // A delivery while closing (an interrupt racing the close) is
        // dropped and its space given back; so is an unreserve.
        assert_eq!(e.deliver(6, &ev(1, 32, 2)), Delivery::Dropped);
        e.unreserve(6, 32);
        assert_eq!(e.stats().charged, 32 + 32, "one 32-byte reservation of 6 left, 7's");
        // The close gives back what the KMS code never did.
        assert!(e.close(6).is_some());
        assert_eq!(e.stats().charged, 32);
        assert_eq!(e.deliver(6, &ev(1, 32, 3)), Delivery::Dropped);
        assert_eq!(e.stats().dropped, 3);
        // The clone ID can be used again.
        assert!(e.open(6, FileQueue::new(false, &ctx())).is_ok());
        assert!(e.open(6, FileQueue::new(false, &ctx())).is_err());
        assert!(e.open(MAX_FILES + 1, FileQueue::new(false, &ctx())).is_err());
    }

    #[test]
    fn device_bound_exempts_the_master() {
        let per_file = EVENT_SPACE;
        let n = MAX_DEVICE_BYTES / per_file;
        let files: Vec<u32> = (1..=n as u32 + 2).collect();
        let mut e = opened(&files);
        for &f in &files[..n] {
            e.reserve(f, per_file).unwrap();
        }
        let other = files[n];
        assert_eq!(e.reserve(other, 32), Err(Errno::ENOMEM), "the device's bound");
        let master = files[n + 1];
        e.set_master(master, true);
        e.reserve(master, per_file).unwrap();
        assert_eq!(e.stats().charged_others, MAX_DEVICE_BYTES);
        // A master that drops the role counts against the bound again.
        e.set_master(master, false);
        assert_eq!(e.stats().charged_others, MAX_DEVICE_BYTES + per_file);
        e.unreserve(master, per_file);
        e.unreserve(files[0], per_file);
        assert_eq!(e.reserve(other, 32), Ok(()));
    }
}
