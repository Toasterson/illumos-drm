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
//! The crate is a `no_std` library on top of the `ddi` crate and is linked
//! statically into each driver module; there is no separate `misc/drm`
//! module yet (see the README for why). A driver implements [`Driver`],
//! uses [`Device`] as the instance type of its `ddi::Driver`, and maps
//! minor numbers with [`minor::instance_of_minor`]:
//!
//! ```ignore
//! ddi::module! { driver: MyModule, ops: [char] }
//!
//! impl ddi::Driver for MyModule {
//!     type Instance = drm::Device<MyDriver>;
//!     const LINKINFO: &'static CStr = c"my DRM driver";
//!     fn attach(dev: DevInfo, ctx: &Sleepable) -> Result<Self::Instance, DdiError> {
//!         drm::Device::attach(dev, MyDriver::new(), ctx)
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
//! [`minor`]) is plain data and is unit-tested on the host; [`Device`] is
//! the thin kernel layer that copies arguments and holds the lock.
#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]
#![warn(missing_docs)]

pub mod caps;
#[cfg(not(test))]
mod device;
pub mod driver;
pub mod ioctl;
pub mod kvec;
pub mod minor;
pub mod state;
#[cfg(not(test))]
pub mod uaccess;

#[cfg(not(test))]
pub use device::Device;
pub use driver::{BusId, Driver, Features, ModeConfig};
