#!/bin/bash
#
# SPDX-License-Identifier: MIT
# Copyright 2026 The illumos-drm authors
#
# Builds the i915 objects of the pinned Linux tree against the DRM facade
# and the LinuxKPI layer with the illumos kernel's compiler flags, then
# links them into the kernel module "i915n" with
#   ld -ztype=kmod -N misc/drm_core -N misc/linuxkpi -N misc/drm_helpers \
#      -N misc/drm_kms -z defs -M ddi.mapfile -M drm_facade.mapfile \
#      -M linuxkpi.mapfile -M drm_helpers.mapfile -M drm_kms.mapfile ...
# so that every undefined symbol must be declared by one of the mapfiles.
# The display build (I915_DISPLAY=1, the default) adds display-tus.txt to
# tus.txt (its "-" lines take headless stubs out) and links against
# misc/drm_kms; I915_DISPLAY=0 builds R5c's headless i915n from tus.txt
# alone, without misc/drm_kms.
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
#   TUS          list of TUs relative to drivers/gpu/drm/i915 [tus.txt]; entries
#                illumos/*.c are this directory's own sources (MIT)
#   I915_DISPLAY 1: the display build (TUS plus DISPLAY_TUS), 0: headless [1]
#   DISPLAY_TUS  the display build's changes to TUS [display-tus.txt]
#   KMS_MAPFILE  misc/drm_kms' exports [carried/drm_kms/drm_kms.mapfile]
#   CROSS        set to 1 when CC is a Linux-hosted x86_64 cross gcc (the
#                measurement container): drops illumos-only flags and
#                undefines the Linux target macros.
set -u
# GNU grep first on illumos (grep -m in the summary); one collation
PATH=/usr/gnu/bin:$PATH
export LC_ALL=C

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-$(cd "$HERE/../.." && pwd)}
: "${LINUX:?set LINUX to the patched Linux tree}"
CC=${CC:-gcc}
LD=${LD:-/usr/bin/ld}
SYSINC=${SYSINC:-/usr/include}
OUT=${OUT:-$PWD/out-i915}
JOBS=${JOBS:-2}
TUS=${TUS:-$HERE/tus.txt}
I915_DISPLAY=${I915_DISPLAY:-1}
DISPLAY_TUS=${DISPLAY_TUS:-$HERE/display-tus.txt}
KMS_MAPFILE=${KMS_MAPFILE:-$REPO/carried/drm_kms/drm_kms.mapfile}
CROSS=${CROSS:-0}
I915=$LINUX/drivers/gpu/drm/i915
mode=${1:-all}

mkdir -p "$OUT/obj" "$OUT/log"
# shellcheck disable=SC1091
. "$REPO/linuxkpi/kflags.sh"
CFLAGS=$LKPI_CFLAGS
CPPFLAGS="$LKPI_CPPFLAGS -DI915 -DKBUILD_MODNAME=\"i915\""
CPPFLAGS="$CPPFLAGS -nostdinc -I$REPO/linuxkpi/include -I$REPO/include"
CPPFLAGS="$CPPFLAGS -I$LINUX/.illumos/mit-include -I$LINUX/.illumos/mit-include/uapi"
CPPFLAGS="$CPPFLAGS -I$I915 -I$OUT/sysinc"
# Last: the illumos glue's headers the patches include (i915_illumos_vbt.h).
CPPFLAGS="$CPPFLAGS -I$HERE/illumos"
CPPFLAGS="$CPPFLAGS -include $HERE/config.h -include $REPO/linuxkpi/include/linux/kconfig.h"
CPPFLAGS="$CPPFLAGS -include $REPO/linuxkpi/include/lkpi/prologue.h"
case "$I915_DISPLAY" in
1) CPPFLAGS="$CPPFLAGS -DI915N_DISPLAY" ;;
0) ;;
*) echo "build.sh: I915_DISPLAY must be 0 or 1" >&2; exit 1 ;;
esac
# The TU list of this build: TUS, and for the display build DISPLAY_TUS'
# additions without its "-" removals.
grep -v '^#' "$TUS" | grep . > "$OUT/tus.txt"
if [ "$I915_DISPLAY" = 1 ]; then
	grep -v '^#' "$DISPLAY_TUS" | sed -n 's/^-//p' > "$OUT/tus-removed.txt"
	for t in $(cat "$OUT/tus-removed.txt"); do
		grep -qx "$t" "$OUT/tus.txt" ||
		    { echo "build.sh: $DISPLAY_TUS removes $t, which $TUS lacks" >&2; exit 1; }
	done
	grep -vxF -f "$OUT/tus-removed.txt" "$OUT/tus.txt" > "$OUT/tus.tmp"
	grep -v '^#' "$DISPLAY_TUS" | grep . | grep -v '^-' >> "$OUT/tus.tmp"
	mv "$OUT/tus.tmp" "$OUT/tus.txt"
fi
[ -z "$(sort "$OUT/tus.txt" | uniq -d)" ] ||
    { echo "build.sh: a TU is listed twice" >&2; exit 1; }
echo "$CC $CFLAGS $CPPFLAGS" > "$OUT/flags.txt"
"$CC" --version | sed -n 1p > "$OUT/compiler.txt"

compile_one() {
	tu=$1
	obj=$OUT/obj/${tu%.c}.o
	log=$OUT/log/$(echo "${tu%.c}" | tr / _).log
	mkdir -p "$(dirname "$obj")"
	# shellcheck disable=SC2086
	case "$tu" in
	illumos/*) src=$HERE/$tu ;;
	*) src=$I915/$tu ;;
	esac
	if "$CC" $CFLAGS $CPPFLAGS -DKBUILD_BASENAME=\"$(basename "${tu%.c}")\" \
	    -MD -MF "${obj%.o}.d" -c "$src" -o "$obj" > "$log" 2>&1; then
		echo "OK $tu"
	else
		rm -f "$obj"
		echo "FAIL $tu"
	fi
}
export -f compile_one
export CC CFLAGS CPPFLAGS OUT I915 HERE

if [ "$mode" = compile ] || [ "$mode" = all ]; then
	# Only this TU list's objects are linked: start from an empty obj/
	# (and log/; `link` keeps the compile's logs).
	rm -rf "$OUT/obj" "$OUT/log"
	mkdir -p "$OUT/log"
	xargs -P "$JOBS" -I{} bash -c 'compile_one {}' < "$OUT/tus.txt" \
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
	[ "$fail" -eq 0 ] || exit 1
	# Every file of the Linux tree the compiler read (the dependency
	# files), for the licence check and the package's licence file.
	cat "$OUT"/obj/*.d "$OUT"/obj/*/*.d "$OUT"/obj/*/*/*.d 2>/dev/null |
	    tr ' \\' '\n\n' | grep "^$LINUX/" | grep -v ':$' |
	    sed "s|^$LINUX/||" | sort -u > "$OUT/linux-inputs.txt"
	echo "linux inputs: $(wc -l < "$OUT/linux-inputs.txt") files"
	# A path mismatch would leave the list empty and check nothing.
	[ -s "$OUT/linux-inputs.txt" ] ||
	    { echo "build.sh: no Linux inputs found in the dependency files"; exit 1; }
	python3 "$HERE/check-licences.py" "$LINUX" "$OUT/linux-inputs.txt" || exit 1
fi

if [ "$mode" = link ] || [ "$mode" = all ]; then
	# The link gate: every undefined symbol must be declared by one of
	# the mapfiles of the modules i915n depends on, or the DDI's.
	: "${DDI_MAPFILE:?set DDI_MAPFILE to the ddi.mapfile of illumos-gate}"
	: "${LKPI_MAPFILE:?set LKPI_MAPFILE to linuxkpi.mapfile from linuxkpi/build.sh}"
	HELPERS_MAPFILE=${HELPERS_MAPFILE:-$REPO/carried/drm_helpers/drm_helpers.mapfile}
	objs=$(find "$OUT/obj" -name '*.o' | sort)
	# Only this build's objects (a compile of the other variant may
	# have left some in obj/).
	n=$(wc -l < "$OUT/tus.txt")
	[ "$(echo "$objs" | grep -c .)" -eq "$n" ] ||
	    { echo "build.sh: obj/ does not hold the $n objects of this build; compile first" >&2; exit 1; }
	kms=""
	if [ "$I915_DISPLAY" = 1 ]; then
		kms="-N misc/drm_kms -M $KMS_MAPFILE"
	fi
	echo "\$ $LD -ztype=kmod -N misc/drm_core -N misc/linuxkpi -N misc/drm_helpers $kms -z defs" \
	    "-M $DDI_MAPFILE -M $REPO/include/drm/drm_facade.mapfile -M $LKPI_MAPFILE" \
	    "-M $HELPERS_MAPFILE -M $HERE/private.mapfile -o $OUT/i915n <$n objects>" \
	    | tee "$OUT/link.txt"
	# shellcheck disable=SC2086
	if "$LD" -ztype=kmod -N misc/drm_core -N misc/linuxkpi -N misc/drm_helpers \
	    $kms -z defs -M "$DDI_MAPFILE" -M "$REPO/include/drm/drm_facade.mapfile" \
	    -M "$LKPI_MAPFILE" -M "$HELPERS_MAPFILE" -M "$HERE/private.mapfile" \
	    -o "$OUT/i915n" $objs >> "$OUT/link.txt" 2>&1; then
		echo "link: OK, 0 unresolved" | tee -a "$OUT/link.txt"
	else
		grep -c 'symbol referencing errors\|undefined' "$OUT/link.txt" || true
		sed -n '/Undefined/,/symbol referencing errors/p' "$OUT/link.txt" | head -80
		echo "link: FAILED (see $OUT/link.txt)"
		exit 1
	fi
fi
