#!/bin/bash
#
# SPDX-License-Identifier: MIT
# Copyright 2026 The illumos-drm authors
#
# Writes the licence file of the i915n package: the copyright lines of
# every Linux file compiled into i915n (display included), then the MIT
# permission notice they carry (SPDX "MIT" or the notice in full) and the HPND notice of the files
# under it. The files are those of `inputs` (build.sh's linux-inputs.txt:
# every Linux file the compiler read, which check-licences.py accepted);
# without it, the TUs of tus.txt and display-tus.txt, i915's own headers
# and mit-headers.txt.
#
#   drivers/i915/gen-license.sh <patched Linux tree> <output file> [inputs]
#
# MODULE=drm_kms writes misc/drm_kms' licence file instead (inputs: the
# linux-inputs.txt of carried/drm_kms/build.sh).
set -euo pipefail
PATH=/usr/gnu/bin:$PATH
HERE=$(cd "$(dirname "$0")" && pwd)
LINUX=${1:?usage: gen-license.sh <linux> <out>}
out=${2:?usage: gen-license.sh <linux> <out>}
I915=drivers/gpu/drm/i915

inputs=${3:-}
if [ "${MODULE:-i915n}" = drm_kms ] && [ -z "$inputs" ]; then
	echo "gen-license: MODULE=drm_kms needs carried/drm_kms/build.sh's linux-inputs.txt" >&2
	exit 1
fi
if [ -n "$inputs" ]; then
	# The tree's copies of the MIT headers stand for their originals.
	sed 's|^\.illumos/mit-include/uapi/|include/uapi/|; s|^\.illumos/mit-include/|include/|' \
	    "$inputs" | sort -u > "$out.files"
else
	{
		grep -v '^#' "$HERE/tus.txt" | grep . | grep -v '^illumos/' | sed "s|^|$I915/|"
		grep -v '^#' "$HERE/display-tus.txt" | grep . | grep -v '^-' |
		    grep -v '^illumos/' | sed "s|^|$I915/|"
		# i915's own headers, minus self-tests (not compiled)
		(cd "$LINUX" && find "$I915" -name '*.h' \
		    ! -path '*/selftests/*' ! -name 'selftest_*' ! -path '*/gvt/*' \
		    ! -name '*_trace.h')
		grep -v '^#' "$HERE/mit-headers.txt" | grep .
	} | sort -u > "$out.files"
fi

tag=$(sed -n 's/^LINUX_TAG=//p' "$HERE/PIN")
{
	if [ "${MODULE:-i915n}" = drm_kms ]; then
		echo "misc/drm_kms is built from these files of Linux $tag (drivers/gpu/drm,"
		echo "drivers/gpu/drm/display, drivers/video/hdmi.c, include/drm,"
		echo "include/uapi/drm), MIT-licensed, with the illumos patches of"
		echo "illumos-drm carried/drm_kms/patches, and from illumos-drm's own MIT"
		echo "sources (carried/drm_kms/illumos). Their copyright notices:"
	else
		echo "i915n is built from these files of Linux $tag (drivers/gpu/drm/i915,"
		echo "include/drm, include/uapi/drm), MIT-licensed, with the illumos patches"
		echo "of illumos-drm drivers/i915/patches, and from illumos-drm's own MIT"
		echo "sources (drivers/i915/illumos). Their copyright notices:"
	fi
	echo
	while read -r f; do
		[ -f "$LINUX/$f" ] || continue
		head -40 "$LINUX/$f" | { grep -i -E '^[ */#]*copyright' || true; } |
		    sed -E 's/^[ */#]*//; s/[ */]*$//'
	done < "$out.files" | sort -u
	echo "Copyright 2026 The illumos-drm authors"
	cat <<'EOT'

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
EOT
	# Files under the HPND notice (Permission to use, copy, modify,
	# distribute, and sell) instead: list them and give their notice as
	# they carry it (the first comment block, copyright lines dropped).
	hpnd=$(while read -r f; do
		[ -f "$LINUX/$f" ] && head -40 "$LINUX/$f" |
		    grep -q 'Permission to use, copy, modify, distribute, and sell' &&
		    echo "$f"
	done < "$out.files" || true)
	if [ -n "$hpnd" ]; then
		echo
		echo "These files carry the following notice instead of the one above:"
		echo "$hpnd" | sed 's/^/  /'
		echo
		for f in $hpnd; do
			awk 'NR == 1 && !/^\/\*/ { exit } { print } /\*\// { exit }' "$LINUX/$f" |
			    sed -E 's|^/\*||; s|\*/$||; s/^ \* ?//; s/^ \*$//' |
			    grep -v -i '^copyright' | sed 's/[[:space:]]*$//' | sed '/./,$!d'
			echo
		done | awk 'BEGIN { RS = ""; ORS = "\n\n" } !seen[$0]++'
	fi
} > "$out"
rm -f "$out.files"
echo "gen-license: $out ($(grep -c -i '^copyright' "$out") copyright lines)"
