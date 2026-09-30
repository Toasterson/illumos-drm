// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The table of device numbers: one atomic word per number, holding its
//! owner or 0.

use core::sync::atomic::{AtomicUsize, Ordering};

/// The number of device numbers, 0 to 63.
pub const COUNT: usize = 64;

/// The table.
pub struct Slots {
    owners: [AtomicUsize; COUNT],
}

impl Default for Slots {
    fn default() -> Slots {
        Slots::new()
    }
}

impl Slots {
    /// An empty table.
    pub const fn new() -> Slots {
        Slots { owners: [const { AtomicUsize::new(0) }; COUNT] }
    }

    /// Takes the lowest number that is free when it is examined for
    /// `owner` (non-zero). Concurrent callers each get a different number;
    /// `None` if all are taken or `owner` is 0.
    pub fn alloc(&self, owner: usize) -> Option<u32> {
        if owner == 0 {
            return None;
        }
        for (i, slot) in self.owners.iter().enumerate() {
            if slot.compare_exchange(0, owner, Ordering::AcqRel, Ordering::Relaxed).is_ok() {
                return u32::try_from(i).ok();
            }
        }
        None
    }

    /// Gives number `id` back if `owner` (non-zero) holds it; `false`, and
    /// no change, otherwise.
    pub fn free(&self, id: u32, owner: usize) -> bool {
        let Some(slot) = usize::try_from(id).ok().and_then(|i| self.owners.get(i)) else { return false };
        owner != 0 && slot.compare_exchange(owner, 0, Ordering::AcqRel, Ordering::Relaxed).is_ok()
    }

    /// The owner of number `id`, if it is taken.
    pub fn owner(&self, id: u32) -> Option<usize> {
        let o = self.owners.get(usize::try_from(id).ok()?)?.load(Ordering::Acquire);
        if o == 0 { None } else { Some(o) }
    }

    /// How many numbers are taken.
    pub fn taken(&self) -> u32 {
        self.owners.iter().filter(|s| s.load(Ordering::Acquire) != 0).count() as u32
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeSet;
    use std::sync::{Arc, Barrier};
    use std::thread;

    #[test]
    fn lowest_free_and_reuse() {
        let s = Slots::new();
        assert_eq!(s.alloc(0x10), Some(0));
        assert_eq!(s.alloc(0x20), Some(1));
        assert_eq!(s.alloc(0x30), Some(2));
        assert!(s.free(1, 0x20));
        // The freed number is the lowest free one again, as after an
        // unload and reload.
        assert_eq!(s.alloc(0x40), Some(1));
        assert_eq!(s.owner(1), Some(0x40));
        assert_eq!(s.taken(), 3);
        assert!(s.free(0, 0x10) && s.free(1, 0x40) && s.free(2, 0x30));
        assert_eq!(s.taken(), 0);
    }

    #[test]
    fn wrong_frees_change_nothing() {
        let s = Slots::new();
        assert_eq!(s.alloc(0x10), Some(0));
        assert!(!s.free(0, 0x20), "another owner");
        assert!(!s.free(1, 0x10), "a number not taken");
        assert!(!s.free(64, 0x10), "out of range");
        assert!(!s.free(u32::MAX, 0x10), "out of range");
        assert!(!s.free(0, 0), "no owner");
        assert_eq!(s.owner(0), Some(0x10));
        assert!(s.free(0, 0x10));
        assert!(!s.free(0, 0x10), "a second free");
        assert_eq!(s.alloc(0), None, "owner 0 is not an owner");
        assert_eq!(s.taken(), 0);
    }

    #[test]
    fn exhaustion() {
        let s = Slots::new();
        for i in 0..COUNT {
            assert_eq!(s.alloc(0x100 + i), Some(i as u32));
        }
        assert_eq!(s.alloc(0x1), None);
        assert!(s.free(40, 0x100 + 40));
        assert_eq!(s.alloc(0x1), Some(40));
    }

    #[test]
    fn concurrent_attach_and_detach() {
        // Eight threads attach and detach devices at once: no number is
        // ever held twice, and everything is free at the end.
        let s = Arc::new(Slots::new());
        let start = Arc::new(Barrier::new(8));
        let threads: Vec<_> = (0..8usize)
            .map(|t| {
                let (s, start) = (Arc::clone(&s), Arc::clone(&start));
                thread::spawn(move || {
                    start.wait();
                    for round in 0..2000usize {
                        let owners: Vec<usize> = (0..4).map(|k| (t << 20) | (round << 4) | (k + 1)).collect();
                        let ids: Vec<u32> =
                            owners.iter().map(|&o| s.alloc(o).expect("64 numbers for 32 devices")).collect();
                        for (&id, &o) in ids.iter().zip(&owners) {
                            assert_eq!(s.owner(id), Some(o));
                        }
                        let unique: BTreeSet<u32> = ids.iter().copied().collect();
                        assert_eq!(unique.len(), ids.len());
                        for (&id, &o) in ids.iter().zip(&owners) {
                            assert!(s.free(id, o));
                        }
                    }
                })
            })
            .collect();
        for t in threads {
            t.join().unwrap();
        }
        assert_eq!(s.taken(), 0);
    }
}
