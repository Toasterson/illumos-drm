// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! A growable array on the kernel heap whose growth never panics.
//!
//! `alloc::vec::Vec` needs a global allocator, and a global allocator has
//! to choose between `KM_SLEEP` and `KM_NOSLEEP` without knowing the
//! context; the optional one of the `ddi` crate (`global-alloc-sleep`)
//! always sleeps, which the core only accepts for its `BTreeMap` tables,
//! with the token checked at every insertion. `KVec` takes the decision
//! per call instead: [`KVec::push`] grows with
//! `KM_SLEEP` and demands the [`Sleepable`] token, [`KVec::try_push`] grows
//! with `KM_NOSLEEP` and may be used anywhere. Both report failure as a
//! value; neither can panic.
//!
//! In host unit tests the memory comes from the Rust global allocator, so
//! that the pure logic built on `KVec` can be tested with `cargo test`.

use core::mem::{align_of, size_of};
use core::ops::{Deref, DerefMut};
use core::ptr::{self, NonNull};

use ddi::{Errno, Sleepable};

use crate::kmem::{self as raw, Flag};

/// A growable array of `T` in kernel memory.
///
/// `kmem_alloc(9F)` aligns to 8 bytes, so `T` may not need more; this is
/// checked at compile time.
pub struct KVec<T> {
    ptr: NonNull<T>,
    len: usize,
    cap: usize,
}

// SAFETY: KVec owns its elements like Vec does.
unsafe impl<T: Send> Send for KVec<T> {}
// SAFETY: shared access only hands out &T.
unsafe impl<T: Sync> Sync for KVec<T> {}

impl<T> KVec<T> {
    const LAYOUT_OK: () = assert!(
        align_of::<T>() <= 8 && size_of::<T>() > 0,
        "KVec elements must be sized and need at most 8-byte alignment"
    );

    /// Creates an empty array. Nothing is allocated until the first push.
    pub const fn new() -> KVec<T> {
        let () = Self::LAYOUT_OK;
        KVec { ptr: NonNull::dangling(), len: 0, cap: 0 }
    }

    /// Returns the number of elements.
    pub fn len(&self) -> usize {
        self.len
    }

    /// Reports whether the array is empty.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Appends `value`, growing the array with `KM_SLEEP` if needed. Growth
    /// may block but does not fail for lack of memory; it fails with
    /// `ENOMEM` only if the new size would not fit in the address space.
    pub fn push(&mut self, value: T, _ctx: &Sleepable) -> Result<(), Errno> {
        self.grow_for_one(Flag::Sleep)?;
        self.write_last(value);
        Ok(())
    }

    /// Appends `value`, growing the array with `KM_NOSLEEP` if needed.
    /// Usable from any context; hands the value back if memory is short.
    pub fn try_push(&mut self, value: T) -> Result<(), T> {
        if self.grow_for_one(Flag::NoSleep).is_err() {
            return Err(value);
        }
        self.write_last(value);
        Ok(())
    }

    /// Removes the element at `index` by moving the last element into its
    /// place, and returns it. Returns `None` if `index` is out of range.
    pub fn swap_remove(&mut self, index: usize) -> Option<T> {
        if index >= self.len {
            return None;
        }
        self.len -= 1;
        // SAFETY: both indices are below the old length, so both slots are
        // initialised; the last slot is logically removed by the new length.
        unsafe {
            let base = self.ptr.as_ptr();
            let v = ptr::read(base.add(index));
            if index != self.len {
                ptr::copy_nonoverlapping(base.add(self.len), base.add(index), 1);
            }
            Some(v)
        }
    }

    fn write_last(&mut self, value: T) {
        // SAFETY: grow_for_one guaranteed cap > len.
        unsafe { ptr::write(self.ptr.as_ptr().add(self.len), value) };
        self.len += 1;
    }

    fn grow_for_one(&mut self, flag: Flag) -> Result<(), Errno> {
        if self.len < self.cap {
            return Ok(());
        }
        let new_cap = if self.cap == 0 { 4 } else { self.cap.checked_mul(2).ok_or(Errno::ENOMEM)? };
        let bytes = new_cap.checked_mul(size_of::<T>()).ok_or(Errno::ENOMEM)?;
        if bytes > isize::MAX as usize {
            return Err(Errno::ENOMEM);
        }
        let p = raw::alloc(bytes, flag) as *mut T;
        let Some(p) = NonNull::new(p) else { return Err(Errno::ENOMEM) };
        if self.cap != 0 {
            // SAFETY: the old buffer holds len initialised elements and the
            // new one has room for more; the old buffer is freed with the size
            // it was allocated with.
            unsafe {
                ptr::copy_nonoverlapping(self.ptr.as_ptr(), p.as_ptr(), self.len);
                raw::free(self.ptr.as_ptr() as *mut u8, self.cap * size_of::<T>());
            }
        }
        self.ptr = p;
        self.cap = new_cap;
        Ok(())
    }
}

impl<T> Default for KVec<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T> Deref for KVec<T> {
    type Target = [T];
    fn deref(&self) -> &[T] {
        // SAFETY: ptr is valid for len initialised elements (or dangling with
        // len 0, which is allowed for slices).
        unsafe { core::slice::from_raw_parts(self.ptr.as_ptr(), self.len) }
    }
}

impl<T> DerefMut for KVec<T> {
    fn deref_mut(&mut self) -> &mut [T] {
        // SAFETY: as in deref, and &mut self gives exclusive access.
        unsafe { core::slice::from_raw_parts_mut(self.ptr.as_ptr(), self.len) }
    }
}

impl<T> Drop for KVec<T> {
    fn drop(&mut self) {
        // SAFETY: the first len slots are initialised and dropped once; the
        // buffer is freed with the size it was allocated with.
        unsafe {
            ptr::drop_in_place(core::ptr::slice_from_raw_parts_mut(self.ptr.as_ptr(), self.len));
            if self.cap != 0 {
                raw::free(self.ptr.as_ptr() as *mut u8, self.cap * size_of::<T>());
            }
        }
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::rc::Rc;

    fn ctx() -> Sleepable {
        // SAFETY: host tests run in an ordinary thread, which may block.
        unsafe { Sleepable::assume() }
    }

    #[test]
    fn push_grow_remove_drop() {
        let ctx = ctx();
        let mut v = KVec::new();
        for i in 0..100u64 {
            v.push(i, &ctx).unwrap();
        }
        assert_eq!(v.len(), 100);
        assert_eq!(v[57], 57);
        assert_eq!(v.swap_remove(0), Some(0));
        assert_eq!(v[0], 99);
        assert_eq!(v.swap_remove(98), Some(98));
        assert_eq!(v.swap_remove(98), None);
        assert_eq!(v.len(), 98);
        assert!(v.try_push(7).is_ok());
    }

    #[test]
    fn drops_every_element_once() {
        let ctx = ctx();
        let marker = Rc::new(());
        {
            let mut v = KVec::new();
            for _ in 0..10 {
                v.push(Rc::clone(&marker), &ctx).unwrap();
            }
            drop(v.swap_remove(3));
            assert_eq!(Rc::strong_count(&marker), 10);
        }
        assert_eq!(Rc::strong_count(&marker), 1);
    }
}
