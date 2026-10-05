// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Display modes: the kernel's mode type, its conversion to and from the
//! uAPI `drm_mode_modeinfo`, validation, the VESA DMT and CTA-861 mode
//! tables, the CVT formula, and the mode list order.
//!
//! The rules follow Linux's `drm_modes.c` (`drm_mode_convert_to_umode`,
//! `drm_mode_convert_umode`, `drm_mode_validate_basic`, `drm_mode_vrefresh`,
//! `drm_cvt_mode`, `drm_mode_sort`); the timings are the published VESA
//! DMT and CTA-861 values. All arithmetic is integer arithmetic, because
//! kernel code is built without floating point.

use alloc::vec::Vec;
use core::cmp::Ordering;

use ddi::Errno;
use drm_uapi as uapi;

/// The length of a mode name, including the terminating NUL
/// (`DRM_DISPLAY_MODE_LEN`).
pub const NAME_LEN: usize = uapi::DRM_DISPLAY_MODE_LEN as usize;

/// `ERANGE`, for the checks that Linux answers with it.
pub const ERANGE: Errno = Errno::ERANGE;

/// The picture aspect ratio of a mode (`HDMI_PICTURE_ASPECT_*`), which the
/// uAPI carries in `DRM_MODE_FLAG_PIC_AR_*` for clients that set
/// `DRM_CLIENT_CAP_ASPECT_RATIO`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Aspect {
    /// No aspect ratio.
    #[default]
    None,
    /// 4:3.
    R4x3,
    /// 16:9.
    R16x9,
    /// 64:27.
    R64x27,
    /// 256:135.
    R256x135,
}

impl Aspect {
    fn flag(self) -> u32 {
        match self {
            Aspect::None => uapi::DRM_MODE_FLAG_PIC_AR_NONE,
            Aspect::R4x3 => uapi::DRM_MODE_FLAG_PIC_AR_4_3,
            Aspect::R16x9 => uapi::DRM_MODE_FLAG_PIC_AR_16_9,
            Aspect::R64x27 => uapi::DRM_MODE_FLAG_PIC_AR_64_27,
            Aspect::R256x135 => uapi::DRM_MODE_FLAG_PIC_AR_256_135,
        }
    }

    fn from_flags(flags: u32) -> Option<Aspect> {
        Some(match flags & uapi::DRM_MODE_FLAG_PIC_AR_MASK {
            uapi::DRM_MODE_FLAG_PIC_AR_NONE => Aspect::None,
            uapi::DRM_MODE_FLAG_PIC_AR_4_3 => Aspect::R4x3,
            uapi::DRM_MODE_FLAG_PIC_AR_16_9 => Aspect::R16x9,
            uapi::DRM_MODE_FLAG_PIC_AR_64_27 => Aspect::R64x27,
            uapi::DRM_MODE_FLAG_PIC_AR_256_135 => Aspect::R256x135,
            _ => return None,
        })
    }
}

/// A display mode: the pixel clock and the horizontal and vertical timings.
///
/// `flags` holds the `DRM_MODE_FLAG_*` bits without the aspect-ratio field,
/// which is kept in `aspect`, and `typ` the `DRM_MODE_TYPE_*` bits.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DisplayMode {
    /// Pixel clock in kHz.
    pub clock: u32,
    /// Visible pixels per line.
    pub hdisplay: u16,
    /// Start of the horizontal sync pulse.
    pub hsync_start: u16,
    /// End of the horizontal sync pulse.
    pub hsync_end: u16,
    /// Pixels per line, including blanking.
    pub htotal: u16,
    /// Horizontal skew.
    pub hskew: u16,
    /// Visible lines.
    pub vdisplay: u16,
    /// Start of the vertical sync pulse.
    pub vsync_start: u16,
    /// End of the vertical sync pulse.
    pub vsync_end: u16,
    /// Lines per frame, including blanking.
    pub vtotal: u16,
    /// Line repetition (0 or 1 for none).
    pub vscan: u16,
    /// `DRM_MODE_FLAG_*`, without `DRM_MODE_FLAG_PIC_AR_*`.
    pub flags: u32,
    /// `DRM_MODE_TYPE_*`.
    pub typ: u32,
    /// The picture aspect ratio.
    pub aspect: Aspect,
    /// The name, NUL-padded.
    pub name: [u8; NAME_LEN],
}

/// Sync polarities for the table constructors.
const PH: u32 = uapi::DRM_MODE_FLAG_PHSYNC;
const NH: u32 = uapi::DRM_MODE_FLAG_NHSYNC;
const PV: u32 = uapi::DRM_MODE_FLAG_PVSYNC;
const NV: u32 = uapi::DRM_MODE_FLAG_NVSYNC;
const IL: u32 = uapi::DRM_MODE_FLAG_INTERLACE;

impl DisplayMode {
    /// Builds a driver mode from its timings and sets its name
    /// (`<h>x<v>`, with `i` for interlaced modes).
    #[allow(clippy::too_many_arguments, reason = "the timing parameters of a mode")]
    pub const fn new(clock: u32, h: [u16; 4], v: [u16; 4], flags: u32) -> DisplayMode {
        let mut m = DisplayMode {
            clock,
            hdisplay: h[0],
            hsync_start: h[1],
            hsync_end: h[2],
            htotal: h[3],
            hskew: 0,
            vdisplay: v[0],
            vsync_start: v[1],
            vsync_end: v[2],
            vtotal: v[3],
            vscan: 0,
            flags,
            typ: uapi::DRM_MODE_TYPE_DRIVER,
            aspect: Aspect::None,
            name: [0; NAME_LEN],
        };
        m.name = default_name(h[0], v[0], flags & IL != 0);
        m
    }

    const fn with_aspect(mut self, aspect: Aspect) -> DisplayMode {
        self.aspect = aspect;
        self
    }

    /// Returns the name without its NUL padding.
    pub fn name_bytes(&self) -> &[u8] {
        let n = self.name.iter().position(|&b| b == 0).unwrap_or(NAME_LEN);
        self.name.get(..n).unwrap_or(&[])
    }

    /// The vertical refresh rate in Hz, rounded to the nearest integer, as
    /// Linux's `drm_mode_vrefresh`.
    pub fn vrefresh(&self) -> u32 {
        let (num, den) = self.refresh_ratio();
        (num * 1000 + den / 2).checked_div(den).unwrap_or(0) as u32
    }

    /// The refresh rate as the fraction `clock * 1000 / den`: numerator and
    /// denominator, in Hz.
    fn refresh_ratio(&self) -> (u64, u64) {
        if self.htotal == 0 || self.vtotal == 0 {
            return (0, 0);
        }
        let mut num = u64::from(self.clock);
        let mut den = u64::from(self.htotal) * u64::from(self.vtotal);
        if self.flags & IL != 0 {
            num *= 2;
        }
        if self.flags & uapi::DRM_MODE_FLAG_DBLSCAN != 0 {
            den *= 2;
        }
        if self.vscan > 1 {
            den *= u64::from(self.vscan);
        }
        (num, den)
    }

    /// The duration of one refresh (a field for interlaced modes) in
    /// nanoseconds, or `None` for a mode without timings.
    pub fn frame_ns(&self) -> Option<u64> {
        let (num, den) = self.refresh_ratio();
        if num == 0 {
            return None;
        }
        // den / (num * 1000) seconds. den reaches 2^49 with the largest
        // totals, double scan and vscan, so the product needs 128 bits;
        // the result is capped rather than wrapped.
        let ns = u128::from(den) * 1_000_000 / u128::from(num);
        Some(u64::try_from(ns).unwrap_or(u64::MAX))
    }

    /// The timings, flags and aspect ratio without type and name: two modes
    /// with equal keys are the same mode (Linux's `drm_mode_equal`).
    fn key(&self) -> (u32, [u16; 10], u32, Aspect) {
        (
            self.clock,
            [
                self.hdisplay,
                self.hsync_start,
                self.hsync_end,
                self.htotal,
                self.hskew,
                self.vdisplay,
                self.vsync_start,
                self.vsync_end,
                self.vtotal,
                self.vscan,
            ],
            self.flags,
            self.aspect,
        )
    }

    /// Reports whether two modes have the same timings, flags and aspect
    /// ratio.
    pub fn same_timings(&self, other: &DisplayMode) -> bool {
        self.key() == other.key()
    }

    /// Converts the mode for userland (`drm_mode_convert_to_umode`). The
    /// aspect ratio is reported only to clients that asked for it
    /// (`DRM_CLIENT_CAP_ASPECT_RATIO`).
    pub fn to_umode(&self, aspect_allowed: bool) -> uapi::drm_mode_modeinfo {
        let mut name = [0 as core::ffi::c_char; NAME_LEN];
        for (d, s) in name.iter_mut().zip(self.name.iter().take(NAME_LEN - 1)) {
            *d = *s as core::ffi::c_char;
        }
        let ar = if aspect_allowed { self.aspect.flag() } else { 0 };
        uapi::drm_mode_modeinfo {
            clock: self.clock,
            hdisplay: self.hdisplay,
            hsync_start: self.hsync_start,
            hsync_end: self.hsync_end,
            htotal: self.htotal,
            hskew: self.hskew,
            vdisplay: self.vdisplay,
            vsync_start: self.vsync_start,
            vsync_end: self.vsync_end,
            vtotal: self.vtotal,
            vscan: self.vscan,
            vrefresh: self.vrefresh(),
            flags: self.flags | ar,
            type_: self.typ,
            name,
        }
    }

    /// Converts a mode from userland (`drm_mode_convert_umode`): `ERANGE`
    /// for a clock or refresh rate above `INT_MAX`, `EINVAL` for an unknown
    /// aspect ratio or a mode that fails [`DisplayMode::validate_basic`].
    /// Unknown type bits are dropped, as Linux does for old userland that
    /// left the field uninitialised.
    pub fn from_umode(u: &uapi::drm_mode_modeinfo) -> Result<DisplayMode, Errno> {
        if u.clock > i32::MAX as u32 || u.vrefresh > i32::MAX as u32 {
            return Err(ERANGE);
        }
        let aspect = Aspect::from_flags(u.flags).ok_or(Errno::EINVAL)?;
        let mut name = [0u8; NAME_LEN];
        for (d, s) in name.iter_mut().zip(u.name.iter().take(NAME_LEN - 1)) {
            *d = *s as u8;
        }
        if let Some(end) = name.iter().position(|&b| b == 0) {
            name.iter_mut().skip(end).for_each(|b| *b = 0);
        }
        let m = DisplayMode {
            clock: u.clock,
            hdisplay: u.hdisplay,
            hsync_start: u.hsync_start,
            hsync_end: u.hsync_end,
            htotal: u.htotal,
            hskew: u.hskew,
            vdisplay: u.vdisplay,
            vsync_start: u.vsync_start,
            vsync_end: u.vsync_end,
            vtotal: u.vtotal,
            vscan: u.vscan,
            flags: u.flags & !uapi::DRM_MODE_FLAG_PIC_AR_MASK,
            typ: u.type_ & uapi::DRM_MODE_TYPE_ALL,
            aspect,
            name,
        };
        m.validate_basic()?;
        Ok(m)
    }

    /// Checks the mode for internal consistency (`drm_mode_validate_basic`):
    /// known type and flag bits, a known stereo layout, a non-zero clock, and
    /// display, sync start, sync end and total in non-decreasing order on
    /// both axes with a non-zero display size.
    pub fn validate_basic(&self) -> Result<(), Errno> {
        if self.typ & !uapi::DRM_MODE_TYPE_ALL != 0 || self.flags & !uapi::DRM_MODE_FLAG_ALL != 0 {
            return Err(Errno::EINVAL);
        }
        if self.flags & uapi::DRM_MODE_FLAG_3D_MASK > uapi::DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF {
            return Err(Errno::EINVAL);
        }
        if self.clock == 0 {
            return Err(Errno::EINVAL);
        }
        let h = self.hdisplay != 0
            && self.hsync_start >= self.hdisplay
            && self.hsync_end >= self.hsync_start
            && self.htotal >= self.hsync_end;
        let v = self.vdisplay != 0
            && self.vsync_start >= self.vdisplay
            && self.vsync_end >= self.vsync_start
            && self.vtotal >= self.vsync_end;
        if h && v { Ok(()) } else { Err(Errno::EINVAL) }
    }
}

/// `<h>x<v>` or `<h>x<v>i`, as Linux's `drm_mode_set_name`.
const fn default_name(h: u16, v: u16, interlaced: bool) -> [u8; NAME_LEN] {
    let mut out = [0u8; NAME_LEN];
    let mut i = 0;
    i = put_num(&mut out, i, h);
    out[i] = b'x';
    i += 1;
    i = put_num(&mut out, i, v);
    if interlaced {
        out[i] = b'i';
    }
    out
}

const fn put_num(out: &mut [u8; NAME_LEN], mut i: usize, n: u16) -> usize {
    let mut digits = [0u8; 5];
    let mut k = 0;
    let mut n = n;
    loop {
        digits[k] = b'0' + (n % 10) as u8;
        k += 1;
        n /= 10;
        if n == 0 {
            break;
        }
    }
    while k > 0 {
        k -= 1;
        out[i] = digits[k];
        i += 1;
    }
    i
}

/// Limits a connector applies to the modes it offers (Linux's
/// `drm_mode_validate_size`, `interlace_allowed`, `doublescan_allowed` and
/// the driver's clock limit).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ModeLimits {
    /// Largest `hdisplay`.
    pub max_width: u16,
    /// Largest `vdisplay`.
    pub max_height: u16,
    /// Largest pixel clock in kHz; 0 for no limit.
    pub max_clock: u32,
    /// Whether interlaced modes are allowed.
    pub interlace: bool,
    /// Whether double-scan modes are allowed.
    pub dblscan: bool,
}

impl ModeLimits {
    /// Reports whether a mode is valid and within the limits.
    pub fn allows(&self, m: &DisplayMode) -> bool {
        m.validate_basic().is_ok()
            && m.hdisplay <= self.max_width
            && m.vdisplay <= self.max_height
            && (self.max_clock == 0 || m.clock <= self.max_clock)
            && (self.interlace || m.flags & IL == 0)
            && (self.dblscan || m.flags & uapi::DRM_MODE_FLAG_DBLSCAN == 0)
    }
}

/// Drops invalid and duplicate modes (keeping the first and merging the
/// preferred bit into it) and sorts the rest as Linux's `drm_mode_sort`:
/// preferred first, then by area, refresh rate and clock, largest first.
pub fn finish_list(modes: &mut Vec<DisplayMode>, limits: &ModeLimits) {
    modes.retain(|m| limits.allows(m));
    let mut i = 0;
    while i < modes.len() {
        let dup = modes.get(..i).and_then(|head| {
            let m = modes.get(i)?;
            head.iter().position(|o| o.same_timings(m)).map(|p| (p, m.typ))
        });
        match dup {
            Some((p, typ)) => {
                if let Some(first) = modes.get_mut(p) {
                    first.typ |= typ & uapi::DRM_MODE_TYPE_PREFERRED;
                }
                modes.remove(i);
            }
            None => i += 1,
        }
    }
    modes.sort_by(mode_order);
}

fn mode_order(a: &DisplayMode, b: &DisplayMode) -> Ordering {
    let pref = |m: &DisplayMode| m.typ & uapi::DRM_MODE_TYPE_PREFERRED != 0;
    let area = |m: &DisplayMode| u32::from(m.hdisplay) * u32::from(m.vdisplay);
    pref(b).cmp(&pref(a)).then(area(b).cmp(&area(a))).then(b.vrefresh().cmp(&a.vrefresh())).then(b.clock.cmp(&a.clock))
}

// --- VESA DMT ----------------------------------------------------------------

/// A table mode with its nominal refresh rate, for lookups by size and rate.
#[derive(Clone, Copy, Debug)]
pub struct TableMode {
    /// The nominal refresh rate in Hz.
    pub refresh: u16,
    /// The mode.
    pub mode: DisplayMode,
}

const fn dmt(refresh: u16, clock: u32, h: [u16; 4], v: [u16; 4], flags: u32) -> TableMode {
    TableMode { refresh, mode: DisplayMode::new(clock, h, v, flags) }
}

/// The VESA Display Monitor Timings this core knows, the modes of the
/// established timings of EDID and the common standard timings.
pub const DMT: &[TableMode] = &[
    dmt(60, 25175, [640, 656, 752, 800], [480, 490, 492, 525], NH | NV),
    dmt(72, 31500, [640, 664, 704, 832], [480, 489, 492, 520], NH | NV),
    dmt(75, 31500, [640, 656, 720, 840], [480, 481, 484, 500], NH | NV),
    dmt(85, 36000, [640, 696, 752, 832], [480, 481, 484, 509], NH | NV),
    dmt(56, 36000, [800, 824, 896, 1024], [600, 601, 603, 625], PH | PV),
    dmt(60, 40000, [800, 840, 968, 1056], [600, 601, 605, 628], PH | PV),
    dmt(72, 50000, [800, 856, 976, 1040], [600, 637, 643, 666], PH | PV),
    dmt(75, 49500, [800, 816, 896, 1056], [600, 601, 604, 625], PH | PV),
    dmt(60, 65000, [1024, 1048, 1184, 1344], [768, 771, 777, 806], NH | NV),
    dmt(70, 75000, [1024, 1048, 1184, 1328], [768, 771, 777, 806], NH | NV),
    dmt(75, 78750, [1024, 1040, 1136, 1312], [768, 769, 772, 800], PH | PV),
    dmt(75, 108000, [1152, 1216, 1344, 1600], [864, 865, 868, 900], PH | PV),
    dmt(60, 74250, [1280, 1390, 1430, 1650], [720, 725, 730, 750], PH | PV),
    dmt(60, 79500, [1280, 1344, 1472, 1664], [768, 771, 778, 798], NH | PV),
    dmt(60, 83500, [1280, 1352, 1480, 1680], [800, 803, 809, 831], NH | PV),
    dmt(60, 108000, [1280, 1376, 1488, 1800], [960, 961, 964, 1000], PH | PV),
    dmt(60, 108000, [1280, 1328, 1440, 1688], [1024, 1025, 1028, 1066], PH | PV),
    dmt(75, 135000, [1280, 1296, 1440, 1688], [1024, 1025, 1028, 1066], PH | PV),
    dmt(60, 85500, [1360, 1424, 1536, 1792], [768, 771, 777, 795], PH | PV),
    dmt(60, 85500, [1366, 1436, 1579, 1792], [768, 771, 774, 798], PH | PV),
    dmt(60, 121750, [1400, 1488, 1632, 1864], [1050, 1053, 1057, 1089], NH | PV),
    dmt(60, 106500, [1440, 1520, 1672, 1904], [900, 903, 909, 934], NH | PV),
    dmt(60, 108000, [1600, 1624, 1704, 1800], [900, 901, 904, 1000], PH | PV),
    dmt(60, 162000, [1600, 1664, 1856, 2160], [1200, 1201, 1204, 1250], PH | PV),
    dmt(60, 146250, [1680, 1784, 1960, 2240], [1050, 1053, 1059, 1089], NH | PV),
    dmt(60, 148500, [1920, 2008, 2052, 2200], [1080, 1084, 1089, 1125], PH | PV),
    dmt(60, 154000, [1920, 1968, 2000, 2080], [1200, 1203, 1209, 1235], PH | NV),
];

/// Finds a DMT mode by size and nominal refresh rate.
pub fn find_dmt(h: u16, v: u16, refresh: u16) -> Option<DisplayMode> {
    DMT.iter().find(|t| t.mode.hdisplay == h && t.mode.vdisplay == v && t.refresh == refresh).map(|t| t.mode)
}

/// The established timings of EDID (bytes 0x23 to 0x25), bit by bit from
/// bit 7 of byte 0x23: the modes of Linux's `edid_est_modes`, which are
/// DMT modes and the historical Apple and IBM ones.
pub const ESTABLISHED: [DisplayMode; 17] = [
    DisplayMode::new(28320, [720, 738, 846, 900], [400, 412, 414, 449], NH | PV),
    DisplayMode::new(35500, [720, 738, 846, 900], [400, 421, 423, 449], NH | NV),
    DisplayMode::new(25175, [640, 656, 752, 800], [480, 490, 492, 525], NH | NV),
    DisplayMode::new(30240, [640, 704, 768, 864], [480, 483, 486, 525], NH | NV),
    DisplayMode::new(31500, [640, 664, 704, 832], [480, 489, 492, 520], NH | NV),
    DisplayMode::new(31500, [640, 656, 720, 840], [480, 481, 484, 500], NH | NV),
    DisplayMode::new(36000, [800, 824, 896, 1024], [600, 601, 603, 625], PH | PV),
    DisplayMode::new(40000, [800, 840, 968, 1056], [600, 601, 605, 628], PH | PV),
    DisplayMode::new(50000, [800, 856, 976, 1040], [600, 637, 643, 666], PH | PV),
    DisplayMode::new(49500, [800, 816, 896, 1056], [600, 601, 604, 625], PH | PV),
    DisplayMode::new(57284, [832, 864, 928, 1152], [624, 625, 628, 667], NH | NV),
    DisplayMode::new(44900, [1024, 1032, 1208, 1264], [768, 768, 776, 817], PH | PV | IL),
    DisplayMode::new(65000, [1024, 1048, 1184, 1344], [768, 771, 777, 806], NH | NV),
    DisplayMode::new(75000, [1024, 1048, 1184, 1328], [768, 771, 777, 806], NH | NV),
    DisplayMode::new(78750, [1024, 1040, 1136, 1312], [768, 769, 772, 800], PH | PV),
    DisplayMode::new(135000, [1280, 1296, 1440, 1688], [1024, 1025, 1028, 1066], PH | PV),
    DisplayMode::new(100000, [1152, 1232, 1280, 1456], [870, 871, 874, 915], PH | PV),
];

// --- CTA-861 -----------------------------------------------------------------

const fn cea(clock: u32, h: [u16; 4], v: [u16; 4], flags: u32, aspect: Aspect) -> DisplayMode {
    DisplayMode::new(clock, h, v, flags).with_aspect(aspect)
}

/// The CTA-861 video identification codes this core knows, with their
/// modes. Short video descriptors naming other codes are ignored.
pub const CEA_VICS: &[(u8, DisplayMode)] = &[
    (1, cea(25175, [640, 656, 752, 800], [480, 490, 492, 525], NH | NV, Aspect::R4x3)),
    (2, cea(27000, [720, 736, 798, 858], [480, 489, 495, 525], NH | NV, Aspect::R4x3)),
    (3, cea(27000, [720, 736, 798, 858], [480, 489, 495, 525], NH | NV, Aspect::R16x9)),
    (4, cea(74250, [1280, 1390, 1430, 1650], [720, 725, 730, 750], PH | PV, Aspect::R16x9)),
    (5, cea(74250, [1920, 2008, 2052, 2200], [1080, 1084, 1094, 1125], PH | PV | IL, Aspect::R16x9)),
    (16, cea(148500, [1920, 2008, 2052, 2200], [1080, 1084, 1089, 1125], PH | PV, Aspect::R16x9)),
    (17, cea(27000, [720, 732, 796, 864], [576, 581, 586, 625], NH | NV, Aspect::R4x3)),
    (18, cea(27000, [720, 732, 796, 864], [576, 581, 586, 625], NH | NV, Aspect::R16x9)),
    (19, cea(74250, [1280, 1720, 1760, 1980], [720, 725, 730, 750], PH | PV, Aspect::R16x9)),
    (20, cea(74250, [1920, 2448, 2492, 2640], [1080, 1084, 1094, 1125], PH | PV | IL, Aspect::R16x9)),
    (31, cea(148500, [1920, 2448, 2492, 2640], [1080, 1084, 1089, 1125], PH | PV, Aspect::R16x9)),
    (32, cea(74250, [1920, 2558, 2602, 2750], [1080, 1084, 1089, 1125], PH | PV, Aspect::R16x9)),
    (33, cea(74250, [1920, 2448, 2492, 2640], [1080, 1084, 1089, 1125], PH | PV, Aspect::R16x9)),
    (34, cea(74250, [1920, 2008, 2052, 2200], [1080, 1084, 1089, 1125], PH | PV, Aspect::R16x9)),
    (95, cea(297000, [3840, 4016, 4104, 4400], [2160, 2168, 2178, 2250], PH | PV, Aspect::R16x9)),
    (97, cea(594000, [3840, 4016, 4104, 4400], [2160, 2168, 2178, 2250], PH | PV, Aspect::R16x9)),
];

/// Finds the mode of a CTA-861 video identification code.
pub fn find_vic(vic: u8) -> Option<DisplayMode> {
    CEA_VICS.iter().find(|(v, _)| *v == vic).map(|(_, m)| *m)
}

// --- CVT ---------------------------------------------------------------------

/// Computes a VESA Coordinated Video Timings mode with normal blanking
/// (CVT 1.1, progressive, no margins), as Linux's `drm_cvt_mode` without
/// the reduced-blanking and interlaced variants. Returns `None` for sizes
/// and rates the formula cannot handle.
pub fn cvt_mode(hdisplay: u16, vdisplay: u16, refresh: u16) -> Option<DisplayMode> {
    const CELL: u64 = 8;
    const MIN_VSYNC_BP_PS: u64 = 550_000_000;
    const MIN_V_PORCH: u64 = 3;
    const MIN_V_BPORCH: u64 = 6;
    if hdisplay < 8 || vdisplay == 0 || refresh == 0 {
        return None;
    }
    let h = u64::from(hdisplay) / CELL * CELL;
    let v = u64::from(vdisplay);
    let vsync: u64 = if v % 3 == 0 && v * 4 / 3 == h {
        4
    } else if v % 9 == 0 && v * 16 / 9 == h {
        5
    } else if v % 10 == 0 && v * 16 / 10 == h {
        6
    } else if (v % 4 == 0 && v * 5 / 4 == h) || (v % 9 == 0 && v * 15 / 9 == h) {
        7
    } else {
        10
    };
    // The estimated line period in picoseconds.
    let field_ps = 1_000_000_000_000u64 / u64::from(refresh);
    let hper = field_ps.checked_sub(MIN_VSYNC_BP_PS)? / (v + MIN_V_PORCH);
    if hper == 0 {
        return None;
    }
    let vsync_bp = (MIN_VSYNC_BP_PS / hper + 1).max(vsync + MIN_V_BPORCH);
    let vtotal = v + vsync_bp + MIN_V_PORCH;
    // The ideal blanking duty cycle in millionths of a percent:
    // C' - M' * H_PERIOD_EST(us) / 1000 with C' = 30 and M' = 300.
    let duty = 30_000_000u64.saturating_sub(300 * hper / 1000).max(20_000_000);
    let hblank = h * duty / ((100_000_000 - duty) * 2 * CELL) * 2 * CELL;
    let htotal = h + hblank;
    // The pixel clock in kHz, in steps of 250 kHz.
    let clock = htotal * 1_000_000_000 / hper / 250 * 250;
    let hsync = htotal * 8 / 100 / CELL * CELL;
    let hbp = hblank / 2;
    let hfp = hblank.checked_sub(hsync + hbp)?;
    let hsync_start = h + hfp;
    let n = |x: u64| u16::try_from(x).ok();
    let mut m = DisplayMode::new(
        u32::try_from(clock).ok()?,
        [n(h)?, n(hsync_start)?, n(hsync_start + hsync)?, n(htotal)?],
        [n(v)?, n(v + MIN_V_PORCH)?, n(v + MIN_V_PORCH + vsync)?, n(vtotal)?],
        NH | PV,
    );
    m.typ = uapi::DRM_MODE_TYPE_DRIVER;
    Some(m)
}

/// The modes a connector without EDID offers (Linux's
/// `drm_add_modes_noedid`): the DMT modes up to `max_w` by `max_h`, at
/// most 1024 by 768 by default, with 1024 by 768 at 60 Hz preferred when
/// present.
pub fn noedid_modes(max_w: u16, max_h: u16) -> Vec<DisplayMode> {
    let mut out = Vec::new();
    for t in DMT {
        if t.mode.hdisplay <= max_w && t.mode.vdisplay <= max_h {
            let mut m = t.mode;
            if m.hdisplay == 1024 && m.vdisplay == 768 && t.refresh == 60 {
                m.typ |= uapi::DRM_MODE_TYPE_PREFERRED;
            }
            out.push(m);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;

    #[test]
    fn names_and_refresh() {
        let m = find_dmt(1024, 768, 60).unwrap();
        assert_eq!(m.name_bytes(), b"1024x768");
        assert_eq!(m.vrefresh(), 60);
        assert_eq!(m.frame_ns(), Some(16_665_600));
        let i = find_vic(5).unwrap();
        assert_eq!(i.name_bytes(), b"1920x1080i");
        assert_eq!(i.vrefresh(), 60);
        assert_eq!(find_vic(97).unwrap().vrefresh(), 60);
        assert_eq!(find_vic(32).unwrap().vrefresh(), 24);
        for t in DMT {
            // The nominal rate names the mode; the real one may differ by a
            // fraction (640x480 "72 Hz" is 72.8 Hz).
            assert!(t.mode.vrefresh().abs_diff(u32::from(t.refresh)) <= 1, "{:?}", t.mode);
            assert!(t.mode.validate_basic().is_ok());
        }
        for (_, m) in CEA_VICS {
            assert!(m.validate_basic().is_ok());
        }
        for m in ESTABLISHED {
            assert!(m.validate_basic().is_ok());
        }
    }

    #[test]
    fn extreme_timings_do_not_overflow() {
        let mut m = DisplayMode::new(1, [1, 1, 1, u16::MAX], [1, 1, 1, u16::MAX], uapi::DRM_MODE_FLAG_DBLSCAN);
        m.vscan = u16::MAX;
        assert!(m.frame_ns().unwrap() > 1_000_000_000_000);
        assert_eq!(m.vrefresh(), 0);
        let fast = DisplayMode::new(u32::MAX, [1, 1, 1, 1], [1, 1, 1, 1], uapi::DRM_MODE_FLAG_INTERLACE);
        assert_eq!(fast.frame_ns(), Some(0));
        assert!(fast.vrefresh() > 0);
    }

    #[test]
    fn cvt_matches_the_vesa_generator() {
        // `cvt 1024 768 60`: 63.50 MHz, 1024 1072 1176 1328, 768 771 775 798.
        let m = cvt_mode(1024, 768, 60).unwrap();
        assert_eq!(m.clock, 63500);
        assert_eq!([m.hdisplay, m.hsync_start, m.hsync_end, m.htotal], [1024, 1072, 1176, 1328]);
        assert_eq!([m.vdisplay, m.vsync_start, m.vsync_end, m.vtotal], [768, 771, 775, 798]);
        assert_eq!(m.flags, NH | PV);
        // `cvt 1920 1080 60`: 173.00 MHz, 1920 2048 2248 2576, 1080 1083 1088 1120.
        let m = cvt_mode(1920, 1080, 60).unwrap();
        assert_eq!(m.clock, 173000);
        assert_eq!([m.hdisplay, m.hsync_start, m.hsync_end, m.htotal], [1920, 2048, 2248, 2576]);
        assert_eq!([m.vdisplay, m.vsync_start, m.vsync_end, m.vtotal], [1080, 1083, 1088, 1120]);
        assert!(cvt_mode(0, 768, 60).is_none());
        assert!(cvt_mode(1024, 768, 0).is_none());
    }

    #[test]
    fn umode_round_trip_and_validation() {
        let mut m = find_vic(16).unwrap();
        m.typ |= uapi::DRM_MODE_TYPE_PREFERRED;
        let u = m.to_umode(true);
        assert_eq!(u.flags & uapi::DRM_MODE_FLAG_PIC_AR_MASK, uapi::DRM_MODE_FLAG_PIC_AR_16_9);
        assert_eq!(u.vrefresh, 60);
        assert_eq!(DisplayMode::from_umode(&u).unwrap(), m);
        let plain = m.to_umode(false);
        assert_eq!(plain.flags & uapi::DRM_MODE_FLAG_PIC_AR_MASK, 0);
        let mut bad = u;
        bad.flags |= uapi::DRM_MODE_FLAG_PIC_AR_MASK;
        assert_eq!(DisplayMode::from_umode(&bad), Err(Errno::EINVAL));
        let mut bad = u;
        bad.clock = 0x8000_0000;
        assert_eq!(DisplayMode::from_umode(&bad), Err(ERANGE));
        let mut bad = u;
        bad.hsync_start = bad.hdisplay - 1;
        assert_eq!(DisplayMode::from_umode(&bad), Err(Errno::EINVAL));
        let mut bad = u;
        bad.vtotal = bad.vsync_end - 1;
        assert_eq!(DisplayMode::from_umode(&bad), Err(Errno::EINVAL));
        let mut bad = u;
        bad.clock = 0;
        assert_eq!(DisplayMode::from_umode(&bad), Err(Errno::EINVAL));
        let mut odd = u;
        odd.type_ = 0xffff_ffff;
        assert_eq!(DisplayMode::from_umode(&odd).unwrap().typ, uapi::DRM_MODE_TYPE_ALL);
        let mut long = u;
        long.name = [b'a' as core::ffi::c_char; NAME_LEN];
        assert_eq!(DisplayMode::from_umode(&long).unwrap().name_bytes().len(), NAME_LEN - 1);
    }

    #[test]
    fn list_order_limits_and_duplicates() {
        let limits = ModeLimits { max_width: 1920, max_height: 1200, max_clock: 0, interlace: false, dblscan: false };
        let mut pref = find_dmt(1024, 768, 60).unwrap();
        pref.typ |= uapi::DRM_MODE_TYPE_PREFERRED;
        let mut v = std::vec![
            find_dmt(640, 480, 60).unwrap(),
            find_vic(5).unwrap(),
            find_dmt(1920, 1080, 60).unwrap(),
            find_dmt(1024, 768, 60).unwrap(),
            pref,
            find_dmt(1024, 768, 75).unwrap(),
            find_vic(97).unwrap(),
        ];
        finish_list(&mut v, &limits);
        let names: std::vec::Vec<_> = v.iter().map(|m| (m.hdisplay, m.vdisplay, m.vrefresh())).collect();
        assert_eq!(names, [(1024, 768, 60), (1920, 1080, 60), (1024, 768, 75), (640, 480, 60)]);
        assert!(v[0].typ & uapi::DRM_MODE_TYPE_PREFERRED != 0, "preferred bit merged into the first copy");
        let small = noedid_modes(1024, 768);
        assert!(small.iter().all(|m| m.hdisplay <= 1024 && m.vdisplay <= 768));
        assert_eq!(small.iter().filter(|m| m.typ & uapi::DRM_MODE_TYPE_PREFERRED != 0).count(), 1);
    }
}
