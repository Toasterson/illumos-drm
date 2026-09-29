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
//! `DROP_MASTER`. Unknown commands fail with `ENOTTY`, and core commands
//! from a 32-bit process fail with `ENOTSUP`, because only the LP64 ABI is
//! supported.
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
//! The crate is a `no_std` library on top of the `ddi` crate and is linked
//! statically into each driver module; there is no separate `misc/drm`
//! module yet (see the README for why). A driver implements [`Driver`],
//! uses [`Device`] as the instance type of its `ddi::Driver`, and maps
//! minor numbers with [`minor::instance_of_minor`]:
//!
//! ```ignore
//! ddi::module! { driver: MyModule, ops: [char, devmap] }
//!
//! impl ddi::Driver for MyModule {
//!     type Instance = drm::Device<MyDriver>;
//!     const LINKINFO: &'static CStr = c"my DRM driver";
//!     fn attach(node: OwnNode, ctx: &Sleepable) -> Result<Self::Instance, DdiError> {
//!         drm::Device::attach(node, MyDriver::new(), ctx)
//!     }
//!     fn detach(inst: Self::Instance, _dev: DevInfo, ctx: &Sleepable)
//!         -> Result<(), (Self::Instance, DdiError)> {
//!         inst.detach(ctx)
//!     }
//!     fn instance_of_minor(m: Minor) -> Option<Instance> {
//!         drm::minor::instance_of_minor(m)
//!     }
//! }
//! ```
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

pub mod caps;
pub mod defer;
#[cfg(not(test))]
mod device;
pub mod dmaerr;
pub mod driver;
pub mod dumb;
pub mod fence;
#[cfg(not(test))]
pub mod gem;
pub mod idmap;
pub mod ioctl;
pub mod karc;
mod kmem;
pub mod kms;
pub mod ksync;
pub mod kvec;
pub mod minor;
pub mod objects;
pub mod offsets;
pub mod resv;
pub mod state;
#[cfg(not(test))]
pub mod uaccess;
pub mod wwmutex;

#[cfg(not(test))]
pub use device::Device;
pub use driver::{BusId, Driver, Features, ModeConfig};
#[cfg(not(test))]
pub use gem::{GemDma, GemObject, GemRef, SysMem};
