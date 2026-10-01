#!/bin/sh
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Builds the cdrmnull kernel module on illumos: compiles cdrmnull.c with
# gcc and the illumos-gate's kernel flags (usr/src/Makefile.master,
# uts/Makefile.uts, uts/intel/Makefile.intel, as tools/cw translates them
# for gcc), then links it as a kernel module that depends on misc/drm_core and
# whose every undefined symbol is declared by a mapfile (-z defs).
#
#   drivers/cdrmnull/build.sh
#
# Environment (defaults in brackets):
#   CC           the compiler [gcc]
#   LD           the illumos link-editor [/usr/bin/ld]
#   REPO         the illumos-drm checkout [two directories above this script]
#   SYSINC       the system headers: sys/*.h, stdint.h and libdrm's
#                drm/drm.h (package developer/header-drm) [/usr/include]
#   DDI_MAPFILE  the gate's usr/src/uts/common/mapfiles/ddi.mapfile, which
#                an installed system does not have [./ddi.mapfile, else
#                ddi.mapfile next to this script]
#   OUT          where cdrmnull.o and the module cdrmnull go [.]
#
# The module is linked with
#   ld -ztype=kmod -N misc/drm_core -z defs -M ddi.mapfile \
#       -M include/drm/drm_facade.mapfile -M private.mapfile
# private.mapfile declares devmap_umem_remap, the one symbol outside the
# DDI, and the soft interrupt functions of the DDI that the gate's
# ddi.mapfile lacks.
#
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-$(cd "$HERE/../.." && pwd)}
CC=${CC:-gcc}
LD=${LD:-/usr/bin/ld}
SYSINC=${SYSINC:-/usr/include}
OUT=${OUT:-.}
FACADE_MAPFILE=$REPO/include/drm/drm_facade.mapfile
PRIVATE_MAPFILE=$HERE/private.mapfile
if [ -z "${DDI_MAPFILE:-}" ]; then
	if [ -f ./ddi.mapfile ]; then
		DDI_MAPFILE=./ddi.mapfile
	else
		DDI_MAPFILE=$HERE/ddi.mapfile
	fi
fi

die() {
	echo "build.sh: $*" >&2
	exit 1
}

for f in "$HERE/cdrmnull.c" "$HERE/cdrmnull_ioctl.h" \
    "$REPO/include/drm/drm_facade.h" "$FACADE_MAPFILE" "$PRIVATE_MAPFILE" \
    "$SYSINC/sys/sunddi.h" "$SYSINC/drm/drm.h"; do
	[ -f "$f" ] || die "missing $f"
done
[ -f "$DDI_MAPFILE" ] || die "missing $DDI_MAPFILE: copy the gate's" \
    "usr/src/uts/common/mapfiles/ddi.mapfile there or set DDI_MAPFILE"
mkdir -p "$OUT"

TMPD=$(mktemp -d /tmp/cdrmnull-build.XXXXXX)
trap 'rm -rf "$TMPD"' EXIT
trap 'exit 1' INT TERM
printf 'int cdrmnull_probe;\n' > "$TMPD/probe.c"

# Whether the compiler takes these flags without a warning.
probe() {
	"$CC" -m64 -Werror "$@" -c -o "$TMPD/probe.o" "$TMPD/probe.c" \
	    >/dev/null 2>&1
}

# The gate's flags for a 64-bit kernel module, as cw passes them to gcc.
CFLAGS="-m64 -mtune=opteron -Ui386 -U__i386 -O2 -std=gnu99"
CFLAGS="$CFLAGS -fno-strict-aliasing -fno-optimize-sibling-calls"
CFLAGS="$CFLAGS -ffreestanding -mcmodel=kernel -mno-red-zone"
CFLAGS="$CFLAGS -mno-mmx -mno-sse"
CFLAGS="$CFLAGS -fident -finline -fno-inline-functions -fno-builtin"
CFLAGS="$CFLAGS -fno-asm -fdiagnostics-show-option"
CFLAGS="$CFLAGS -fno-asynchronous-unwind-tables"
CFLAGS="$CFLAGS -Wall -Wextra -Werror -Wno-missing-braces -Wno-sign-compare"
CFLAGS="$CFLAGS -Wno-unused-parameter -Wno-missing-field-initializers"
# shellcheck disable=SC2086 # word splitting of the flag list
probe $CFLAGS || die "$CC does not take the base kernel flags: $CFLAGS"

# Flags of the gate that depend on the compiler's version or on illumos'
# gcc patches (-msave-args): used where the compiler has them.
for f in -msave-args \
    "-mindirect-branch=thunk-extern -mindirect-branch-register" \
    -fno-shrink-wrap -fno-unit-at-a-time \
    -fno-inline-small-functions -fno-inline-functions-called-once \
    -fno-ipa-cp -fno-ipa-icf -fno-clone-functions \
    -fno-reorder-functions -fno-reorder-blocks-and-partition \
    -fno-aggressive-loop-optimizations \
    --param=max-inline-insns-single=450 \
    -fstack-protector-strong; do
	# shellcheck disable=SC2086 # $f may hold two flags
	if probe $f; then
		CFLAGS="$CFLAGS $f"
	else
		echo "build.sh: $CC does not take $f; built without it"
	fi
done

CPPFLAGS="-D_KERNEL -D_SYSCALL32 -D_SYSCALL32_IMPL -D_ELF64 -D_DDI_STRICT"
CPPFLAGS="$CPPFLAGS -Dsun -D__sun -D__SVR4 -D_ASM_INLINES"
# Only the facade and the system headers: the facade's drm/ directory
# first (it has no drm.h), then libdrm's drm/drm.h and the kernel headers.
CPPFLAGS="$CPPFLAGS -nostdinc -I$REPO/include -I$SYSINC"

echo "\$ $CC --version"
"$CC" --version | sed -n 1p
echo "\$ $CC $CFLAGS $CPPFLAGS -c $HERE/cdrmnull.c -o $OUT/cdrmnull.o"
# shellcheck disable=SC2086 # word splitting of the flag lists
"$CC" $CFLAGS $CPPFLAGS -c "$HERE/cdrmnull.c" -o "$OUT/cdrmnull.o"

echo "\$ $LD -ztype=kmod -N misc/drm_core -z defs -M $DDI_MAPFILE" \
    "-M $FACADE_MAPFILE -M $PRIVATE_MAPFILE -o $OUT/cdrmnull $OUT/cdrmnull.o"
"$LD" -ztype=kmod -N misc/drm_core -z defs -M "$DDI_MAPFILE" \
    -M "$FACADE_MAPFILE" -M "$PRIVATE_MAPFILE" \
    -o "$OUT/cdrmnull" "$OUT/cdrmnull.o"

if command -v elfdump >/dev/null 2>&1; then
	echo "\$ elfdump -d $OUT/cdrmnull | grep NEEDED"
	elfdump -d "$OUT/cdrmnull" | grep NEEDED || true
fi
if command -v digest >/dev/null 2>&1; then
	echo "\$ digest -a sha256 $OUT/cdrmnull"
	digest -a sha256 "$OUT/cdrmnull" || true
fi
echo "build.sh: built $OUT/cdrmnull (object $OUT/cdrmnull.o)"
