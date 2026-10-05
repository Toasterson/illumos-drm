// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The geometry of dumb buffers (`DRM_IOCTL_MODE_CREATE_DUMB`).
//!
//! A dumb buffer is a linear, CPU-mappable buffer for software rendering
//! and simple scanout. The caller gives width, height and bits per pixel;
//! the kernel chooses the pitch (bytes per row) and the size. The default
//! rules, which a driver may replace through
//! [`Driver::dumb_layout`](crate::Driver::dumb_layout), are:
//!
//! * `width`, `height` and `bpp` are non-zero; any `bpp` is accepted and
//!   counts as `ceil(bpp / 8)` bytes per pixel, as in Linux's
//!   `drm_mode_create_dumb` (which refuses only `bpp > U32_MAX - 8`);
//! * `flags` is ignored, as Linux ignores it: the uAPI header says it must
//!   be zero, but the kernel has never checked, so clients do not reliably
//!   clear it;
//! * the pitch is `width * ceil(bpp / 8)` rounded up to 64 bytes, which
//!   suits the scanout engines and CPU cache lines;
//! * the size is `pitch * height` rounded up to a whole page;
//! * pitch and size fit the 32-bit arithmetic of Linux's
//!   `drm_mode_create_dumb`, so a client sees the same limits as there.
//!
//! Anything else, including every overflow, is `EINVAL`. The arithmetic is
//! checked at each step and never wraps.

use ddi::Errno;

use crate::offsets::PAGE_SIZE;

/// The caller's request, from `struct drm_mode_create_dumb`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DumbRequest {
    /// Width in pixels.
    pub width: u32,
    /// Height in pixels.
    pub height: u32,
    /// Bits per pixel.
    pub bpp: u32,
    /// Reserved; ignored, as in Linux.
    pub flags: u32,
}

/// The kernel's answer: bytes per row and total size.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DumbLayout {
    /// Bytes per row.
    pub pitch: u32,
    /// Size of the buffer in bytes, a multiple of the page size.
    pub size: u64,
}

/// The pitch alignment of the default rules.
pub const PITCH_ALIGN: u32 = 64;

/// Checks the parts of a request that do not depend on the layout, as
/// Linux's `drm_mode_create_dumb` does: non-zero dimensions and a `bpp`
/// that is non-zero and at most `U32_MAX - 8`. `flags` is not checked.
pub fn check_request(req: &DumbRequest) -> Result<(), Errno> {
    if req.width == 0 || req.height == 0 || req.bpp == 0 || req.bpp > u32::MAX - 8 {
        return Err(Errno::EINVAL);
    }
    Ok(())
}

/// Computes the layout for `bytes_per_pixel`-byte pixels with the pitch
/// rounded up to `pitch_align` bytes (a power of two), with every overflow
/// reported as `EINVAL`.
pub fn layout(req: &DumbRequest, bytes_per_pixel: u32, pitch_align: u32) -> Result<DumbLayout, Errno> {
    check_request(req)?;
    if bytes_per_pixel == 0 || !pitch_align.is_power_of_two() {
        return Err(Errno::EINVAL);
    }
    let row = req.width.checked_mul(bytes_per_pixel).ok_or(Errno::EINVAL)?;
    let pitch = row.checked_add(pitch_align - 1).ok_or(Errno::EINVAL)? & !(pitch_align - 1);
    let bytes = pitch.checked_mul(req.height).ok_or(Errno::EINVAL)?;
    let size = u64::from(bytes).checked_add(PAGE_SIZE - 1).ok_or(Errno::EINVAL)? & !(PAGE_SIZE - 1);
    // Linux computes the page-aligned size in 32 bits and refuses a wrap.
    if size > u64::from(u32::MAX) {
        return Err(Errno::EINVAL);
    }
    Ok(DumbLayout { pitch, size })
}

/// The default rules of the module documentation.
pub fn standard_layout(req: &DumbRequest) -> Result<DumbLayout, Errno> {
    check_request(req)?;
    layout(req, req.bpp.div_ceil(8), PITCH_ALIGN)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn req(width: u32, height: u32, bpp: u32) -> DumbRequest {
        DumbRequest { width, height, bpp, flags: 0 }
    }

    #[test]
    fn common_formats() {
        assert_eq!(standard_layout(&req(1024, 768, 32)), Ok(DumbLayout { pitch: 4096, size: 4096 * 768 }));
        assert_eq!(standard_layout(&req(640, 480, 16)), Ok(DumbLayout { pitch: 1280, size: 1280 * 480 }));
        // 3 bytes per pixel, pitch rounded to 64: 100 * 3 = 300 -> 320.
        assert_eq!(standard_layout(&req(100, 10, 24)), Ok(DumbLayout { pitch: 320, size: 4096 }));
        assert_eq!(standard_layout(&req(1, 1, 8)), Ok(DumbLayout { pitch: 64, size: 4096 }));
        assert_eq!(standard_layout(&req(64, 64, 32)), Ok(DumbLayout { pitch: 256, size: 16384 }));
    }

    #[test]
    fn refuses_bad_requests() {
        for r in [req(0, 1, 32), req(1, 0, 32), req(1, 1, 0)] {
            assert_eq!(standard_layout(&r), Err(Errno::EINVAL), "{r:?}");
        }
        for bpp in [u32::MAX, u32::MAX - 7] {
            assert_eq!(standard_layout(&req(16, 16, bpp)), Err(Errno::EINVAL), "bpp {bpp}");
        }
    }

    #[test]
    fn linux_leniency() {
        // flags is ignored.
        assert_eq!(standard_layout(&DumbRequest { flags: 0xffff_ffff, ..req(16, 16, 32) }).map(|l| l.pitch), Ok(64));
        // Any non-zero bpp: ceil(bpp / 8) bytes per pixel.
        assert_eq!(standard_layout(&req(100, 1, 1)).map(|l| l.pitch), Ok(128));
        assert_eq!(standard_layout(&req(100, 1, 12)).map(|l| l.pitch), Ok(256));
        assert_eq!(standard_layout(&req(100, 1, 64)).map(|l| l.pitch), Ok(832));
        // The largest bpp Linux accepts; the pitch then overflows.
        assert_eq!(standard_layout(&req(1, 1, u32::MAX - 8)).map(|l| l.pitch), Ok(0x2000_0000));
        assert_eq!(standard_layout(&req(2, 1, u32::MAX - 8)), Ok(DumbLayout { pitch: 0x4000_0000, size: 0x4000_0000 }));
        assert_eq!(standard_layout(&req(8, 1, u32::MAX - 8)), Err(Errno::EINVAL));
    }

    #[test]
    fn refuses_overflow() {
        // width * 4 overflows u32.
        assert_eq!(standard_layout(&req(0x4000_0000, 1, 32)), Err(Errno::EINVAL));
        // The pitch rounding overflows.
        assert_eq!(standard_layout(&req(u32::MAX, 1, 8)), Err(Errno::EINVAL));
        // pitch * height overflows u32.
        assert_eq!(standard_layout(&req(65536, 65536, 32)), Err(Errno::EINVAL));
        // pitch * height fits in u32 (0xffff_ffc0) but the page rounding
        // reaches 4 GiB.
        assert_eq!(standard_layout(&req(16, 67_108_863, 32)), Err(Errno::EINVAL));
        // The largest page-aligned size that still fits: 0xffff_f000.
        assert_eq!(standard_layout(&req(1024, 1_048_575, 32)), Ok(DumbLayout { pitch: 4096, size: 0xffff_f000 }));
        // Custom layouts check their parameters.
        assert_eq!(layout(&req(1, 1, 32), 0, 64), Err(Errno::EINVAL));
        assert_eq!(layout(&req(1, 1, 32), 4, 48), Err(Errno::EINVAL));
        assert_eq!(layout(&req(3, 1, 64), 8, 1), Ok(DumbLayout { pitch: 24, size: 4096 }));
    }
}
