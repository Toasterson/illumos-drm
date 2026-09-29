// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! A software vblank source for drivers without a vblank interrupt.
//!
//! [`SoftVblank`] runs a `ddi_periodic(9F)` callback every 10 ms (the
//! resolution of that interface) and calls the driver once for each frame
//! boundary of the mode that has passed, with the boundary's exact time.
//! Frame boundaries are computed from `gethrtime(9F)` and the frame length,
//! so the counter advances at the mode's refresh rate on average (60 Hz is
//! 16.67 ms) and timestamps are exact, while each vblank is reported up to
//! 10 ms after its time. If the callback falls behind by more than four
//! frames (a stalled system), the missed boundaries are skipped and the
//! source continues from the latest one.

use alloc::boxed::Box;

use ddi::prelude::*;
use ddi::sys;

/// The period of the polling callback.
const POLL: core::time::Duration = core::time::Duration::from_millis(10);

type Tick = Box<dyn FnMut(&Sleepable) + Send>;

/// A running software vblank source; dropping it stops the callbacks,
/// waiting for a running one. Do not drop it while holding a lock the
/// callback takes.
pub struct SoftVblank {
    _periodic: Periodic<Tick>,
}

fn now() -> i64 {
    // SAFETY: gethrtime(9F) may be called from any context.
    unsafe { sys::gethrtime() }
}

impl SoftVblank {
    /// Starts calling `on_vblank(time_ns)` once per frame of `frame_ns`
    /// nanoseconds, the first time one frame from now. The callback runs on
    /// the level-0 periodic task queue (thread context).
    pub fn start<F: FnMut(i64) + Send + 'static>(frame_ns: u64, mut on_vblank: F, ctx: &Sleepable) -> SoftVblank {
        let frame = i64::try_from(frame_ns.max(1_000_000)).unwrap_or(i64::MAX);
        let mut next = now().saturating_add(frame);
        let tick: Tick = Box::new(move |_ctx: &Sleepable| {
            let t = now();
            if t.saturating_sub(next) > 4 * frame {
                next += (t - next) / frame * frame;
            }
            while t >= next {
                on_vblank(next);
                next = next.saturating_add(frame);
            }
        });
        SoftVblank { _periodic: Periodic::new(POLL, tick, ctx) }
    }
}
