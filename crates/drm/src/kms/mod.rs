// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Kernel mode setting (milestone R3).

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
