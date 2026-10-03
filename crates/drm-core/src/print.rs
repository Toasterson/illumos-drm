// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The rules of the facade's logging (`include/drm/drm_print.h`): which
//! Linux level goes where in `cmn_err(9F)`, which debug messages
//! `drm_debug` lets through, and how a message is put together in a
//! fixed buffer. The kernel module formats with `vsnprintf(9F)` into a
//! [`Msg`] and hands it to `dev_err`/`cmn_err`; everything else is here and
//! tested on the host.

/// A message's room, the terminating NUL included (Linux's printk limit
/// is 1024; illumos's `LOG_MSGSIZE` is 200 per log line, longer messages
/// wrap).
pub const MSG_SIZE: usize = 512;

/// What ends a message that did not fit.
const CUT: &[u8] = b"...";

/// Linux's `KERN_DEBUG`.
pub const LEVEL_DEBUG: u8 = 7;
/// Linux's `KERN_INFO`, the level of a message without a valid one.
pub const LEVEL_INFO: u8 = 6;

/// The level of a Linux level string (`KERN_SOH` and a digit), `KERN_INFO`
/// for anything else.
pub fn parse_level(level: Option<&[u8]>) -> u8 {
    match level {
        Some([1, d, ..]) if d.is_ascii_digit() && *d <= b'7' => d - b'0',
        _ => LEVEL_INFO,
    }
}

/// Where a message of `level` goes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Route {
    /// `CE_WARN` (true) or `CE_NOTE`.
    pub warn: bool,
    /// The system log only (`!`), not the console.
    pub log_only: bool,
}

/// Errors and warnings (levels 0 to 4) are `CE_WARN` on the console and in
/// the log; notices `CE_NOTE` on both; information and debug messages
/// `CE_NOTE` in the log only.
pub fn route(level: u8) -> Route {
    match level {
        0..=4 => Route { warn: true, log_only: false },
        5 => Route { warn: false, log_only: false },
        _ => Route { warn: false, log_only: true },
    }
}

/// Whether a message is printed: debug messages of a category (0 to 31)
/// only while its bit is set in `mask` (`drm_debug`); every other message
/// always.
pub fn enabled(mask: u32, level: u8, category: i32) -> bool {
    if level != LEVEL_DEBUG || !(0..32).contains(&category) {
        return true;
    }
    mask & (1u32 << category) != 0
}

/// A message being put together: prefixes, then the formatted text, NUL
/// terminated, cut with "..." when it does not fit.
pub struct Msg {
    buf: [u8; MSG_SIZE],
    len: usize,
}

impl Default for Msg {
    fn default() -> Self {
        Self::new()
    }
}

impl Msg {
    /// An empty message.
    pub const fn new() -> Msg {
        Msg { buf: [0; MSG_SIZE], len: 0 }
    }

    /// Appends `s` and a space (a prefix); an empty `s` adds nothing.
    pub fn prefix(&mut self, s: &[u8]) {
        if s.is_empty() {
            return;
        }
        self.push(s);
        self.push(b" ");
    }

    fn push(&mut self, s: &[u8]) {
        let room = MSG_SIZE - 1 - self.len;
        let n = s.len().min(room);
        self.buf[self.len..self.len + n].copy_from_slice(&s[..n]);
        self.len += n;
        self.buf[self.len] = 0;
    }

    /// Appends formatted text: `fmt(dst, room)` writes at most `room` bytes
    /// including a NUL into `dst` and returns the length the whole text
    /// has (`vsnprintf`'s contract). A text that did not fit ends in
    /// "...", and a final newline is dropped (`cmn_err` adds one).
    pub fn format(&mut self, fmt: impl FnOnce(*mut u8, usize) -> usize) {
        let room = MSG_SIZE - self.len;
        let want = fmt(self.buf[self.len..].as_mut_ptr(), room);
        if want >= room {
            self.len = MSG_SIZE - 1;
            self.buf[self.len - CUT.len()..self.len].copy_from_slice(CUT);
        } else {
            self.len += want;
        }
        self.buf[self.len] = 0;
        if self.len > 0 && self.buf[self.len - 1] == b'\n' {
            self.len -= 1;
            self.buf[self.len] = 0;
        }
    }

    /// The message without its NUL.
    pub fn bytes(&self) -> &[u8] {
        &self.buf[..self.len]
    }

    /// The message as a C string.
    pub fn as_ptr(&self) -> *const core::ffi::c_char {
        self.buf.as_ptr().cast()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// vsnprintf's contract with a fixed text.
    fn text(t: &'static [u8]) -> impl FnOnce(*mut u8, usize) -> usize {
        move |dst, room| {
            let n = t.len().min(room - 1);
            // SAFETY: the buffer has `room` bytes.
            unsafe {
                core::ptr::copy_nonoverlapping(t.as_ptr(), dst, n);
                *dst.add(n) = 0;
            }
            t.len()
        }
    }

    #[test]
    fn levels_and_routes() {
        assert_eq!(parse_level(Some(b"\x013")), 3);
        assert_eq!(parse_level(Some(b"\x017")), LEVEL_DEBUG);
        assert_eq!(parse_level(Some(b"\x019")), LEVEL_INFO);
        assert_eq!(parse_level(Some(b"3")), LEVEL_INFO);
        assert_eq!(parse_level(None), LEVEL_INFO);
        assert_eq!(route(3), Route { warn: true, log_only: false });
        assert_eq!(route(4), Route { warn: true, log_only: false });
        assert_eq!(route(5), Route { warn: false, log_only: false });
        assert_eq!(route(6), Route { warn: false, log_only: true });
        assert_eq!(route(7), Route { warn: false, log_only: true });
    }

    #[test]
    fn debug_categories() {
        // drm_debug 0x6: DRM_UT_DRIVER (1) and DRM_UT_KMS (2).
        assert!(!enabled(0x6, LEVEL_DEBUG, 0));
        assert!(enabled(0x6, LEVEL_DEBUG, 1));
        assert!(enabled(0x6, LEVEL_DEBUG, 2));
        assert!(!enabled(0x6, LEVEL_DEBUG, 9));
        assert!(!enabled(0, LEVEL_DEBUG, 31));
        assert!(enabled(u32::MAX, LEVEL_DEBUG, 31));
        // Not debug, or no category: always.
        assert!(enabled(0, 3, 1));
        assert!(enabled(0, LEVEL_DEBUG, -1));
        assert!(enabled(0, LEVEL_DEBUG, 32));
    }

    #[test]
    fn composing() {
        let mut m = Msg::new();
        m.prefix(b"[drm]");
        m.prefix(b"");
        m.prefix(b"fence");
        m.format(text(b"signalled 3\n"));
        assert_eq!(m.bytes(), b"[drm] fence signalled 3");

        let mut m = Msg::new();
        m.prefix(b"[drm]");
        static LONG: [u8; 600] = [b'x'; 600];
        m.format(text(&LONG));
        assert_eq!(m.bytes().len(), MSG_SIZE - 1);
        assert!(m.bytes().ends_with(b"xxx..."));
        assert!(m.bytes().starts_with(b"[drm] xxx"));

        // Exactly filling the room still fits.
        let mut m = Msg::new();
        static FIT: [u8; MSG_SIZE - 1] = [b'y'; MSG_SIZE - 1];
        m.format(text(&FIT));
        assert_eq!(m.bytes().len(), MSG_SIZE - 1);
        assert!(m.bytes().ends_with(b"yyy"));
    }
}
