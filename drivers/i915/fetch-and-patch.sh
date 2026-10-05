#!/bin/bash
#
# SPDX-License-Identifier: MIT
# Copyright 2026 The illumos-drm authors
#
# Fetches the pinned Linux tree (PIN), applies the illumos patch series
# (patches/*.patch, in order) and prepares <tree>/.illumos/mit-include with
# the MIT-licensed Linux headers of mit-headers.txt. The Linux source stays
# outside illumos-drm; only the patches and this script are in it.
#
#   drivers/i915/fetch-and-patch.sh <directory>
#
# <directory> is created (git init + fetch of the pinned commit, about
# 260 MB) or reused if it already holds the pinned commit. An existing
# tree must be clean or carry exactly this patch series; the script never
# discards changes of its own accord.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
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

series=$( (cd "$HERE" && ls patches/*.patch 2>/dev/null || true) | sort)
sum=$(cd "$HERE" && cat /dev/null $series | sha256sum 2>/dev/null | cut -d' ' -f1 ||
    cat /dev/null $series | digest -a sha256)
stamp=$dest/.illumos/applied
if [ -f "$stamp" ] && [ "$(sed -n 1p "$stamp")" = "$sum" ]; then
	echo "fetch-and-patch: series already applied ($sum)"
else
	if [ -n "$(git -C "$dest" status --porcelain --untracked-files=no)" ]; then
		echo "fetch-and-patch: $dest has changes that are not this series;" \
		    "check them out (git -C $dest checkout -- .) yourself" >&2
		exit 1
	fi
	for p in $series; do
		echo "fetch-and-patch: applying $p"
		git -C "$dest" apply --whitespace=nowarn "$HERE/$p"
	done
	mkdir -p "$dest/.illumos"
	{ echo "$sum"; echo "$series"; } > "$stamp"
fi

mit=$dest/.illumos/mit-include
rm -rf "$mit"
mkdir -p "$mit"
grep -v '^#' "$HERE/mit-headers.txt" | grep . | while read -r f; do
	src=$dest/$f
	if ! head -40 "$src" | grep -q -i -E 'SPDX-License-Identifier:.*\bMIT\b|Permission is hereby granted, free of charge|Permission to use, copy, modify, distribute, and sell this software'; then
		echo "fetch-and-patch: $f is not MIT-licensed (or HPND); refusing it" >&2
		exit 1
	fi
	d=$mit/${f#include/}
	mkdir -p "$(dirname "$d")"
	cp "$src" "$d"
done
echo "fetch-and-patch: $dest ready (Linux $LINUX_TAG $LINUX_REV, $(echo "$series" | grep -c . || true) patches)"
