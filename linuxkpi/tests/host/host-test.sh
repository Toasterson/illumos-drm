#!/bin/sh
#
# SPDX-License-Identifier: MIT
# Copyright 2026 The illumos-drm authors
#
# Builds and runs the host unit tests of the LinuxKPI layer's pure-logic
# code: the real linuxkpi/src files below, compiled against the layer's
# headers with include/ of this directory first on the include path (its
# lkpi/prologue.h and sys/*.h replace the illumos kernel headers with the
# host stand-ins of include/lkpi_host.h), with AddressSanitizer and
# UndefinedBehaviorSanitizer.
#
#   tests/host/host-test.sh [test-name ...]
#
# Environment: CC [clang], OUT [${TMPDIR:-/tmp}/lkpi-host-test].
# The layer is x86-64 code (inline assembly in some headers), so on an
# arm64 Mac the tests are built for x86_64 and run under Rosetta 2.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
LKPI=$(cd "$HERE/../.." && pwd)
REPO=$(cd "$LKPI/.." && pwd)
CC=${CC:-clang}
OUT=${OUT:-${TMPDIR:-/tmp}/lkpi-host-test}

# The layer sources under test.
SRCS="lkpi_rbtree.c lkpi_xarray.c lkpi_bitmap.c lkpi_printf.c lkpi_string.c
lkpi_misc.c lkpi_kobject.c"

ARCH=
if [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ]; then
	ARCH="-arch x86_64"
fi

# Linux's llist_for_each_entry() (like Linux) forms container_of(NULL)
# at the end of a list and only tests the member address; the kernel is
# built with -fno-delete-null-pointer-checks, so that is not checked here.
# Signed overflow wraps in the kernel build (-fno-strict-overflow; the
# refcount_t saturation checks rely on it): -fwrapv, not checked.
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
COMMON="$ARCH -std=gnu11 -g -O1 $SAN -fms-extensions -funsigned-char
-fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -Wall -Werror=implicit-function-declaration
-Wno-comment -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare
-Wno-macro-redefined -Wno-microsoft-anon-tag -Wno-gnu-variable-sized-type-not-at-end
-Wno-format -Wno-unknown-warning-option"
LAYER="-I$HERE/include -I$LKPI/include -I$REPO/include -I$LKPI/src
-include $LKPI/include/linux/kconfig.h -include $HERE/include/lkpi/prologue.h"

rm -rf "$OUT"
mkdir -p "$OUT"
grep -ho '^TEST([a-z0-9_]*)' "$HERE"/t_*.c | sed 's/^TEST(\(.*\))$/T(\1)/' \
    > "$OUT/tests.def"

objs=
for f in $SRCS; do
	# shellcheck disable=SC2086
	$CC $COMMON $LAYER -c "$LKPI/src/$f" -o "$OUT/${f%.c}.o"
	objs="$objs $OUT/${f%.c}.o"
done
for f in "$HERE"/t_*.c; do
	b=$(basename "$f" .c)
	# shellcheck disable=SC2086
	$CC $COMMON $LAYER -c "$f" -o "$OUT/$b.o"
	objs="$objs $OUT/$b.o"
done
# shellcheck disable=SC2086
$CC $COMMON -I$HERE/include -c "$HERE/host_stubs.c" -o "$OUT/host_stubs.o"
# shellcheck disable=SC2086
$CC $COMMON -I"$OUT" -c "$HERE/main.c" -o "$OUT/main.o"
# shellcheck disable=SC2086
$CC $ARCH $SAN -o "$OUT/lkpi-host-test" $objs "$OUT/host_stubs.o" \
    "$OUT/main.o" -lpthread

ASAN_OPTIONS=${ASAN_OPTIONS:-abort_on_error=0:detect_leaks=0} \
UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1} \
    "$OUT/lkpi-host-test" "$@"
