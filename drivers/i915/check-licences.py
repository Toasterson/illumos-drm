#!/usr/bin/env python3
#
# SPDX-License-Identifier: MIT
# Copyright 2026 The illumos-drm authors
#
# Checks that Linux files built into i915n are MIT-compatible: an SPDX
# expression that is MIT or offers MIT as an alternative ("GPL-2.0 OR
# MIT"; an "AND" or a missing MIT refuses), or, without SPDX, the MIT
# permission notice or the HPND one ("Permission to use, copy, modify,
# distribute, and sell"). Prints every refused file and exits 1 if any.
#
#   check-licences.py <linux tree> <file of tree-relative paths>
import re
import sys

root, listing = sys.argv[1], sys.argv[2]
SPDX = re.compile(r"SPDX-License-Identifier:\s*(.+?)\s*(\*/)?\s*$")
MIT_NOTICE = "Permission is hereby granted, free of charge"
HPND_NOTICE = "Permission to use, copy, modify, distribute, and sell"


def spdx_ok(expr):
    e = expr.replace("(", " ").replace(")", " ")
    words = e.split()
    if "AND" in words or "WITH" in words:
        return False
    alternatives = [w for w in words if w != "OR"]
    return "MIT" in alternatives


bad = 0
n = 0
for rel in open(listing, encoding="utf-8"):
    rel = rel.strip()
    if not rel:
        continue
    n += 1
    try:
        head = open(f"{root}/{rel}", encoding="utf-8", errors="replace").read(6000)
    except OSError as e:
        print(f"check-licences: {rel}: {e}")
        bad += 1
        continue
    m = None
    for line in head.splitlines()[:60]:
        m = SPDX.search(line)
        if m:
            break
    if m:
        if not spdx_ok(m.group(1)):
            print(f"check-licences: {rel}: SPDX {m.group(1)!r} is not MIT-compatible")
            bad += 1
    elif MIT_NOTICE not in head and HPND_NOTICE not in head:
        print(f"check-licences: {rel}: no SPDX tag and no MIT/HPND notice")
        bad += 1
print(f"check-licences: {n} files, {bad} refused")
sys.exit(1 if bad else 0)
