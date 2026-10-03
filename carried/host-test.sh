#!/bin/sh
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# host-test.sh: builds the carried helpers (carried/fetch-and-patch's
# output), linuxkpi's red-black trees and the self-tests into a userland
# program and runs it: the drm_mm tests the kernel's drm_helpers_test runs,
# the host-only drm_mm tests and the tree tests (carried/test/host/).
#
#   carried/host-test.sh [selftest_host arguments: -s seed -x scale names]
#
# Environment: CC [cc], OUT [a temporary directory, removed afterwards],
# SANITIZE=1 adds -fsanitize=address,undefined (clang or gcc with libasan).
# The program's headers are carried/test/host/include (the host's
# linux/types.h, linux/bug.h and drm/drm_print.h) before linuxkpi/include.
#
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${CC:-cc}
if [ -n "${OUT:-}" ]; then
	mkdir -p "$OUT"
	keep=1
else
	OUT=$(mktemp -d /tmp/carried-host-test.XXXXXX)
	keep=0
fi
cleanup() {
	[ "$keep" -eq 1 ] || rm -rf "$OUT"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

"$HERE/fetch-and-patch" "$OUT"

CFLAGS="-std=gnu99 -O2 -g -Wall -Wextra -Werror -Wno-sign-compare"
CFLAGS="$CFLAGS -Wno-unused-parameter -Wno-missing-field-initializers"
CFLAGS="$CFLAGS -fno-strict-aliasing"
LDFLAGS=""
if [ "${SANITIZE:-0}" = 1 ]; then
	CFLAGS="$CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer"
	CFLAGS="$CFLAGS -fno-sanitize-recover=undefined"
	LDFLAGS="-fsanitize=address,undefined"
fi
CPPFLAGS="-I$HERE/test/host/include -I$REPO/linuxkpi/include"
CPPFLAGS="$CPPFLAGS -I$OUT/linux/include -I$HERE/test"

echo "\$ $CC --version"
"$CC" --version | sed -n 1p
set -- "$@"
objs=
for src in \
    "$OUT/linux/drivers/gpu/drm/drm_mm.c" \
    "$REPO/linuxkpi/lib/rbtree.c" \
    "$HERE/drm_mm/tests/drm_mm_selftest.c" \
    "$HERE/test/host/drm_mm_host_test.c" \
    "$HERE/test/host/rbtree_test.c" \
    "$HERE/test/host/selftest_host.c"; do
	obj="$OUT/$(basename "$src" .c).o"
	# shellcheck disable=SC2086 # word splitting of the flag lists
	"$CC" $CFLAGS $CPPFLAGS -c "$src" -o "$obj"
	objs="$objs $obj"
done
# shellcheck disable=SC2086
"$CC" $CFLAGS $LDFLAGS -o "$OUT/selftest_host" $objs -lpthread
echo "\$ selftest_host $*"
"$OUT/selftest_host" "$@"
