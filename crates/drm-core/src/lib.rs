// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The DRM core for illumos.
//!
//! This crate is the part of a DRM driver that every driver shares: it
//! registers the device's card and render nodes, keeps per-file state
//! (authentication, master, client capabilities) through clone opens, and
//! answers the core ioctls that libdrm uses to find and open a device:
//! `VERSION`, `GET_CAP`, `SET_CLIENT_CAP`, `GET_UNIQUE`, `SET_UNIQUE`,
//! `SET_VERSION`, `GET_MAGIC`, `AUTH_MAGIC`, `GET_CLIENT`, `SET_MASTER` and
//! `DROP_MASTER`. A caller's argument structure may be shorter or longer
//! than the kernel's, as with Linux's `drm_ioctl()` ([`ioctl`]). Unknown
//! commands fail with `ENOTTY`, and core commands from a 32-bit process
//! fail with `ENOTSUP`, because only the LP64 ABI is supported.
//!
//! Since milestone R1 it also has GEM buffer objects ([`gem`]): per-file
//! handles, FLINK names, fake mmap offsets and `mmap(2)` through
//! `devmap(9E)`, system-memory objects from `ddi_umem_alloc(9F)` with an
//! optional DMA scatter/gather table, dumb buffers, and the ioctls
//! `GEM_CLOSE`, `GEM_FLINK`, `GEM_OPEN`, `MODE_CREATE_DUMB`,
//! `MODE_MAP_DUMB` and `MODE_DESTROY_DUMB`, plus driver-private ioctls.
//! References to objects can be dropped in interrupt context
//! ([`defer`]).
//!
//! Since milestone R3 it does kernel mode setting ([`kms`]): mode objects
//! and properties, framebuffers and blobs, atomic commits with the legacy
//! ioctls built on them, vblank counters and events read through `read(2)`
//! and `poll(2)`, and EDID parsing, for drivers that implement
//! [`kms::KmsDriver`] and return it from [`Driver::kms`].
//!
//! The handle and name tables are `alloc::collections::BTreeMap`s, so the
//! driver module must provide a global allocator: enable the
//! `global-alloc-sleep` feature of the `ddi` crate, as `rdrmnull` does. The
//! core only inserts into those maps with the `Sleepable` token; all its
//! other allocations go through [`kvec::KVec`] and [`karc::KArc`], which
//! choose `KM_SLEEP` or `KM_NOSLEEP` per call.
//!
//! The crate is a `no_std` library on top of the `ddi` crate. Since R5a
//! exactly one kernel module compiles it with its `kernel` feature (the
//! stateful layer: devices, GEM, fences, sync objects, PRIME, the table of
//! device numbers in [`minor`] and [`slots`]): `misc/drm_core`
//! (`modules/drm_core`), which instantiates [`Device`] over its facade
//! driver and exports the C facade of `include/drm`. Drivers do not use
//! this crate's kernel layer: Rust drivers go through the `drm-facade`
//! crate, C drivers through the headers, and both are linked with
//! `-N misc/drm_core`. Without the `kernel` feature the crate offers the
//! value helpers drivers may share (display modes, dumb-buffer layouts).
//! The [`Driver`] trait is the interface between the core and
//! `misc/drm_core`'s facade driver.
//!
//! The logic that decides things ([`state`], [`caps`], [`ioctl`],
//! [`minor`], [`objects`], [`idmap`], [`offsets`], [`dumb`], [`dmaerr`], [`karc`],
//! [`defer`]) is plain data and is unit-tested on the host, several parts
//! from many threads; [`Device`] and [`gem`] are the kernel layer that
//! copies arguments, holds the locks and calls the DDI.
#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]
#![warn(missing_docs)]

extern crate alloc;

#[cfg(feature = "kernel")]
pub mod anon_fd;
#[cfg(feature = "kernel")]
pub mod caps;
pub mod defer;
#[cfg(not(test))]
#[cfg(feature = "kernel")]
mod device;
#[cfg(feature = "kernel")]
pub mod dmaerr;
#[cfg(feature = "kernel")]
pub mod driver;
pub mod dumb;
#[cfg(feature = "kernel")]
pub mod fence;
#[cfg(not(test))]
#[cfg(feature = "kernel")]
pub mod fence_dev;
#[cfg(not(test))]
#[cfg(feature = "kernel")]
pub mod gem;
#[cfg(feature = "kernel")]
pub mod idmap;
#[cfg(feature = "kernel")]
pub mod ioctl;
pub mod karc;
mod kmem;
pub mod kms;
pub mod ksync;
pub mod kvec;
#[cfg(feature = "kernel")]
pub mod minor;
#[cfg(feature = "kernel")]
pub mod objects;
pub mod offsets;
#[cfg(not(test))]
#[cfg(feature = "kernel")]
pub mod prime;
pub mod published;
#[cfg(feature = "kernel")]
pub mod resv;
#[cfg(feature = "kernel")]
pub mod slots;
#[cfg(feature = "kernel")]
pub mod state;
#[cfg(feature = "kernel")]
pub mod sync_file;
#[cfg(feature = "kernel")]
pub mod syncobj;
#[cfg(not(test))]
#[cfg(feature = "kernel")]
pub mod syncobj_ioctl;
#[cfg(not(test))]
#[cfg(feature = "kernel")]
pub mod uaccess;
#[cfg(feature = "kernel")]
pub mod wwmutex;

#[cfg(all(feature = "kernel", not(test)))]
pub use device::{Device, numbers_taken};
#[cfg(feature = "kernel")]
pub use driver::{BusId, Driver, DriverInfo, DumbCreated, Features, ModeConfig};
#[cfg(all(feature = "kernel", not(test)))]
pub use gem::{GemDma, GemObject, GemRef, SysMem};
