// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The fake mmap offsets of GEM objects, the counterpart of Linux's
//! `drm_vma_manager`.
//!
//! A process maps a GEM object by calling `mmap(2)` on its DRM file with
//! an offset that the kernel handed out for the object (`MODE_MAP_DUMB`
//! and the driver-specific map ioctls return it). Each object gets one
//! page-aligned range of offsets, as long as the object, when it is first
//! needed, and keeps it until the object is destroyed. The ranges come from
//! a [`RangeAlloc`]; in the kernel that is a `vmem(9F)` arena over
//! [`OFFSET_START`]..[`OFFSET_START`]` + `[`OFFSET_SIZE`], which is the
//! range Linux uses on 64-bit systems (`DRM_FILE_PAGE_OFFSET_START` and
//! `DRM_FILE_PAGE_OFFSET_SIZE`): from 256 MiB up, 64 GiB of offsets. The
//! range below 256 MiB stays free for anything the card and render nodes
//! may map themselves; offsets there are never GEM objects.
//!
//! An [`OffsetMap`] records which object owns which range. `mmap(2)` must
//! start exactly at an object's offset and may not extend beyond the
//! object, as with Linux's `drm_vma_offset_exact_lookup_locked` and the
//! size check of `drm_gem_mmap_obj`.

use alloc::collections::BTreeMap;

use ddi::{Errno, Sleepable};

/// The page size the offsets are counted in: the amd64 MMU page. The
/// device checks it against `ptob(1)` when it attaches.
pub const PAGE_SIZE: u64 = 4096;
/// The first GEM offset, `DRM_FILE_PAGE_OFFSET_START` pages: 256 MiB.
pub const OFFSET_START: u64 = ((0x0fff_ffff >> 12) + 1) * PAGE_SIZE;
/// The size of the GEM offset space, `DRM_FILE_PAGE_OFFSET_SIZE` pages:
/// just under 64 GiB.
pub const OFFSET_SIZE: u64 = (0x0fff_ffff >> 12) * 256 * PAGE_SIZE;

/// A source of page-aligned ranges.
pub trait RangeAlloc {
    /// Returns the start of a free range of `len` bytes, a multiple of
    /// [`PAGE_SIZE`], or `None` if no such range is free or the allocator
    /// cannot get memory without blocking.
    fn alloc(&mut self, len: u64) -> Option<u64>;
    /// Returns a range obtained from [`RangeAlloc::alloc`].
    fn free(&mut self, start: u64, len: u64);
}

/// Rounds `len` up to whole pages; `None` for 0 or on overflow.
pub fn page_round(len: u64) -> Option<u64> {
    if len == 0 {
        return None;
    }
    len.checked_add(PAGE_SIZE - 1).map(|l| l & !(PAGE_SIZE - 1))
}

/// Offset ranges and their owners. See the module documentation.
pub struct OffsetMap<V, A: RangeAlloc> {
    map: BTreeMap<u64, (u64, V)>,
    ranges: A,
}

impl<V, A: RangeAlloc> OffsetMap<V, A> {
    /// An empty map drawing ranges from `ranges`.
    pub const fn new(ranges: A) -> OffsetMap<V, A> {
        OffsetMap { map: BTreeMap::new(), ranges }
    }

    /// Gives `v` a range of `len` bytes, rounded up to whole pages, and
    /// returns its start. Fails with `EINVAL` for an empty or impossibly
    /// large length and with `ENOSPC`, as Linux's `drm_mm` does, when no
    /// range of that size is free.
    pub fn insert(&mut self, len: u64, v: V, _ctx: &Sleepable) -> Result<u64, (V, Errno)> {
        let Some(len) = page_round(len).filter(|l| *l <= OFFSET_SIZE) else { return Err((v, Errno::EINVAL)) };
        let Some(start) = self.ranges.alloc(len) else { return Err((v, Errno::ENOSPC)) };
        let valid = start.is_multiple_of(PAGE_SIZE)
            && start >= OFFSET_START
            && start.checked_add(len).is_some_and(|end| end <= OFFSET_START + OFFSET_SIZE)
            && self.overlapping(start, len).is_none();
        if !valid {
            // The allocator broke its contract; keep the range out of use.
            return Err((v, Errno::EINVAL));
        }
        self.map.insert(start, (len, v));
        Ok(start)
    }

    fn overlapping(&self, start: u64, len: u64) -> Option<u64> {
        let before = self.map.range(..start.saturating_add(len)).next_back();
        match before {
            Some((&s, &(l, _))) if s.saturating_add(l) > start => Some(s),
            _ => None,
        }
    }

    /// Removes the range that starts at `start` and returns its value.
    pub fn remove(&mut self, start: u64) -> Option<V> {
        let (len, v) = self.map.remove(&start)?;
        self.ranges.free(start, len);
        Some(v)
    }

    /// Finds the owner of a mapping of `len` bytes at `off`: the range must
    /// start exactly at `off` and be at least `len` bytes, rounded up to
    /// whole pages, long. Returns the value and the length of its range.
    pub fn lookup_exact(&self, off: u64, len: u64) -> Option<(&V, u64)> {
        let (range_len, v) = self.map.get(&off)?;
        let want = page_round(len)?;
        if want > *range_len {
            return None;
        }
        Some((v, *range_len))
    }

    /// Returns the number of ranges in use.
    pub fn len(&self) -> usize {
        self.map.len()
    }

    /// Reports whether no range is in use.
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }
}

/// A first-fit range allocator over the GEM offset space, the host model
/// of the vmem arena.
#[cfg(test)]
pub struct FirstFitRanges {
    used: BTreeMap<u64, u64>,
}

#[cfg(test)]
impl FirstFitRanges {
    /// The whole GEM offset space, free.
    pub fn new() -> FirstFitRanges {
        FirstFitRanges { used: BTreeMap::new() }
    }
}

#[cfg(test)]
impl RangeAlloc for FirstFitRanges {
    fn alloc(&mut self, len: u64) -> Option<u64> {
        let mut at = OFFSET_START;
        for (&s, &l) in &self.used {
            if s >= at + len {
                break;
            }
            at = at.max(s + l);
        }
        if at + len > OFFSET_START + OFFSET_SIZE {
            return None;
        }
        self.used.insert(at, len);
        Some(at)
    }
    fn free(&mut self, start: u64, len: u64) {
        assert_eq!(self.used.remove(&start), Some(len), "freeing a range that is not in use");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ctx() -> Sleepable {
        // SAFETY: host tests may block.
        unsafe { Sleepable::assume() }
    }

    #[test]
    fn linux_offset_space() {
        // DRM_FILE_PAGE_OFFSET_START and _SIZE on a 64-bit Linux with 4 KiB
        // pages.
        assert_eq!(OFFSET_START, 0x1000_0000);
        assert_eq!(OFFSET_SIZE / PAGE_SIZE, 0xffff * 256);
    }

    #[test]
    fn page_rounding() {
        assert_eq!(page_round(0), None);
        assert_eq!(page_round(1), Some(4096));
        assert_eq!(page_round(4096), Some(4096));
        assert_eq!(page_round(4097), Some(8192));
        assert_eq!(page_round(u64::MAX), None);
        assert_eq!(page_round(u64::MAX - 4095), Some(u64::MAX - 4095));
    }

    #[test]
    fn exact_lookup_and_reuse() {
        let ctx = ctx();
        let mut m = OffsetMap::new(FirstFitRanges::new());
        let a = m.insert(4096 * 3, 'a', &ctx).ok().unwrap();
        let b = m.insert(100, 'b', &ctx).ok().unwrap();
        assert_eq!(a, OFFSET_START);
        assert_eq!(b, a + 3 * 4096, "ranges do not overlap");
        assert_eq!(m.lookup_exact(a, 4096 * 3), Some((&'a', 4096 * 3)));
        assert_eq!(m.lookup_exact(a, 1), Some((&'a', 4096 * 3)), "shorter mappings are allowed");
        assert_eq!(m.lookup_exact(a, 4096 * 3 + 1), None, "not beyond the object");
        assert_eq!(m.lookup_exact(a + 4096, 4096), None, "only at the start");
        assert_eq!(m.lookup_exact(b, 4096), Some((&'b', 4096)));
        assert_eq!(m.lookup_exact(b, 0), None);
        assert_eq!(m.lookup_exact(0, 4096), None, "nothing below OFFSET_START");
        assert_eq!(m.remove(a), Some('a'));
        assert_eq!(m.remove(a), None);
        assert_eq!(m.lookup_exact(a, 4096), None);
        assert_eq!(m.insert(4096, 'c', &ctx).ok(), Some(a), "freed ranges are reused");
        assert_eq!(m.len(), 2);
    }

    #[test]
    fn refuses_bad_lengths_and_exhaustion() {
        let ctx = ctx();
        let mut m = OffsetMap::new(FirstFitRanges::new());
        assert_eq!(m.insert(0, 'x', &ctx).err(), Some(('x', Errno::EINVAL)));
        assert_eq!(m.insert(u64::MAX, 'x', &ctx).err(), Some(('x', Errno::EINVAL)));
        assert_eq!(m.insert(OFFSET_SIZE + 1, 'x', &ctx).err(), Some(('x', Errno::EINVAL)));
        assert!(m.insert(OFFSET_SIZE - 4096, 'y', &ctx).is_ok());
        assert_eq!(m.insert(8192, 'z', &ctx).err(), Some(('z', Errno::ENOSPC)));
        assert!(m.insert(4096, 'w', &ctx).is_ok());
        assert!(!m.is_empty() && m.len() == 2);
    }

    /// An allocator that hands out a range overlapping one in use.
    struct Overlapping(u64);
    impl RangeAlloc for Overlapping {
        fn alloc(&mut self, _len: u64) -> Option<u64> {
            let s = self.0;
            self.0 += 4096;
            Some(s)
        }
        fn free(&mut self, _: u64, _: u64) {}
    }

    #[test]
    fn refuses_overlaps_from_a_broken_allocator() {
        let ctx = ctx();
        let mut m = OffsetMap::new(Overlapping(OFFSET_START));
        assert!(m.insert(8192, 1, &ctx).is_ok());
        assert_eq!(m.insert(4096, 2, &ctx).err(), Some((2, Errno::EINVAL)));
        let mut m = OffsetMap::new(Overlapping(1));
        assert_eq!(m.insert(4096, 3, &ctx).err(), Some((3, Errno::EINVAL)), "unaligned, below the space");
    }
}
