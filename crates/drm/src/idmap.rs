// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Numbered tables: GEM handles per file and FLINK names per device.
//!
//! An [`IdMap`] hands out numbers from an [`IdAlloc`] and maps them to
//! values in an `alloc::collections::BTreeMap`. In the kernel the numbers
//! come from `id_space(9F)` with first-fit allocation, so that, as with
//! Linux's `idr_alloc(…, 1, 0, …)`, the lowest free number is used and
//! numbers start at 1; 0 is never a valid handle or name. On the host the
//! tests use `FirstFit`, a model with the same behaviour.
//!
//! The map is not locked; its owner keeps it behind a mutex. Inserting may
//! allocate a tree node, which in the kernel comes from the global
//! allocator of the `ddi` crate (`global-alloc-sleep`, always `KM_SLEEP`),
//! so [`IdMap::insert`] demands the [`Sleepable`] token. Removing only
//! frees memory, which is allowed in any context.

use alloc::collections::BTreeMap;

use ddi::{Errno, Sleepable};

/// A source of unique numbers.
pub trait IdAlloc {
    /// Returns the lowest free number, or `None` if the space is exhausted
    /// or the allocator cannot get memory without blocking.
    fn alloc(&mut self) -> Option<u32>;
    /// Returns a number obtained from [`IdAlloc::alloc`].
    fn free(&mut self, id: u32);
}

/// Numbers mapped to values. See the module documentation.
pub struct IdMap<V, A: IdAlloc> {
    map: BTreeMap<u32, V>,
    ids: A,
}

impl<V, A: IdAlloc> IdMap<V, A> {
    /// An empty map drawing numbers from `ids`.
    pub const fn new(ids: A) -> IdMap<V, A> {
        IdMap { map: BTreeMap::new(), ids }
    }

    /// Stores `v` under a new number and returns the number. Fails with
    /// `ENOMEM`, handing the value back, if no number is available.
    pub fn insert(&mut self, v: V, _ctx: &Sleepable) -> Result<u32, (V, Errno)> {
        let Some(id) = self.ids.alloc() else { return Err((v, Errno::ENOMEM)) };
        if id == 0 || self.map.contains_key(&id) {
            // The allocator broke its contract. Do not overwrite an entry and
            // do not free a number that may be in use.
            return Err((v, Errno::EINVAL));
        }
        self.map.insert(id, v);
        Ok(id)
    }

    /// Returns the value stored under `id`.
    pub fn get(&self, id: u32) -> Option<&V> {
        self.map.get(&id)
    }

    /// Removes the value stored under `id` and frees the number.
    pub fn remove(&mut self, id: u32) -> Option<V> {
        let v = self.map.remove(&id)?;
        self.ids.free(id);
        Some(v)
    }

    /// Returns the number of entries.
    pub fn len(&self) -> usize {
        self.map.len()
    }

    /// Reports whether the map is empty.
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }

    /// Iterates over the entries in number order.
    pub fn iter(&self) -> impl Iterator<Item = (u32, &V)> {
        self.map.iter().map(|(k, v)| (*k, v))
    }

    /// Removes the entry with the lowest number and frees the number; used
    /// to empty the map one entry at a time without holding its lock across
    /// the release of each value.
    pub fn pop_first(&mut self) -> Option<(u32, V)> {
        let (id, v) = self.map.pop_first()?;
        self.ids.free(id);
        Some((id, v))
    }
}

/// A first-fit number allocator over `[low, high)`, the host model of
/// `id_allocff_nosleep(9F)`.
#[cfg(test)]
pub struct FirstFit {
    used: alloc::collections::BTreeSet<u32>,
    low: u32,
    high: u32,
}

#[cfg(test)]
impl FirstFit {
    /// Numbers from `low` (inclusive) to `high` (exclusive).
    pub fn new(low: u32, high: u32) -> FirstFit {
        FirstFit { used: alloc::collections::BTreeSet::new(), low, high }
    }
}

#[cfg(test)]
impl IdAlloc for FirstFit {
    fn alloc(&mut self) -> Option<u32> {
        let mut n = self.low;
        for &u in self.used.range(self.low..) {
            if u != n {
                break;
            }
            n = n.checked_add(1)?;
        }
        if n >= self.high {
            return None;
        }
        self.used.insert(n);
        Some(n)
    }
    fn free(&mut self, id: u32) {
        assert!(self.used.remove(&id), "freeing a number that is not in use");
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
    fn lowest_free_number_from_one() {
        let ctx = ctx();
        let mut m = IdMap::new(FirstFit::new(1, 4));
        assert_eq!(m.insert('a', &ctx).ok(), Some(1));
        assert_eq!(m.insert('b', &ctx).ok(), Some(2));
        assert_eq!(m.insert('c', &ctx).ok(), Some(3));
        assert_eq!(m.insert('d', &ctx).err().map(|e| e.1), Some(Errno::ENOMEM), "space exhausted");
        assert_eq!(m.remove(2), Some('b'));
        assert_eq!(m.remove(2), None);
        assert_eq!(m.get(0), None);
        assert_eq!(m.insert('e', &ctx).ok(), Some(2), "reused, as idr_alloc does");
        assert_eq!(m.len(), 3);
        assert_eq!(m.pop_first(), Some((1, 'a')));
        assert_eq!(m.iter().map(|(k, _)| k).collect::<alloc::vec::Vec<_>>(), [2, 3]);
        assert_eq!(m.insert('f', &ctx).ok(), Some(1));
    }

    struct Broken;
    impl IdAlloc for Broken {
        fn alloc(&mut self) -> Option<u32> {
            Some(0)
        }
        fn free(&mut self, _: u32) {
            panic!("must not free what it never handed out");
        }
    }

    #[test]
    fn refuses_a_broken_allocator() {
        let mut m = IdMap::new(Broken);
        assert_eq!(m.insert(1, &ctx()).err(), Some((1, Errno::EINVAL)));
        assert!(m.is_empty());
    }
}
