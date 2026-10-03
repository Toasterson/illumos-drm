#!/bin/bash
#
# SPDX-License-Identifier: MIT
# Copyright 2026 The illumos-drm authors
#
# Builds the headless i915 objects of the pinned Linux tree against the
# DRM facade and the LinuxKPI layer with the illumos kernel's compiler
# flags, then links them into the kernel module "i915" with
#   ld -ztype=kmod -N misc/drm_core -N misc/linuxkpi -z defs \
#      -M ddi.mapfile -M drm_facade.mapfile -M linuxkpi.mapfile ...
# so that every undefined symbol must be declared by one of the mapfiles.
#
#   drivers/i915/build.sh compile    compile every TU of tus.txt (-k style)
#   drivers/i915/build.sh link       link the objects (illumos only)
#   drivers/i915/build.sh all        both
#
# Environment (defaults in brackets):
#   LINUX        the patched Linux tree (fetch-and-patch.sh) [required]
#   REPO         the illumos-drm checkout [two directories above]
#   CC           the compiler [gcc]
#   LD           the illumos link-editor [/usr/bin/ld]
#   SYSINC       the illumos system headers (sys/, vm/, asm/) [/usr/include]
#   DDI_MAPFILE  the gate's usr/src/uts/common/mapfiles/ddi.mapfile
#   OUT          build directory [./out-i915]
#   JOBS         parallel compiles [2]
#   TUS          list of TUs relative to drivers/gpu/drm/i915 [tus.txt]
#   CROSS        set to 1 when CC is a Linux-hosted x86_64 cross gcc (the
#                measurement container): drops illumos-only flags and
#                undefines the Linux target macros.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-$(cd "$HERE/../.." && pwd)}
: "${LINUX:?set LINUX to the patched Linux tree}"
CC=${CC:-gcc}
LD=${LD:-/usr/bin/ld}
SYSINC=${SYSINC:-/usr/include}
OUT=${OUT:-$PWD/out-i915}
JOBS=${JOBS:-2}
TUS=${TUS:-$HERE/tus.txt}
CROSS=${CROSS:-0}
I915=$LINUX/drivers/gpu/drm/i915
mode=${1:-all}

mkdir -p "$OUT/obj" "$OUT/log" "$OUT/sysinc"
# Only the kernel header directories of the system include tree, so a
# Linux header the layer lacks is an error and not a userland header.
for d in sys vm asm ia32 amd64 netinet inet net rpc; do
	[ -e "$SYSINC/$d" ] && ln -sfn "$SYSINC/$d" "$OUT/sysinc/$d"
done

TMPD=$(mktemp -d /tmp/i915-build.XXXXXX)
trap 'rm -rf "$TMPD"' EXIT
printf 'int probe;\n' > "$TMPD/probe.c"
probe() {
	"$CC" -m64 -Werror "$@" -c -o "$TMPD/probe.o" "$TMPD/probe.c" >/dev/null 2>&1
}

# The gate's 64-bit kernel flags (as drivers/cdrmnull/build.sh), with the
# Linux dialect i915 needs: gnu11, -fms-extensions (anonymous struct
# members by tag), unsigned char, no strict aliasing/overflow.
CFLAGS="-m64 -O2 -std=gnu11 -fno-strict-aliasing -fno-strict-overflow"
CFLAGS="$CFLAGS -fno-optimize-sibling-calls -ffreestanding -mcmodel=kernel"
CFLAGS="$CFLAGS -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-avx"
CFLAGS="$CFLAGS -fno-builtin -fno-common -fno-asynchronous-unwind-tables"
CFLAGS="$CFLAGS -fms-extensions -funsigned-char -fno-delete-null-pointer-checks"
CFLAGS="$CFLAGS -fno-jump-tables -fdiagnostics-show-option"
CFLAGS="$CFLAGS -Wall -Wno-unused-parameter -Wno-sign-compare"
CFLAGS="$CFLAGS -Wno-missing-field-initializers -Wno-unused-function"
CFLAGS="$CFLAGS -Wno-address-of-packed-member -Wno-format-truncation"
CFLAGS="$CFLAGS -Wno-microsoft-anon-tag -Wno-override-init"
CFLAGS="$CFLAGS -Werror=implicit-function-declaration -Werror=implicit-int"
CFLAGS="$CFLAGS -Werror=incompatible-pointer-types -Werror=int-conversion"
CFLAGS="$CFLAGS -Werror=return-type"
for f in -msave-args "-mindirect-branch=thunk-extern -mindirect-branch-register" \
    -fno-shrink-wrap -fno-ipa-cp -fno-ipa-icf -fno-clone-functions \
    -fno-reorder-functions -fno-reorder-blocks-and-partition \
    -fno-aggressive-loop-optimizations -fstack-protector-strong; do
	# shellcheck disable=SC2086
	if [ "$CROSS" = 1 ] && [ "$f" = -msave-args ]; then continue; fi
	# shellcheck disable=SC2086
	probe $f && CFLAGS="$CFLAGS $f"
done
# shellcheck disable=SC2086
CFLAGS=$(for f in $CFLAGS; do probe "$f" && printf '%s ' "$f"; done)

CPPFLAGS="-D_KERNEL -D_SYSCALL32 -D_SYSCALL32_IMPL -D_ELF64 -D_DDI_STRICT"
CPPFLAGS="$CPPFLAGS -Dsun -D__sun -D__SVR4 -D__illumos__ -D_ASM_INLINES"
CPPFLAGS="$CPPFLAGS -D__KERNEL__ -DMODULE -DI915 -DKBUILD_MODNAME=\\\"i915\\\""
if [ "$CROSS" = 1 ]; then
	CPPFLAGS="$CPPFLAGS -U__linux__ -U__linux -Ulinux -U__gnu_linux__ -U__unix__ -Uunix"
fi
CPPFLAGS="$CPPFLAGS -nostdinc -I$REPO/linuxkpi/include -I$REPO/include"
CPPFLAGS="$CPPFLAGS -I$LINUX/.illumos/mit-include -I$I915 -I$OUT/sysinc"
CPPFLAGS="$CPPFLAGS -include $HERE/config.h -include $REPO/linuxkpi/include/linux/kconfig.h"
CPPFLAGS="$CPPFLAGS -include $REPO/linuxkpi/include/lkpi/prologue.h"
echo "$CC $CFLAGS $CPPFLAGS" > "$OUT/flags.txt"
"$CC" --version | sed -n 1p > "$OUT/compiler.txt"

compile_one() {
	tu=$1
	obj=$OUT/obj/${tu%.c}.o
	log=$OUT/log/$(echo "${tu%.c}" | tr / _).log
	mkdir -p "$(dirname "$obj")"
	# shellcheck disable=SC2086
	if "$CC" $CFLAGS $CPPFLAGS -DKBUILD_BASENAME=\"$(basename "${tu%.c}")\" \
	    -c "$I915/$tu" -o "$obj" > "$log" 2>&1; then
		echo "OK $tu"
	else
		rm -f "$obj"
		echo "FAIL $tu"
	fi
}
export -f compile_one
export CC CFLAGS CPPFLAGS OUT I915

if [ "$mode" = compile ] || [ "$mode" = all ]; then
	grep -v '^#' "$TUS" | grep . | xargs -P "$JOBS" -I{} bash -c 'compile_one {}' \
	    > "$OUT/results.txt"
	ok=$(grep -c '^OK' "$OUT/results.txt")
	fail=$(grep -c '^FAIL' "$OUT/results.txt")
	{
		echo "compiled: $ok ok, $fail failed ($(wc -l < "$OUT/results.txt") TUs)"
		echo "--- missing headers (count)"
		grep -ho 'fatal error: [^:]*: No such file' "$OUT"/log/*.log |
		    sed 's/fatal error: //; s/: No such file//' | sort | uniq -c | sort -rn
		echo "--- first errors (count, location: message)"
		for l in "$OUT"/log/*.log; do
			grep -m1 -E 'error:' "$l" | sed "s|$OUT/sysinc|SYS|; s|$LINUX/||; s|$REPO/||"
		done | sort | uniq -c | sort -rn | head -80
		echo "--- error kinds (count)"
		grep -h 'error:' "$OUT"/log/*.log | sed -E 's/^[^ ]+ error: //' |
		    sed -E "s/'[^']*'/X/g" | sort | uniq -c | sort -rn | head -60
	} > "$OUT/summary.txt"
	head -1 "$OUT/summary.txt"
fi
