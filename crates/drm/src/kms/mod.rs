// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Kernel mode setting (milestone R3).

pub mod atomic;
pub mod core;
pub mod edid;
pub mod events;
pub mod formats;
pub mod legacy;
pub mod modes;
pub mod objects;
pub mod props;
pub mod state;
#[cfg(test)]
mod tests;
