#!/bin/sh
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Builds misc/drm_helpers (the carried Linux helpers) and the test driver
# drm_helpers_test on illumos, with gcc and the illumos-gate's kernel flags
# (as drivers/cdrmnull/build.sh does):
#
#   carried/build.sh
#
# 1. carried/fetch-and-patch verifies the vendored Linux files and applies
#    the patch series into $OUT/src/linux.
# 2. drm_mm.c and drm_cache.c (Linux), linuxkpi/lib/rbtree.c and
#    x86_cpu.c and the module linkage are
#    compiled against carried headers, linuxkpi/include, the facade
#    (include/) and the system headers, and linked
#      ld -ztype=kmod -z defs -B reduce -M ddi.mapfile \
#          -M drm_helpers/private.mapfile -M drm_helpers/drm_helpers_scope.mapfile
#    The module's global symbols must be exactly the scope mapfile's (else
#    the build fails), so i915 can bind to drm_mm and nothing else.
# 3. The test driver is linked
#      ld -ztype=kmod -N misc/drm_helpers -z defs -M ddi.mapfile \
#          -M drm_helpers/drm_helpers.mapfile
#    i.e. against the exported functions only, as a C driver would be.
#
# Environment (defaults in brackets):
#   CC           the compiler [gcc]
#   LD           the illumos link-editor [/usr/bin/ld]
#   SYSINC       the system headers, with libdrm's drm/drm.h [/usr/include]
#   DDI_MAPFILE  the gate's usr/src/uts/common/mapfiles/ddi.mapfile
#                [./ddi.mapfile, else drivers/cdrmnull/ddi.mapfile]
#   OUT          where the objects and the modules go [./target/carried]
#
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${CC:-gcc}
LD=${LD:-/usr/bin/ld}
SYSINC=${SYSINC:-/usr/include}
OUT=${OUT:-./target/carried}
if [ -z "${DDI_MAPFILE:-}" ]; then
	if [ -f ./ddi.mapfile ]; then
		DDI_MAPFILE=./ddi.mapfile
	else
		DDI_MAPFILE=$REPO/drivers/cdrmnull/ddi.mapfile
	fi
fi

die() {
	echo "build.sh: $*" >&2
	exit 1
}

[ -f "$DDI_MAPFILE" ] || die "missing $DDI_MAPFILE: copy the gate's" \
    "usr/src/uts/common/mapfiles/ddi.mapfile there or set DDI_MAPFILE"
for f in "$SYSINC/sys/sunddi.h" "$SYSINC/drm/drm.h" \
    "$REPO/include/drm/drm_facade.h"; do
	[ -f "$f" ] || die "missing $f"
done
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

"$HERE/fetch-and-patch" "$OUT/src"

TMPD=$(mktemp -d /tmp/carried-build.XXXXXX)
trap 'rm -rf "$TMPD"' EXIT
trap 'exit 1' INT TERM
printf 'int carried_probe;\n' > "$TMPD/probe.c"
probe() {
	"$CC" -m64 -Werror "$@" -c -o "$TMPD/probe.o" "$TMPD/probe.c" \
	    >/dev/null 2>&1
}

# The gate's flags for a 64-bit kernel module, as cw passes them to gcc
# (the same list as drivers/cdrmnull/build.sh).
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
# The Linux configuration of the carried code (Linux passes its Kconfig on
# the command line as well): x86-64. CONFIG_LKPI_STRUCT_PAGE and
# CONFIG_LKPI_IOSYS_MAP stay undefined until R5c's page and iosys-map
# models exist (carried/drm_cache/patches).
CPPFLAGS="$CPPFLAGS -DCONFIG_X86=1 -DCONFIG_X86_64=1"
# Carried Linux headers, then linuxkpi's linux/*.h, then the facade's
# drm/*.h (drm_print.h and the base types), then the system headers.
CPPFLAGS="$CPPFLAGS -nostdinc -I$OUT/src/linux/include -I$REPO/linuxkpi/include"
CPPFLAGS="$CPPFLAGS -I$REPO/include -I$HERE/test -I$SYSINC"

cc1() {
	echo "\$ $CC ... -c $1 -o $2"
	# shellcheck disable=SC2086 # word splitting of the flag lists
	"$CC" $CFLAGS $CPPFLAGS -c "$1" -o "$2"
}

echo "\$ $CC --version"
"$CC" --version | sed -n 1p
echo "CFLAGS: $CFLAGS"
echo "CPPFLAGS: $CPPFLAGS"

mkdir -p "$OUT/obj"
cc1 "$OUT/src/linux/drivers/gpu/drm/drm_mm.c" "$OUT/obj/drm_mm.o"
cc1 "$OUT/src/linux/drivers/gpu/drm/drm_cache.c" "$OUT/obj/drm_cache.o"
cc1 "$REPO/linuxkpi/lib/rbtree.c" "$OUT/obj/rbtree.o"
cc1 "$REPO/linuxkpi/lib/x86_cpu.c" "$OUT/obj/x86_cpu.o"
cc1 "$HERE/drm_helpers/drm_helpers_mod.c" "$OUT/obj/drm_helpers_mod.o"

SCOPE=$HERE/drm_helpers/drm_helpers_scope.mapfile
echo "\$ $LD -ztype=kmod -z defs -B reduce -M $DDI_MAPFILE" \
    "-M $HERE/drm_helpers/private.mapfile -M $SCOPE -o $OUT/drm_helpers ..."
"$LD" -ztype=kmod -z defs -B reduce -M "$DDI_MAPFILE" \
    -M "$HERE/drm_helpers/private.mapfile" -M "$SCOPE" \
    -o "$OUT/drm_helpers" "$OUT/obj/drm_mm.o" "$OUT/obj/drm_cache.o" \
    "$OUT/obj/rbtree.o" "$OUT/obj/x86_cpu.o" "$OUT/obj/drm_helpers_mod.o"

# The module's global definitions are exactly the scope mapfile's.
nm "$OUT/drm_helpers" | awk -F'|' '$5 ~ /GLOB/ && $7 !~ /UNDEF/ {
    gsub(/ /, "", $8); print $8 }' | sort > "$OUT/drm_helpers.globals"
awk '/global:/ { g = 1; next } /local:/ { g = 0 }
    g && /;/ { gsub(/[;\t ]/, ""); print }' "$SCOPE" | sort \
    > "$OUT/drm_helpers.want"
diff "$OUT/drm_helpers.want" "$OUT/drm_helpers.globals" ||
    die "misc/drm_helpers' global symbols differ from $SCOPE"
# And the consumers' mapfile declares the same names, minus the entry points.
awk '/global:/ { g = 1; next } g && /EXTERN/ { print $1 }' \
    "$HERE/drm_helpers/drm_helpers.mapfile" | sort > "$OUT/drm_helpers.extern"
grep -v -e '^_init$' -e '^_fini$' -e '^_info$' "$OUT/drm_helpers.want" |
    diff - "$OUT/drm_helpers.extern" ||
    die "drm_helpers.mapfile and drm_helpers_scope.mapfile differ"
echo "misc/drm_helpers: $(wc -l < "$OUT/drm_helpers.globals" | tr -d ' ')" \
    "global symbols, as $(basename "$SCOPE")"

cc1 "$HERE/drm_mm/tests/drm_mm_selftest.c" "$OUT/obj/drm_mm_selftest.o"
cc1 "$HERE/drm_cache/tests/drm_cache_selftest.c" \
    "$OUT/obj/drm_cache_selftest.o"
cc1 "$HERE/test/selftest_kernel.c" "$OUT/obj/selftest_kernel.o"
cc1 "$HERE/test/drm_helpers_test.c" "$OUT/obj/drm_helpers_test.o"
echo "\$ $LD -ztype=kmod -N misc/drm_helpers -z defs -M $DDI_MAPFILE" \
    "-M $HERE/drm_helpers/drm_helpers.mapfile -o $OUT/drm_helpers_test ..."
"$LD" -ztype=kmod -N misc/drm_helpers -z defs -M "$DDI_MAPFILE" \
    -M "$HERE/drm_helpers/drm_helpers.mapfile" \
    -o "$OUT/drm_helpers_test" "$OUT/obj/drm_helpers_test.o" \
    "$OUT/obj/selftest_kernel.o" "$OUT/obj/drm_mm_selftest.o" \
    "$OUT/obj/drm_cache_selftest.o"

if command -v elfdump >/dev/null 2>&1; then
	for m in drm_helpers drm_helpers_test; do
		echo "\$ elfdump -d $OUT/$m | grep -E 'NEEDED|SUNW_KMOD'"
		elfdump -d "$OUT/$m" | grep -E 'NEEDED|SUNW_KMOD' || true
	done
fi
if command -v digest >/dev/null 2>&1; then
	digest -v -a sha256 "$OUT/drm_helpers" "$OUT/drm_helpers_test" || true
fi
echo "build.sh: built $OUT/drm_helpers and $OUT/drm_helpers_test"
