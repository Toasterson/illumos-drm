// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Locks, condition variables and the monotonic clock for the
//! synchronisation objects, with a host implementation for unit tests.
//!
//! In the kernel these are `ddi::Mutex`, `ddi::CondVar` and `gethrtime(9F)`;
//! in `cargo test` on the host they are `std::sync::Mutex`,
//! `std::sync::Condvar` and `std::time::Instant`, so that the fence,
//! wound/wait and sync object logic runs from real threads. The API is the
//! subset the core uses, with the kernel's shape: waits take the
//! [`Sleepable`] token and report why they returned.
//!
//! A lock that code in interrupt context takes (fence state, sync object
//! waiters) is created with [`KMutex::with_priority`] and the priority of
//! the device's fence domain; see [`crate::fence`] for the rule.

use ddi::{Errno, IntrPriority, Sleepable};

/// Why a wait returned.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Wake {
    /// Woken, or woken spuriously; the caller checks its condition.
    Woken,
    /// The deadline passed.
    TimedOut,
    /// A signal is pending for the thread (interruptible waits only).
    Interrupted,
}

impl Wake {
    /// The errno Linux gives a wait that ended this way without its
    /// condition becoming true: `ETIME` or `EINTR`.
    pub fn errno(self) -> Option<Errno> {
        match self {
            Wake::Woken => None,
            Wake::TimedOut => Some(ETIME),
            Wake::Interrupted => Some(Errno::EINTR),
        }
    }
}

/// `ETIME`, the errno of a timed-out fence or sync object wait (Linux
/// `-ETIME`). `ddi::Errno` has no named constant for it.
pub const ETIME: Errno = match Errno::new(ddi::sys::errno::ETIME) {
    Some(e) => e,
    None => Errno::EINVAL,
};

/// An absolute time on the monotonic clock in nanoseconds
/// (`CLOCK_MONOTONIC`, `gethrtime(9F)`), the unit of DRM wait timeouts.
pub type Nanos = i64;

#[cfg(not(test))]
mod imp {
    use super::*;
    use core::time::Duration;

    /// A mutex owning its data.
    pub struct KMutex<T>(ddi::Mutex<T>);
    /// The guard of a [`KMutex`].
    pub type KGuard<'a, T> = ddi::MutexGuard<'a, T>;

    impl<T> KMutex<T> {
        /// An adaptive mutex for thread context only.
        pub fn new(v: T) -> KMutex<T> {
            KMutex(ddi::Mutex::new(v))
        }
        /// A mutex that code at priority `pri` (a soft interrupt, say) may
        /// take as well; `None` means thread context and callouts only.
        pub fn with_priority(v: T, pri: Option<IntrPriority>) -> KMutex<T> {
            match pri {
                Some(p) => KMutex(ddi::Mutex::with_priority(v, p)),
                None => KMutex(ddi::Mutex::new(v)),
            }
        }
        /// Locks the mutex.
        pub fn lock(&self) -> KGuard<'_, T> {
            self.0.lock()
        }
    }

    /// A condition variable.
    pub struct KCondVar(ddi::CondVar);

    impl KCondVar {
        /// A new condition variable.
        pub fn new() -> KCondVar {
            KCondVar(ddi::CondVar::new())
        }
        /// Wakes every waiter.
        pub fn notify_all(&self) {
            self.0.notify_all()
        }
        /// Waits until woken, the monotonic clock reaches `deadline`
        /// (`None`: no deadline) or, if `intr`, a signal is pending.
        pub fn wait<'a, T>(
            &self,
            g: KGuard<'a, T>,
            deadline: Option<Nanos>,
            intr: bool,
            ctx: &Sleepable,
        ) -> (KGuard<'a, T>, Wake) {
            match deadline {
                None if intr => {
                    let (g, ok) = self.0.wait_sig(g, ctx);
                    (g, if ok { Wake::Woken } else { Wake::Interrupted })
                }
                None => (self.0.wait(g, ctx), Wake::Woken),
                Some(d) => {
                    let left = d.saturating_sub(now());
                    if left <= 0 {
                        return (g, Wake::TimedOut);
                    }
                    let t = Duration::from_nanos(left as u64);
                    let (g, r) = if intr { self.0.wait_timeout_sig(g, t, ctx) } else { self.0.wait_timeout(g, t, ctx) };
                    let w = match r {
                        ddi::WaitResult::Woken => Wake::Woken,
                        ddi::WaitResult::TimedOut => Wake::TimedOut,
                        ddi::WaitResult::Interrupted => Wake::Interrupted,
                    };
                    (g, w)
                }
            }
        }
    }

    /// The monotonic clock, `gethrtime(9F)`; any context.
    pub fn now() -> Nanos {
        ddi::HrTime::now().as_nanos()
    }

    /// Gives other threads a chance while a callback that runs on another
    /// processor finishes: one clock tick.
    pub fn yield_briefly(ctx: &Sleepable) {
        ddi::time::delay(ddi::Ticks::from_micros(1), ctx)
    }
}

#[cfg(test)]
mod imp {
    extern crate std;
    use super::*;
    use std::sync::{Condvar, Mutex, MutexGuard, OnceLock};
    use std::time::{Duration, Instant};

    /// A mutex owning its data (host: `std::sync::Mutex`).
    pub struct KMutex<T>(Mutex<T>);
    /// The guard of a [`KMutex`].
    pub type KGuard<'a, T> = MutexGuard<'a, T>;

    impl<T> KMutex<T> {
        /// An adaptive mutex.
        pub fn new(v: T) -> KMutex<T> {
            KMutex(Mutex::new(v))
        }
        /// A mutex for interrupt priority `pri` (ignored on the host).
        pub fn with_priority(v: T, _pri: Option<IntrPriority>) -> KMutex<T> {
            KMutex(Mutex::new(v))
        }
        /// Locks the mutex.
        pub fn lock(&self) -> KGuard<'_, T> {
            self.0.lock().unwrap_or_else(|e| e.into_inner())
        }
    }

    std::thread_local! {
        /// Set by a test to make the next interruptible wait of this
        /// thread report a pending signal.
        pub static SIGNAL_PENDING: core::cell::Cell<bool> = const { core::cell::Cell::new(false) };
    }

    /// A condition variable (host: `std::sync::Condvar`).
    pub struct KCondVar(Condvar);

    impl KCondVar {
        /// A new condition variable.
        pub fn new() -> KCondVar {
            KCondVar(Condvar::new())
        }
        /// Wakes every waiter.
        pub fn notify_all(&self) {
            self.0.notify_all()
        }
        /// Waits as the kernel version does; a pending test signal ends an
        /// interruptible wait.
        pub fn wait<'a, T>(
            &self,
            g: KGuard<'a, T>,
            deadline: Option<Nanos>,
            intr: bool,
            _ctx: &Sleepable,
        ) -> (KGuard<'a, T>, Wake) {
            if intr && SIGNAL_PENDING.with(|s| s.replace(false)) {
                return (g, Wake::Interrupted);
            }
            match deadline {
                None => (self.0.wait(g).unwrap_or_else(|e| e.into_inner()), Wake::Woken),
                Some(d) => {
                    let left = d.saturating_sub(now());
                    if left <= 0 {
                        return (g, Wake::TimedOut);
                    }
                    let (g, r) =
                        self.0.wait_timeout(g, Duration::from_nanos(left as u64)).unwrap_or_else(|e| e.into_inner());
                    (g, if r.timed_out() { Wake::TimedOut } else { Wake::Woken })
                }
            }
        }
    }

    /// Nanoseconds since the first call, plus one second.
    pub fn now() -> Nanos {
        static START: OnceLock<Instant> = OnceLock::new();
        // Start at one second so that "a deadline in the past" exists.
        START.get_or_init(Instant::now).elapsed().as_nanos() as Nanos + 1_000_000_000
    }

    /// Yields the processor.
    pub fn yield_briefly(_ctx: &Sleepable) {
        std::thread::yield_now()
    }
}

#[cfg(test)]
pub(crate) use imp::SIGNAL_PENDING;
pub use imp::{KCondVar, KGuard, KMutex, now, yield_briefly};

impl Default for KCondVar {
    fn default() -> Self {
        Self::new()
    }
}
