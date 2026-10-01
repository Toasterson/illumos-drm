// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! EDID parsing: from the bytes a display reports to a list of modes.
//!
//! The parser follows Linux's `drm_edid.c` in what it accepts and in the
//! order it adds modes: detailed timings (the first one preferred), CVT
//! codes, standard timings, established timings, and the short video
//! descriptors of a CTA-861 extension. It reads:
//!
//! * the base block: header (up to two wrong bytes are tolerated and
//!   repaired, as Linux does), checksum, vendor and product, version,
//!   physical size, established and standard timings, and four 18-byte
//!   descriptors (detailed timings, the display name, standard timings and
//!   CVT codes);
//! * CTA-861 extensions (tag 0x02): the video data block (VICs from
//!   [`modes::CEA_VICS`]), the HDMI vendor block, and the detailed timings
//!   after the data blocks.
//!
//! Extensions with a wrong checksum are skipped. Not read: other extension
//! types (DisplayID, block maps), established timings III, reduced-blanking
//! CVT codes and GTF (standard timings that are not DMT modes are computed
//! with CVT instead of GTF on EDID 1.3 displays), stereo detailed timings,
//! and the HDMI 2.x and colour data blocks.

use alloc::vec::Vec;

use drm_uapi as uapi;

use super::modes::{self, Aspect, DisplayMode};

/// The size of one EDID block.
pub const BLOCK: usize = 128;
/// The most extension blocks this parser reads (the EDID limit is 255;
/// Linux's EDID loader stops at 254). More are ignored.
pub const MAX_EXTENSIONS: usize = 7;

const HEADER: [u8; 8] = [0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00];

/// Why an EDID was rejected.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EdidError {
    /// Shorter than one block, or not a whole number of blocks.
    Length,
    /// More than two bytes of the fixed header are wrong.
    Header,
    /// The base block's bytes do not sum to 0 modulo 256.
    Checksum,
}

/// What an EDID says about a display.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EdidInfo {
    /// The three-letter PNP vendor ID.
    pub vendor: [u8; 3],
    /// The product code.
    pub product: u16,
    /// The serial number.
    pub serial: u32,
    /// EDID version and revision.
    pub version: (u8, u8),
    /// Image width in mm (0 if unknown).
    pub width_mm: u32,
    /// Image height in mm (0 if unknown).
    pub height_mm: u32,
    /// The monitor name descriptor, without trailing newline or padding.
    pub name: Vec<u8>,
    /// A CTA-861 extension with an HDMI vendor block was found.
    pub hdmi: bool,
    /// The modes, in the order they were found, before validation and
    /// sorting (see [`modes::finish_list`]).
    pub modes: Vec<DisplayMode>,
}

fn checksum_ok(b: &[u8]) -> bool {
    b.iter().fold(0u8, |a, &x| a.wrapping_add(x)) == 0
}

fn byte(b: &[u8], i: usize) -> u8 {
    b.get(i).copied().unwrap_or(0)
}

/// Checks the base block and returns the blocks worth reading: the base
/// block (with its header repaired) and every extension whose checksum is
/// right, up to [`MAX_EXTENSIONS`].
pub fn blocks(edid: &[u8]) -> Result<Vec<[u8; BLOCK]>, EdidError> {
    if edid.len() < BLOCK || !edid.len().is_multiple_of(BLOCK) {
        return Err(EdidError::Length);
    }
    let mut base = [0u8; BLOCK];
    base.copy_from_slice(edid.get(..BLOCK).ok_or(EdidError::Length)?);
    let wrong = base.iter().zip(HEADER.iter()).filter(|(a, b)| a != b).count();
    if wrong > 2 {
        return Err(EdidError::Header);
    }
    if wrong != 0 {
        // Repaired as Linux's drm_edid_block_valid does. The checksum still
        // covers the bytes as they came.
        let sum = base.iter().fold(0u8, |a, &x| a.wrapping_add(x));
        if sum != 0 {
            return Err(EdidError::Checksum);
        }
        base.get_mut(..8).ok_or(EdidError::Length)?.copy_from_slice(&HEADER);
    } else if !checksum_ok(&base) {
        return Err(EdidError::Checksum);
    }
    let mut out = Vec::new();
    out.push(base);
    let count = usize::from(byte(&base, 0x7e)).min(MAX_EXTENSIONS);
    for i in 1..=count {
        let Some(b) = edid.get(i * BLOCK..(i + 1) * BLOCK) else { break };
        if checksum_ok(b) {
            let mut blk = [0u8; BLOCK];
            blk.copy_from_slice(b);
            out.push(blk);
        }
    }
    Ok(out)
}

/// Parses an EDID. See the module documentation for what is read.
pub fn parse(edid: &[u8]) -> Result<EdidInfo, EdidError> {
    let blocks = blocks(edid)?;
    let base = blocks.first().ok_or(EdidError::Length)?;
    let v = u16::from_be_bytes([byte(base, 8), byte(base, 9)]);
    let vendor = [b'@' + ((v >> 10) & 0x1f) as u8, b'@' + ((v >> 5) & 0x1f) as u8, b'@' + (v & 0x1f) as u8];
    let version = (byte(base, 0x12), byte(base, 0x13));
    let mut info = EdidInfo {
        vendor,
        product: u16::from_le_bytes([byte(base, 10), byte(base, 11)]),
        serial: u32::from_le_bytes([byte(base, 12), byte(base, 13), byte(base, 14), byte(base, 15)]),
        version,
        width_mm: u32::from(byte(base, 0x15)) * 10,
        height_mm: u32::from(byte(base, 0x16)) * 10,
        name: Vec::new(),
        hdmi: false,
        modes: Vec::new(),
    };
    // EDID 1.3 and later: the first detailed timing is the preferred mode
    // (1.3 announces it in the feature byte, 1.4 always has it).
    let preferred = version.1 >= 4 || byte(base, 0x18) & 0x02 != 0 || version.0 > 1;
    let mut cvt = Vec::new();
    let mut std = Vec::new();
    let mut first = true;
    // Detailed timings and display descriptors of the base block.
    for d in base.get(0x36..0x7e).unwrap_or(&[]).chunks_exact(18) {
        descriptor(d, &mut info, &mut first, preferred, &mut cvt, &mut std);
    }
    // Detailed timings of CTA extensions come next, as in Linux, which
    // walks every detailed timing block before the other mode sources.
    let mut vics = Vec::new();
    for ext in blocks.iter().skip(1) {
        if byte(ext, 0) != 0x02 {
            continue;
        }
        let dtd = usize::from(byte(ext, 2));
        if dtd >= 4 && byte(ext, 1) >= 3 {
            data_blocks(ext.get(4..dtd.min(BLOCK)).unwrap_or(&[]), &mut vics, &mut info.hdmi);
        }
        if dtd >= 4 {
            for d in ext.get(dtd..BLOCK - 1).unwrap_or(&[]).chunks_exact(18) {
                if d.first().copied().unwrap_or(0) == 0 && d.get(1).copied().unwrap_or(0) == 0 {
                    break;
                }
                descriptor(d, &mut info, &mut first, preferred, &mut cvt, &mut std);
            }
        }
    }
    info.modes.extend(cvt);
    // Standard timings: eight in the base block, plus those of 0xFA
    // descriptors.
    let mut std_all: Vec<[u8; 2]> = base.get(0x26..0x36).unwrap_or(&[]).chunks_exact(2).map(|c| [c[0], c[1]]).collect();
    std_all.extend(std);
    for t in std_all {
        if let Some(m) = standard_timing(t, version) {
            info.modes.push(m);
        }
    }
    // Established timings.
    let est = [byte(base, 0x23), byte(base, 0x24), byte(base, 0x25) & 0x80];
    for (i, m) in modes::ESTABLISHED.iter().enumerate() {
        let (b, bit) = (i / 8, 7 - (i % 8));
        if est.get(b).is_some_and(|x| x & (1 << bit) != 0) {
            info.modes.push(*m);
        }
    }
    for vic in vics {
        if let Some(m) = modes::find_vic(vic) {
            info.modes.push(m);
        }
    }
    Ok(info)
}

/// Reads one 18-byte descriptor: a detailed timing, or a display
/// descriptor (name, standard timings, CVT codes).
fn descriptor(
    d: &[u8],
    info: &mut EdidInfo,
    first: &mut bool,
    preferred: bool,
    cvt: &mut Vec<DisplayMode>,
    std: &mut Vec<[u8; 2]>,
) {
    if byte(d, 0) != 0 || byte(d, 1) != 0 {
        if let Some(mut m) = detailed_timing(d) {
            if *first && preferred {
                m.typ |= uapi::DRM_MODE_TYPE_PREFERRED;
            }
            *first = false;
            info.modes.push(m);
        }
        return;
    }
    match byte(d, 3) {
        0xfc => {
            let text = d.get(5..18).unwrap_or(&[]);
            let end = text.iter().position(|&c| c == b'\n').unwrap_or(text.len());
            let t = text.get(..end).unwrap_or(&[]);
            let t = match t.iter().rposition(|&c| c != b' ') {
                Some(p) => t.get(..=p).unwrap_or(&[]),
                None => &[],
            };
            info.name = t.to_vec();
        }
        0xfa => {
            for c in d.get(5..17).unwrap_or(&[]).chunks_exact(2) {
                std.push([c[0], c[1]]);
            }
        }
        0xf8 => {
            for c in d.get(6..18).unwrap_or(&[]).chunks_exact(3) {
                cvt_code(c, cvt);
            }
        }
        _ => {}
    }
}

/// Decodes an 18-byte detailed timing descriptor (`drm_mode_detailed`).
/// Stereo timings, timings smaller than 64 by 64 and timings without sync
/// pulses are skipped, as Linux does.
pub fn detailed_timing(d: &[u8]) -> Option<DisplayMode> {
    let b = |i: usize| u16::from(byte(d, i));
    let clock = u32::from(u16::from_le_bytes([byte(d, 0), byte(d, 1)])) * 10;
    let hactive = b(2) | (b(4) & 0xf0) << 4;
    let hblank = b(3) | (b(4) & 0x0f) << 8;
    let vactive = b(5) | (b(7) & 0xf0) << 4;
    let vblank = b(6) | (b(7) & 0x0f) << 8;
    let hso = b(8) | (b(11) & 0xc0) << 2;
    let hspw = b(9) | (b(11) & 0x30) << 4;
    let vso = (b(10) >> 4) | (b(11) & 0x0c) << 2;
    let vspw = (b(10) & 0x0f) | (b(11) & 0x03) << 4;
    let misc = byte(d, 17);
    if clock == 0 || misc & 0x20 != 0 || hactive < 64 || vactive < 64 || hspw == 0 || vspw == 0 {
        return None;
    }
    let hss = hactive + hso;
    let hse = hss + hspw;
    let mut htotal = hactive + hblank;
    let vss = vactive + vso;
    let vse = vss + vspw;
    let mut vtotal = vactive + vblank;
    // Some displays report a total inside the sync pulse; Linux extends it.
    if hse > htotal {
        htotal = hse + 1;
    }
    if vse > vtotal {
        vtotal = vse + 1;
    }
    let mut flags = if misc & 0x02 != 0 { uapi::DRM_MODE_FLAG_PHSYNC } else { uapi::DRM_MODE_FLAG_NHSYNC };
    flags |= if misc & 0x04 != 0 { uapi::DRM_MODE_FLAG_PVSYNC } else { uapi::DRM_MODE_FLAG_NVSYNC };
    let mut v = [vactive, vss, vse, vtotal];
    if misc & 0x80 != 0 {
        flags |= uapi::DRM_MODE_FLAG_INTERLACE;
        v = [vactive * 2, vss * 2, vse * 2, (vtotal * 2) | 1];
    }
    let mut m = DisplayMode::new(clock, [hactive, hss, hse, htotal], v, flags);
    m.typ = uapi::DRM_MODE_TYPE_DRIVER;
    Some(m)
}

/// Decodes a two-byte standard timing. Unused entries (0x0101, 0x0000,
/// 0x2020) give `None`.
pub fn standard_timing(t: [u8; 2], version: (u8, u8)) -> Option<DisplayMode> {
    if matches!(t, [0x01, 0x01] | [0x00, 0x00] | [0x20, 0x20]) {
        return None;
    }
    let h = (u16::from(t[0]) + 31) * 8;
    let refresh = u16::from(t[1] & 0x3f) + 60;
    let v = match t[1] >> 6 {
        // 16:10 since EDID 1.3, 1:1 before.
        0 if version.0 > 1 || version.1 >= 3 => h * 10 / 16,
        0 => h,
        1 => h * 3 / 4,
        2 => h * 4 / 5,
        _ => h * 9 / 16,
    };
    // 1360x765 is how 1366x768 displays describe themselves; the DMT has
    // 1360x768 for it (Linux's "HDTV hack").
    let v = if h == 1360 && v == 765 { 768 } else { v };
    modes::find_dmt(h, v, refresh).or_else(|| modes::cvt_mode(h, v, refresh))
}

/// Decodes a three-byte CVT code of an 0xF8 descriptor: the normal-blanking
/// modes at 50, 60, 75 and 85 Hz that it marks as supported.
fn cvt_code(c: &[u8], out: &mut Vec<DisplayMode>) {
    let (c0, c1, c2) = (u32::from(byte(c, 0)), u32::from(byte(c, 1)), byte(c, 2));
    if c0 == 0 && c1 == 0 && c2 == 0 {
        return;
    }
    let height = ((c0 | (c1 & 0xf0) << 4) + 1) * 2;
    let width = match c1 & 0x0c {
        0x00 => height * 4 / 3,
        0x04 => height * 16 / 9,
        0x08 => height * 16 / 10,
        _ => height * 15 / 9,
    };
    // Bits 1 to 4: 85, 75, 60 and 50 Hz; bit 0 is 60 Hz with reduced
    // blanking, which is not implemented (Linux skips it too).
    for (bit, rate) in [(1, 85), (2, 75), (3, 60), (4, 50)] {
        let (Ok(w), Ok(h)) = (u16::try_from(width), u16::try_from(height)) else { return };
        if c2 & (1 << bit) != 0
            && let Some(m) = modes::cvt_mode(w, h, rate)
        {
            out.push(m);
        }
    }
}

/// Walks the data block collection of a CTA-861 extension.
fn data_blocks(mut b: &[u8], vics: &mut Vec<u8>, hdmi: &mut bool) {
    while let Some((&hdr, rest)) = b.split_first() {
        let len = usize::from(hdr & 0x1f);
        let Some(payload) = rest.get(..len) else { return };
        match hdr >> 5 {
            // Video data block: short video descriptors.
            2 => {
                for &svd in payload {
                    let vic = if (1..=64).contains(&svd) || (129..=192).contains(&svd) { svd & 0x7f } else { svd };
                    vics.push(vic);
                }
            }
            // Vendor-specific block: the HDMI licensing OUI 00-0C-03.
            3 if payload.get(..3) == Some(&[0x03, 0x0c, 0x00]) => *hdmi = true,
            _ => {}
        }
        b = rest.get(len..).unwrap_or(&[]);
    }
}

/// The aspect ratio a CTA mode is announced with, for tests and drivers.
pub fn vic_aspect(vic: u8) -> Option<Aspect> {
    modes::find_vic(vic).map(|m| m.aspect)
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use crate::kms::modes::ModeLimits;

    fn load(name: &str) -> std::vec::Vec<u8> {
        let p = std::format!("{}/tests/edid/{name}", env!("CARGO_MANIFEST_DIR"));
        std::fs::read(&p).unwrap_or_else(|e| panic!("{p}: {e}"))
    }

    fn summary(info: &EdidInfo) -> std::vec::Vec<(u16, u16, u32, bool)> {
        let mut m = info.modes.clone();
        let limits = ModeLimits { max_width: 4096, max_height: 4096, max_clock: 0, interlace: true, dblscan: false };
        modes::finish_list(&mut m, &limits);
        m.iter().map(|x| (x.hdisplay, x.vdisplay, x.vrefresh(), x.typ & uapi::DRM_MODE_TYPE_PREFERRED != 0)).collect()
    }

    #[test]
    fn rdrmnull_edid() {
        let e = load("rdrmnull.bin");
        let info = parse(&e).unwrap();
        assert_eq!(&info.vendor, b"RDN");
        assert_eq!(info.version, (1, 4));
        assert_eq!(info.name, b"rdrmnull");
        assert_eq!((info.width_mm, info.height_mm), (340, 270));
        assert!(!info.hdmi);
        let s = summary(&info);
        assert_eq!(s[0], (1024, 768, 60, true));
        assert!(s.contains(&(1280, 1024, 60, false)), "{s:?}");
        assert!(s.contains(&(800, 600, 60, false)));
        assert!(s.contains(&(640, 480, 60, false)));
    }

    #[test]
    fn hdmi_tv_with_cea_extension() {
        let e = load("hdmi-1080p.bin");
        let info = parse(&e).unwrap();
        assert!(info.hdmi);
        assert_eq!(info.name, b"illumos TV");
        let s = summary(&info);
        assert_eq!(s[0], (1920, 1080, 60, true));
        // VICs 4, 16, 19, 31, 97 and a DTD in the extension (1280x1024).
        for want in [(1280, 720, 60), (1280, 720, 50), (1920, 1080, 50), (3840, 2160, 60), (1280, 1024, 60)] {
            assert!(s.iter().any(|m| (m.0, m.1, m.2) == want), "{want:?} missing from {s:?}");
        }
        // VIC 16 and the preferred DTD have the same timings but VIC 16
        // carries a 16:9 aspect ratio, so both stay, as in Linux; clients
        // without the aspect-ratio capability see one (connector exposure).
        assert_eq!(s.iter().filter(|m| (m.0, m.1, m.2) == (1920, 1080, 60)).count(), 2);
        // An unknown VIC (200) is ignored.
        assert!(info.modes.iter().all(|m| m.hdisplay != 0));
    }

    #[test]
    fn edid_13_standard_timings_and_cvt() {
        let e = load("dvi-1600x1200-edid13.bin");
        let info = parse(&e).unwrap();
        assert_eq!(info.version, (1, 3));
        let s = summary(&info);
        assert_eq!(s[0], (1600, 1200, 60, true));
        // A standard timing outside the DMT table becomes a CVT mode.
        assert!(s.iter().any(|m| (m.0, m.1, m.2) == (1152, 720, 60)), "{s:?}");
        // CVT code descriptor: 1920x1200 at 60 Hz.
        assert!(s.iter().any(|m| (m.0, m.1) == (1920, 1200)), "{s:?}");
        // 1024x768@87i is filtered only by limits that exclude interlace.
        assert!(s.iter().any(|m| (m.0, m.1, m.2) == (1024, 768, 87)));
    }

    #[test]
    fn broken_edids() {
        let good = load("rdrmnull.bin");
        assert_eq!(parse(&good[..100]), Err(EdidError::Length));
        let mut bad = good.clone();
        bad[0x40] ^= 0x55;
        assert_eq!(parse(&bad), Err(EdidError::Checksum));
        let mut hdr = good.clone();
        hdr[1] = 0; // one wrong header byte; fix the checksum
        hdr[127] = hdr[127].wrapping_add(0xff);
        assert!(parse(&hdr).is_ok(), "one wrong header byte is repaired");
        let mut hdr3 = good.clone();
        hdr3[1] = 0;
        hdr3[2] = 0;
        hdr3[3] = 0;
        assert_eq!(parse(&hdr3), Err(EdidError::Header));
        // A corrupt extension is skipped, the base block still counts.
        let mut tv = load("hdmi-1080p.bin");
        tv[200] ^= 1;
        let info = parse(&tv).unwrap();
        assert!(!info.hdmi);
        assert!(info.modes.iter().any(|m| m.hdisplay == 1920));
    }

    #[test]
    fn large_cvt_codes() {
        // A 12-bit height code: 8192 lines, 13107 pixels at 16:10; no
        // arithmetic may overflow on the way.
        let mut v = std::vec::Vec::new();
        cvt_code(&[0xff, 0xf8, 0x1e], &mut v);
        cvt_code(&[0xff, 0xfc, 0x1e], &mut v);
        assert!(v.iter().all(|m| m.vdisplay == 8192));
    }

    #[test]
    fn detailed_timing_fields() {
        // 1024x768@60 (65 MHz) encoded by hand.
        let d = [
            0x64, 0x19, 0x00, 0x40, 0x41, 0x00, 0x26, 0x30, 0x18, 0x88, 0x36, 0x00, 0x54, 0x0e, 0x11, 0x00, 0x00, 0x18,
        ];
        let m = detailed_timing(&d).unwrap();
        assert_eq!(m, modes::find_dmt(1024, 768, 60).unwrap());
        let mut stereo = d;
        stereo[17] |= 0x20;
        assert!(detailed_timing(&stereo).is_none());
        let mut nosync = d;
        nosync[9] = 0;
        nosync[11] &= !0x30;
        assert!(detailed_timing(&nosync).is_none());
        assert_eq!(vic_aspect(3), Some(Aspect::R16x9));
    }
}
