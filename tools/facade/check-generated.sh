#!/bin/sh
#
# SPDX-License-Identifier: MIT
# Copyright 2026 The illumos-drm authors
#
# Checks that the files generated from the facade headers are current:
#
#   always: gen_facade.py rerun on crates/drm-facade-sys/src/bindings.rs
#     reproduces modules/drm_core/drm_core.mapfile,
#     include/drm/drm_facade.mapfile, tools/facade/facade_layout.c and
#     xtask/src/facade_table.rs;
#   with ILLUMOS_GATE set and bindgen installed: generate.sh rerun on
#     include/drm reproduces bindings.rs too.
#
# Works on a scratch copy; the checkout is not changed. Exit status 1 on
# a difference (shown as a diff).
#
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/facade-check.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

FILES="crates/drm-facade-sys/src/bindings.rs modules/drm_core/drm_core.mapfile
include/drm/drm_facade.mapfile tools/facade/facade_layout.c
xtask/src/facade_table.rs"

# A copy of what the generators read and write.
(cd "$ROOT" && tar cf - include tools/facade crates/drm-facade-sys/src \
    modules/drm_core/drm_core.mapfile xtask/src/facade_table.rs \
    rustfmt.toml) | (cd "$TMP" && tar xf -)

if [ -n "${ILLUMOS_GATE:-}" ] && command -v "${BINDGEN:-bindgen}" >/dev/null 2>&1; then
	echo "check-generated: regenerating bindings.rs with bindgen from include/drm"
	# The header line names the gate commit; compare without it.
	sh "$TMP/tools/facade/generate.sh" >/dev/null
else
	echo "check-generated: no ILLUMOS_GATE or bindgen: bindings.rs is taken as is"
	python3 "$TMP/tools/facade/gen_facade.py" "$TMP"
	rustfmt --edition 2024 --config-path "$TMP/rustfmt.toml" "$TMP/xtask/src/facade_table.rs"
fi

rc=0
for f in $FILES; do
	if ! diff -u -I '^// Target: ' "$ROOT/$f" "$TMP/$f"; then
		echo "check-generated: $f is out of date; rerun tools/facade/generate.sh" >&2
		rc=1
	fi
done
[ $rc -eq 0 ] && echo "check-generated: every generated file is current"
exit $rc
