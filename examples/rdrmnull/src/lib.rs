// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! A DRM driver without hardware.
//!
//! `rdrmnull` registers one DRM device per instance of its pseudo node
//! (`rdrmnull.conf` asks for instance 0), with the primary node `card0` and
//! the render node `renderD128`, and implements nothing beyond the core
//! ioctls. It declares mode setting and atomic support so that every client
//! capability and every mode-setting capability path of the core can be
//! exercised; it has no CRTCs, planes or connectors, and the mode-setting
//! ioctls that would list them arrive with milestone R3.
//!
//! It is the kernel module the core is tested with, and the template for a
//! real driver: implement [`drm::Driver`], use [`drm::Device`] as the
//! instance, and map minor numbers with [`drm::minor::instance_of_minor`].
#![no_std]

use core::ffi::CStr;

use ddi::prelude::*;
use drm::{Features, ModeConfig};

ddi::module! {
    driver: RDrmNull,
    ops: [char],
}

/// The kernel module.
pub struct RDrmNull;

/// The DRM driver: no state, because there is no hardware.
pub struct NullDriver;

impl drm::Driver for NullDriver {
    const NAME: &'static CStr = c"rdrmnull";
    const DATE: &'static CStr = c"20260928";
    const DESC: &'static CStr = c"Rust DRM null driver (illumos-drm R0)";
    const MAJOR: i32 = 1;
    const MINOR: i32 = 0;
    const PATCHLEVEL: i32 = 0;
    const FEATURES: Features = Features::MODESET.union(Features::ATOMIC).union(Features::RENDER);
    const MODE_CONFIG: ModeConfig = ModeConfig::DEFAULT;
}

impl Driver for RDrmNull {
    type Instance = drm::Device<NullDriver>;
    const LINKINFO: &'static CStr = c"Rust DRM null driver r0";

    fn attach(dev: DevInfo, ctx: &Sleepable) -> Result<Self::Instance, DdiError> {
        drm::Device::attach(dev, NullDriver, ctx)
    }

    fn detach(inst: Self::Instance, _dev: DevInfo, ctx: &Sleepable) -> Result<(), (Self::Instance, DdiError)> {
        inst.detach(ctx)
    }

    fn instance_of_minor(minor: Minor) -> Option<Instance> {
        drm::minor::instance_of_minor(minor)
    }
}
