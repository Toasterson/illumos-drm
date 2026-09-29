// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Pixel formats (`drm_fourcc.h` codes), format modifiers, the legacy
//! `bpp`/`depth` mapping of `ADDFB`, and the `IN_FORMATS` blob.
//!
//! `drm-uapi` does not bind `drm_fourcc.h` (its codes are macros), so the
//! codes this core knows are spelled out here with Linux's `fourcc_code`
//! and `fourcc_mod_code` rules.

use alloc::vec::Vec;

/// Builds a fourcc code from its four characters (`fourcc_code`).
pub const fn fourcc(a: u8, b: u8, c: u8, d: u8) -> u32 {
    (a as u32) | (b as u32) << 8 | (c as u32) << 16 | (d as u32) << 24
}

/// 8-bit colour index (`DRM_FORMAT_C8`).
pub const C8: u32 = fourcc(b'C', b'8', b' ', b' ');
/// 16-bit RGB 5:6:5 (`DRM_FORMAT_RGB565`).
pub const RGB565: u32 = fourcc(b'R', b'G', b'1', b'6');
/// 16-bit xRGB 1:5:5:5 (`DRM_FORMAT_XRGB1555`).
pub const XRGB1555: u32 = fourcc(b'X', b'R', b'1', b'5');
/// 16-bit ARGB 1:5:5:5 (`DRM_FORMAT_ARGB1555`).
pub const ARGB1555: u32 = fourcc(b'A', b'R', b'1', b'5');
/// 24-bit RGB (`DRM_FORMAT_RGB888`).
pub const RGB888: u32 = fourcc(b'R', b'G', b'2', b'4');
/// 24-bit BGR (`DRM_FORMAT_BGR888`).
pub const BGR888: u32 = fourcc(b'B', b'G', b'2', b'4');
/// 32-bit xRGB 8:8:8:8 (`DRM_FORMAT_XRGB8888`).
pub const XRGB8888: u32 = fourcc(b'X', b'R', b'2', b'4');
/// 32-bit ARGB 8:8:8:8 (`DRM_FORMAT_ARGB8888`).
pub const ARGB8888: u32 = fourcc(b'A', b'R', b'2', b'4');
/// 32-bit xBGR 8:8:8:8 (`DRM_FORMAT_XBGR8888`).
pub const XBGR8888: u32 = fourcc(b'X', b'B', b'2', b'4');
/// 32-bit ABGR 8:8:8:8 (`DRM_FORMAT_ABGR8888`).
pub const ABGR8888: u32 = fourcc(b'A', b'B', b'2', b'4');
/// 32-bit xRGB 2:10:10:10 (`DRM_FORMAT_XRGB2101010`).
pub const XRGB2101010: u32 = fourcc(b'X', b'R', b'3', b'0');
/// 32-bit ARGB 2:10:10:10 (`DRM_FORMAT_ARGB2101010`).
pub const ARGB2101010: u32 = fourcc(b'A', b'R', b'3', b'0');
/// 2-plane YCbCr 4:2:0 (`DRM_FORMAT_NV12`).
pub const NV12: u32 = fourcc(b'N', b'V', b'1', b'2');

/// The linear layout (`DRM_FORMAT_MOD_LINEAR`).
pub const MOD_LINEAR: u64 = 0;
/// No valid modifier (`DRM_FORMAT_MOD_INVALID`).
pub const MOD_INVALID: u64 = 0x00ff_ffff_ffff_ffff;

/// Builds a vendor modifier (`fourcc_mod_code`).
pub const fn mod_code(vendor: u8, val: u64) -> u64 {
    (vendor as u64) << 56 | (val & 0x00ff_ffff_ffff_ffff)
}

/// Intel X tiling (`I915_FORMAT_MOD_X_TILED`), used by the tests as a
/// modifier no plane of this core supports.
pub const MOD_I915_X_TILED: u64 = mod_code(0x01, 1);

/// What the core needs to know about a format: bytes per pixel of each
/// plane and the chroma subsampling.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FormatInfo {
    /// The fourcc code.
    pub format: u32,
    /// Bytes per pixel of each plane; 0 past the last plane.
    pub cpp: [u8; 4],
    /// Horizontal chroma subsampling of planes after the first.
    pub hsub: u8,
    /// Vertical chroma subsampling of planes after the first.
    pub vsub: u8,
    /// The colour depth `ADDFB` and `GETFB` report (0 if none).
    pub depth: u8,
}

impl FormatInfo {
    /// The number of planes.
    pub fn planes(&self) -> usize {
        self.cpp.iter().take_while(|&&c| c != 0).count()
    }
    /// The bits per pixel of the first plane.
    pub fn bpp(&self) -> u32 {
        u32::from(self.cpp[0]) * 8
    }
    /// The width of plane `i` for an image `width` pixels wide.
    pub fn plane_width(&self, width: u32, i: usize) -> u32 {
        if i == 0 { width } else { width.div_ceil(u32::from(self.hsub.max(1))) }
    }
    /// The height of plane `i` for an image `height` lines high.
    pub fn plane_height(&self, height: u32, i: usize) -> u32 {
        if i == 0 { height } else { height.div_ceil(u32::from(self.vsub.max(1))) }
    }
}

const fn info(format: u32, cpp: u8, depth: u8) -> FormatInfo {
    FormatInfo { format, cpp: [cpp, 0, 0, 0], hsub: 1, vsub: 1, depth }
}

/// The formats the core knows.
pub const FORMATS: &[FormatInfo] = &[
    info(C8, 1, 8),
    info(RGB565, 2, 16),
    info(XRGB1555, 2, 15),
    info(ARGB1555, 2, 15),
    info(RGB888, 3, 24),
    info(BGR888, 3, 24),
    info(XRGB8888, 4, 24),
    info(ARGB8888, 4, 32),
    info(XBGR8888, 4, 24),
    info(ABGR8888, 4, 32),
    info(XRGB2101010, 4, 30),
    info(ARGB2101010, 4, 30),
    FormatInfo { format: NV12, cpp: [1, 2, 0, 0], hsub: 2, vsub: 2, depth: 0 },
];

/// Looks a format up.
pub fn lookup(format: u32) -> Option<&'static FormatInfo> {
    FORMATS.iter().find(|f| f.format == format)
}

/// The format of a legacy `ADDFB` request (`drm_driver_legacy_fb_format`),
/// or `None` for a combination that has none.
pub fn legacy_format(bpp: u32, depth: u32) -> Option<u32> {
    Some(match (bpp, depth) {
        (8, 8) => C8,
        (16, 15) => XRGB1555,
        (16, 16) => RGB565,
        (24, 24) => RGB888,
        (32, 24) => XRGB8888,
        (32, 30) => XRGB2101010,
        (32, 32) => ARGB8888,
        _ => return None,
    })
}

/// The size of the `drm_format_modifier_blob` header.
const BLOB_HEADER: usize = 24;
/// The size of one `drm_format_modifier`.
const BLOB_MODIFIER: usize = 24;

/// Builds the `IN_FORMATS` blob of a plane (`struct
/// drm_format_modifier_blob`, version 1): the formats, then for each
/// modifier the bit mask of the formats it applies to. `supported` says
/// whether a format and modifier combine. Planes have at most 64 formats,
/// so every mask has offset 0.
pub fn in_formats_blob(formats: &[u32], modifiers: &[u64], supported: impl Fn(u32, u64) -> bool) -> Vec<u8> {
    let fmt_off = BLOB_HEADER;
    let mod_off = (fmt_off + 4 * formats.len()).next_multiple_of(8);
    let mut b = Vec::with_capacity(mod_off + BLOB_MODIFIER * modifiers.len());
    for v in [1u32, 0, formats.len() as u32, fmt_off as u32, modifiers.len() as u32, mod_off as u32] {
        b.extend_from_slice(&v.to_ne_bytes());
    }
    for f in formats {
        b.extend_from_slice(&f.to_ne_bytes());
    }
    b.resize(mod_off, 0);
    for &m in modifiers {
        let mut mask = 0u64;
        for (i, &f) in formats.iter().enumerate().take(64) {
            if supported(f, m) {
                mask |= 1 << i;
            }
        }
        b.extend_from_slice(&mask.to_ne_bytes());
        b.extend_from_slice(&0u32.to_ne_bytes());
        b.extend_from_slice(&0u32.to_ne_bytes());
        b.extend_from_slice(&m.to_ne_bytes());
    }
    b
}

#[cfg(test)]
mod tests {
    use super::*;
    use drm_uapi as uapi;

    #[test]
    fn codes() {
        assert_eq!(XRGB8888, 0x3432_5258);
        assert_eq!(ARGB8888, 0x3432_5241);
        assert_eq!(RGB565, 0x3631_4752);
        assert_eq!(MOD_I915_X_TILED, 0x0100_0000_0000_0001);
        assert_eq!(legacy_format(32, 24), Some(XRGB8888));
        assert_eq!(legacy_format(32, 16), None);
        assert_eq!(lookup(NV12).unwrap().planes(), 2);
        assert_eq!(lookup(NV12).unwrap().plane_width(101, 1), 51);
        assert_eq!(lookup(XRGB8888).unwrap().bpp(), 32);
        assert!(lookup(fourcc(b'Y', b'U', b'Y', b'V')).is_none());
    }

    #[test]
    fn in_formats_layout() {
        let b = in_formats_blob(&[XRGB8888, ARGB8888, RGB565], &[MOD_LINEAR], |f, _| f != RGB565);
        let u32_at = |o: usize| u32::from_ne_bytes(b[o..o + 4].try_into().unwrap());
        let u64_at = |o: usize| u64::from_ne_bytes(b[o..o + 8].try_into().unwrap());
        assert_eq!(core::mem::size_of::<uapi::drm_format_modifier_blob>(), BLOB_HEADER);
        assert_eq!((u32_at(0), u32_at(8), u32_at(12), u32_at(16), u32_at(20)), (1, 3, 24, 1, 40));
        assert_eq!((u32_at(24), u32_at(28), u32_at(32)), (XRGB8888, ARGB8888, RGB565));
        assert_eq!(u64_at(40), 0b011);
        assert_eq!(u64_at(56), MOD_LINEAR);
        assert_eq!(b.len(), 64);
    }
}
