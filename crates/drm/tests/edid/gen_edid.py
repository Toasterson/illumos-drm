#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Generates the EDID test blobs of illumos-drm.

Provenance: every blob in this directory is synthesized by this script from
the published VESA EDID 1.3/1.4 and CTA-861 layouts and VESA DMT/CTA timing
values; none is captured from a real display or copied from another project,
so they carry the MPL-2.0 like the rest of the repository. Run it from this
directory to regenerate them; the output is deterministic.

  rdrmnull.bin               EDID 1.4, one block: the virtual display of the
                             rdrmnull test driver (also built into it).
  hdmi-1080p.bin             EDID 1.3 with a CTA-861 extension: HDMI vendor
                             block, VICs 16 (native), 4, 19, 31, 97 and an
                             unknown one (200), and a detailed timing in the
                             extension.
  dvi-1600x1200-edid13.bin   EDID 1.3, one block: standard timings inside and
                             outside the DMT table, 1024x768 interlaced, and a
                             CVT code descriptor.
"""


def pnp(vendor):
    v = 0
    for c in vendor:
        v = (v << 5) | (ord(c) - ord('@'))
    return [v >> 8, v & 0xff]


def dtd(clock_khz, h, v, flags, mm=(0, 0)):
    """An 18-byte detailed timing: h/v = (active, sync start, sync end,
    total); flags: 'P'/'N' for h and v polarity, e.g. "PN"."""
    ha, hss, hse, ht = h
    va, vss, vse, vt = v
    hbl, vbl = ht - ha, vt - va
    hso, hspw, vso, vspw = hss - ha, hse - hss, vss - va, vse - vss
    c = clock_khz // 10
    misc = 0x18 | (0x02 if flags[0] == 'P' else 0) | (0x04 if flags[1] == 'P' else 0)
    return [
        c & 0xff, c >> 8,
        ha & 0xff, hbl & 0xff, ((ha >> 8) << 4) | (hbl >> 8),
        va & 0xff, vbl & 0xff, ((va >> 8) << 4) | (vbl >> 8),
        hso & 0xff, hspw & 0xff, ((vso & 0xf) << 4) | (vspw & 0xf),
        ((hso >> 8) << 6) | ((hspw >> 8) << 4) | ((vso >> 4) << 2) | (vspw >> 4),
        mm[0] & 0xff, mm[1] & 0xff, ((mm[0] >> 8) << 4) | (mm[1] >> 8),
        0, 0, misc,
    ]


def text(tag, s):
    b = list(s.encode()) + [0x0a]
    b += [0x20] * (13 - len(b))
    return [0, 0, 0, tag, 0] + b[:13]


def dummy():
    return [0, 0, 0, 0x10, 0] + [0] * 13


def std(h, aspect, rate):
    a = {'16:10': 0, '4:3': 1, '5:4': 2, '16:9': 3}[aspect]
    return [h // 8 - 31, (a << 6) | (rate - 60)]


def finish(block):
    assert len(block) == 127, len(block)
    return block + [(-sum(block)) & 0xff]


def base(vendor, product, serial, version, cm, est, stds, descs, feature, ext=0):
    b = [0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00]
    b += pnp(vendor) + [product & 0xff, product >> 8]
    b += [serial & 0xff, (serial >> 8) & 0xff, (serial >> 16) & 0xff, serial >> 24]
    b += [1, 36, 1, version]          # week, year (2026), version 1.x
    b += [0x80 | 0x20, cm[0], cm[1], 120, feature]   # digital 8 bpc, size, gamma 2.2
    b += [0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54]  # sRGB-ish chromaticity
    b += est
    s = [x for t in stds for x in t]
    s += [0x01, 0x01] * (8 - len(stds))
    b += s
    for d in descs:
        assert len(d) == 18
        b += d
    b += [ext]
    return finish(b)


DMT_1024 = (65000, (1024, 1048, 1184, 1344), (768, 771, 777, 806), "NN")
DMT_1280x1024 = (108000, (1280, 1328, 1440, 1688), (1024, 1025, 1028, 1066), "PP")
CEA_1080P = (148500, (1920, 2008, 2052, 2200), (1080, 1084, 1089, 1125), "PP")
DMT_1600 = (162000, (1600, 1664, 1856, 2160), (1200, 1201, 1204, 1250), "PP")


def rdrmnull():
    # Established: 640x480@60 (0x23 bit 5), 800x600@60 (0x23 bit 0),
    # 1024x768@60 (0x24 bit 3).
    est = [0x21, 0x08, 0x00]
    descs = [dtd(*DMT_1024, mm=(340, 270)), text(0xfc, "rdrmnull"), text(0xff, "0"), dummy()]
    # Feature 0x06: sRGB default, preferred timing mode.
    return base("RDN", 0x0001, 1, 4, (34, 27), est, [std(1280, '5:4', 60)], descs, 0x06)


def hdmi_tv():
    est = [0x20, 0x00, 0x00]
    descs = [dtd(*CEA_1080P, mm=(1210, 680)), text(0xfc, "illumos TV"),
             [0, 0, 0, 0xfd, 0, 23, 76, 15, 80, 15, 0, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20], dummy()]
    b0 = base("ILL", 0x1080, 42, 3, (121, 68), est, [], descs, 0x0a, ext=1)
    vdb = [0x40 | 6, 0x80 | 16, 4, 19, 31, 97, 200]
    hdmi = [0x60 | 6, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x00]
    blocks = vdb + hdmi
    d = 4 + len(blocks)
    ext = [0x02, 0x03, d, 0x70 | 1] + blocks + dtd(*DMT_1280x1024)
    ext += [0] * (127 - len(ext))
    return b0 + finish(ext)


def dvi():
    # Established: 640x480@60, 800x600@60, 1024x768@87i (0x24 bit 4),
    # 1024x768@60.
    est = [0x21, 0x18, 0x00]
    stds = [std(1152, '16:10', 60), std(1280, '5:4', 60), std(1280, '5:4', 75)]
    cvt = [0, 0, 0, 0xf8, 0, 0x01, 0x57, 0x28, 0x08] + [0] * 9
    descs = [dtd(*DMT_1600, mm=(408, 306)), text(0xfc, "Old DVI"), cvt, dummy()]
    # EDID 1.3 with the preferred-timing feature bit.
    return base("OLD", 0x1600, 7, 3, (41, 31), est, stds, descs, 0x0a)


for name, blob in [("rdrmnull.bin", rdrmnull()), ("hdmi-1080p.bin", hdmi_tv()),
                   ("dvi-1600x1200-edid13.bin", dvi())]:
    with open(name, "wb") as f:
        f.write(bytes(blob))
