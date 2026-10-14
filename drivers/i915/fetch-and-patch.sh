#!/bin/bash
#
# SPDX-License-Identifier: MIT
# Copyright 2026 The illumos-drm authors
#
# Fetches the pinned Linux tree (PIN), applies the illumos patch series
# (patches/*.patch, then carried/drm_kms/patches/*.patch, in order) and
# prepares <tree>/.illumos/mit-include with the MIT-licensed Linux headers
# of mit-headers.txt. The Linux source stays outside illumos-drm; only the
# patches and this script are in it. One tree serves i915n
# (drivers/i915/build.sh) and misc/drm_kms (carried/drm_kms/build.sh).
#
# Licences: every TU of tus.txt, display-tus.txt and
# carried/drm_kms/tus.txt and every header of mit-headers.txt must be MIT
# or HPND (check-licences.py), else the script refuses the tree. GPL-2.0
# headers the builds include are replaced in the tree: the tracepoint
# headers by MIT no-op headers that illumos/gen-trace-stubs.py writes from
# the originals' event names (TRACE_HEADERS below), intel_ips.h and
# drm_dp_mst_topology_internal.h by MIT files of illumos-drm (REPLACED);
# the build scripts check every Linux file the compiler read, so no GPL
# text reaches a build.
#
#   drivers/i915/fetch-and-patch.sh <directory>
#
# <directory> is created (git init + fetch of the pinned commit, about
# 260 MB) or reused if it already holds the pinned commit. An existing
# tree must be clean or carry a series this script applied (it keeps
# copies in <tree>/.illumos/series and reverts them before applying a
# changed series); the script never discards other changes.
set -euo pipefail
# GNU grep and sed first on illumos; one collation everywhere
PATH=/usr/gnu/bin:$PATH
export LC_ALL=C

HERE=$(cd "$(dirname "$0")" && pwd)
KMS=$(cd "$HERE/../../carried/drm_kms" && pwd)
# shellcheck disable=SC1091
. "$HERE/PIN"
dest=${1:?usage: fetch-and-patch.sh <directory>}

if ! git -C "$dest" rev-parse --git-dir >/dev/null 2>&1; then
	mkdir -p "$dest"
	git -C "$dest" init -q
	git -C "$dest" remote add origin "$LINUX_REPO"
	git -C "$dest" fetch -q --depth 1 origin "$LINUX_REV"
	git -C "$dest" checkout -q FETCH_HEAD
fi
head=$(git -C "$dest" rev-parse HEAD)
if [ "$head" != "$LINUX_REV" ]; then
	echo "fetch-and-patch: $dest is at $head, not the pinned $LINUX_REV" >&2
	exit 1
fi

I915=drivers/gpu/drm/i915
TRACE_HEADERS="$I915/i915_trace.h $I915/intel_uncore_trace.h"
TRACE_HEADERS="$TRACE_HEADERS $I915/display/intel_display_trace.h drivers/gpu/drm/drm_trace.h"
# GPL-2.0 headers the builds include by relative path, replaced by MIT
# files of illumos-drm (tree path, then replacement relative to the
# repository)
REPLACED="drivers/platform/x86/intel_ips.h drivers/i915/illumos/intel_ips.h"
REPLACED="$REPLACED drivers/gpu/drm/display/drm_dp_mst_topology_internal.h"
REPLACED="$REPLACED carried/drm_kms/illumos/drm_dp_mst_topology_internal.h"
REPO=$(cd "$HERE/../.." && pwd)
# git pathspecs that leave out the generated and replaced headers
keep=()
for h in $TRACE_HEADERS; do keep+=(":(exclude)$h"); done
set -- $REPLACED
while [ $# -ge 2 ]; do keep+=(":(exclude)$1"); shift 2; done

# The series: i915's patches, then misc/drm_kms', as paths relative to
# the repository (each group in name order).
series=$( (cd "$REPO" && { ls drivers/i915/patches/*.patch 2>/dev/null | sort
    ls carried/drm_kms/patches/*.patch 2>/dev/null | sort; } || true) )
sum=$(cd "$REPO" && cat /dev/null $series | sha256sum 2>/dev/null | cut -d' ' -f1 ||
    cat /dev/null $series | digest -a sha256)
stamp=$dest/.illumos/applied
if [ -f "$stamp" ] && [ "$(sed -n 1p "$stamp")" = "$sum" ]; then
	echo "fetch-and-patch: series already applied ($sum)"
else
	# A tree that carries an earlier series of this script: take that
	# series off again (the copies kept at the time), newest first.
	if [ -f "$stamp" ] && [ -d "$dest/.illumos/series" ]; then
		# shellcheck disable=SC2086
		git -C "$dest" checkout -- $TRACE_HEADERS $(echo $REPLACED | awk '{ for (i = 1; i <= NF; i += 2) print $i }')
		for p in $(ls "$dest/.illumos/series" | sort -r); do
			echo "fetch-and-patch: reverting the earlier $p"
			git -C "$dest" apply -R --whitespace=nowarn "$dest/.illumos/series/$p"
		done
		rm -rf "$dest/.illumos/series" "$stamp"
	fi
	if [ -n "$(git -C "$dest" status --porcelain --untracked-files=no -- . "${keep[@]}")" ]; then
		echo "fetch-and-patch: $dest has changes that are not this series;" \
		    "check them out (git -C $dest checkout -- .) yourself" >&2
		exit 1
	fi
	# shellcheck disable=SC2086
	git -C "$dest" checkout -- $TRACE_HEADERS $(echo $REPLACED | awk '{ for (i = 1; i <= NF; i += 2) print $i }')
	for p in $series; do
		echo "fetch-and-patch: applying $p"
		git -C "$dest" apply --whitespace=nowarn "$REPO/$p"
	done
	# Copies named in application order, so that a later run can take
	# them off newest first (sort -r) without the repository's files.
	mkdir -p "$dest/.illumos/series"
	i=0
	for p in $series; do
		i=$((i + 1))
		cp "$REPO/$p" "$dest/.illumos/series/$(printf '%03d' $i)-$(basename "$p")"
	done
	{ echo "$sum"; echo "$series"; } > "$stamp"
fi

# The tracepoint headers: MIT no-op replacements (see above), from the
# pinned originals.
for h in $TRACE_HEADERS; do
	git -C "$dest" show "HEAD:$h" > "$dest/.illumos/$(basename "$h").orig"
	python3 "$HERE/illumos/gen-trace-stubs.py" "$dest/.illumos/$(basename "$h").orig" "$dest/$h"
	rm -f "$dest/.illumos/$(basename "$h").orig"
done

# The replaced headers.
set -- $REPLACED
while [ $# -ge 2 ]; do
	cp "$REPO/$2" "$dest/$1"
	shift 2
done

# The TUs and the headers used unchanged: MIT (or HPND) only. The build
# checks every Linux file the compiler read, too (build.sh).
{
	grep -v '^#' "$HERE/tus.txt" | grep . | grep -v '^illumos/' | sed "s|^|$I915/|"
	grep -v '^#' "$HERE/display-tus.txt" | grep . | grep -v '^-' |
	    grep -v '^illumos/' | sed "s|^|$I915/|"
	grep -v '^#' "$KMS/tus.txt" | grep .
	grep -v '^#' "$HERE/mit-headers.txt" | grep .
} > "$dest/.illumos/licence-check.txt"
python3 "$HERE/check-licences.py" "$dest" "$dest/.illumos/licence-check.txt"

mit=$dest/.illumos/mit-include
rm -rf "$mit"
mkdir -p "$mit"
grep -v '^#' "$HERE/mit-headers.txt" | grep . | while read -r f; do
	src=$dest/$f
	d=$mit/${f#include/}
	mkdir -p "$(dirname "$d")"
	cp "$src" "$d"
done
echo "fetch-and-patch: $dest ready (Linux $LINUX_TAG $LINUX_REV, $(echo "$series" | grep -c . || true) patches)"

# Keep opt-in binding data tied to the same PCI table and admission groups.
python3 "$HERE/gen-aliases.py" "$dest" > "$dest/.illumos/i915n.aliases"
if ! cmp -s "$dest/.illumos/i915n.aliases" "$REPO/packaging/i915n.aliases"; then
	echo "fetch-and-patch: regenerate packaging/i915n.aliases for this PIN" >&2
	exit 1
fi
