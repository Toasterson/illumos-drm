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
# drm_cache's tests run in the kernel only (drm_helpers_test): drm_cache
# needs misc/linuxkpi's processor facts, which exist in the kernel.
#
#   carried/host-test.sh [selftest_host arguments: -s seed -x scale names]
#
# Environment: CC [clang], OUT [a temporary directory, removed afterwards].
# Everything is compiled as linuxkpi's host tests are
# (linuxkpi/tests/host/host-test.sh): the layer's headers and
# src/lkpi_rbtree.c with the host prologue of linuxkpi/tests/host/include,
# AddressSanitizer and UndefinedBehaviorSanitizer; on an arm64 Mac as
# x86_64 (Rosetta 2), since the layer's headers have x86 inline assembly.
#
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${CC:-clang}
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

LH=$REPO/linuxkpi/tests/host
ARCH=
if [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ]; then
	ARCH="-arch x86_64"
fi
# SANITIZE (default 1 with clang): AddressSanitizer and UBSan.
case "${SANITIZE:-auto}" in
auto) case "$("$CC" --version 2>/dev/null | sed -n 1p)" in
	*clang*) SANITIZE=1 ;;
	*) SANITIZE=0 ;;
	esac ;;
esac
SAN=
if [ "$SANITIZE" = 1 ]; then
	SAN="-fsanitize=address,undefined -fno-sanitize=pointer-overflow
-fno-sanitize=signed-integer-overflow
-fno-sanitize-recover=undefined -fno-omit-frame-pointer"
fi
CFLAGS="$ARCH -std=gnu11 -g -O1 $SAN -fms-extensions -funsigned-char
-fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -Wall
-Werror=implicit-function-declaration -Wno-comment -Wno-unused-function
-Wno-unused-parameter -Wno-sign-compare -Wno-macro-redefined
-Wno-microsoft-anon-tag -Wno-gnu-variable-sized-type-not-at-end -Wno-format
-Wno-unknown-warning-option"
CPPFLAGS="-I$OUT/linux/include -I$LH/include -I$REPO/linuxkpi/include
-I$REPO/include -I$REPO/linuxkpi/src -I$HERE/test
-include $REPO/linuxkpi/include/linux/kconfig.h -include $LH/include/lkpi/prologue.h"

echo "\$ $CC --version"
"$CC" --version | sed -n 1p
objs=
for src in "$OUT/linux/drivers/gpu/drm/drm_mm.c" \
    "$REPO/linuxkpi/src/lkpi_rbtree.c" \
    "$REPO/linuxkpi/src/lkpi_printf.c" \
    "$REPO/linuxkpi/src/lkpi_string.c" \
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
"$CC" $CFLAGS -I"$LH/include" -c "$LH/host_stubs.c" -o "$OUT/host_stubs.o"
# shellcheck disable=SC2086
"$CC" $ARCH $SAN -o "$OUT/selftest_host" $objs "$OUT/host_stubs.o" -lpthread
echo "\$ selftest_host $*"
ASAN_OPTIONS=${ASAN_OPTIONS:-abort_on_error=0:detect_leaks=0} \
    "$OUT/selftest_host" "$@"
