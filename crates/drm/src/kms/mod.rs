// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Kernel mode setting (milestone R3).
//!
//! A driver with display hardware implements [`KmsDriver`]: it describes
//! its pipeline ([`Layout`]: planes, CRTCs, encoders, connectors), probes
//! connectors ([`Probe`], usually with an EDID that [`edid`] parses), and
//! checks and applies object states in hooks. The core does the rest of
//! what the uAPI promises: object IDs and properties ([`objects`],
//! [`props`]), framebuffers and blobs, atomic updates with Linux's checks
//! and errnos ([`atomic`], [`state`]), the legacy ioctls as atomic updates
//! ([`legacy`]), vblank counters and events ([`events`]), and the ioctl
//! layer with `read(2)` and `poll(2)` (`dev`, kernel only). [`modes`] and
//! [`formats`] hold the mode and pixel-format tables. `soft` (kernel only)
//! is a software vblank source for hardware without a vblank interrupt.
//!
//! The README's "Mode setting (R3)" sections give the decisions and the
//! differences from Linux.

pub mod atomic;
pub mod core;
#[cfg(not(test))]
pub mod dev;
pub mod edid;
pub mod events;
pub mod formats;
pub mod ioctl;
pub mod legacy;
pub mod modes;
pub mod objects;
pub mod props;
#[cfg(not(test))]
pub mod soft;
pub mod state;
#[cfg(test)]
mod tests;

#[cfg(not(test))]
pub use dev::{Kms, VblankHandle};
pub use objects::{ConnectorDesc, CrtcDesc, EncoderDesc, Framebuffer, Layout, PlaneDesc, PlaneType};
pub use state::{
    ConnectorState, ConnectorStatus, CrtcState, FbBacking, KmsDriver, PlaneState, Probe, StateView, check_plane_helper,
};
