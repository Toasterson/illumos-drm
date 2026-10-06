#!/bin/bash
#
# SPDX-License-Identifier: MIT
# Copyright 2026 The illumos-drm authors
#
# Builds misc/drm_kms: Linux's DRM KMS code (the TUs of tus.txt, from the
# Linux tree that drivers/i915/fetch-and-patch.sh prepared, with this
# directory's patches applied) and the illumos sources of illumos/, with
# the kernel flags of linuxkpi/kflags.sh and i915n's configuration
# (drivers/i915/config.h), then links the kernel module
#   ld -ztype=kmod -N misc/drm_core -N misc/linuxkpi -N misc/drm_helpers \
#      -z defs -B reduce -M ddi.mapfile -M drm_facade.mapfile \
#      -M linuxkpi.mapfile -M drm_helpers.mapfile -M private.mapfile \
#      -M <scope mapfile>
# so that every undefined symbol is declared by a mapfile of a module it
# depends on (or the DDI's) and only the exported symbols stay global.
#
# Exports: the functions and variables the carried Linux files export
# (EXPORT_SYMBOL*, found in the sources) and the illumos sources' exports
# (marked with EXPORT_SYMBOL too), as far as the objects define them.
# The list is drm_kms.mapfile, which drivers link against
# (-N misc/drm_kms -M drm_kms.mapfile); the build generates it again and
# fails if the committed file differs (refresh it with UPDATE_MAPFILE=1)
# or if the module's global symbols are not exactly the mapfile's.
#
#   carried/drm_kms/build.sh compile    compile (-k style), licence check
#   carried/drm_kms/build.sh link       link the objects (illumos only)
#   carried/drm_kms/build.sh all        both
#
# Environment (defaults in brackets):
#   LINUX        the patched Linux tree (drivers/i915/fetch-and-patch.sh)
#                [required]
#   REPO         the illumos-drm checkout [two directories above]
#   CC           the compiler [gcc]
#   LD           the illumos link-editor [/usr/bin/ld]
#   NM           GNU nm for the export list [gnm; x86_64-linux-gnu-nm with CROSS]
#   SYSINC       the illumos system headers [/usr/include]
#   DDI_MAPFILE  the gate's usr/src/uts/common/mapfiles/ddi.mapfile
#   LKPI_MAPFILE linuxkpi.mapfile from linuxkpi/build.sh
#   FACADE_MAPFILE the facade's mapfile [include/drm/drm_facade.mapfile]
#   OUT          build directory [./out-drm_kms]
#   JOBS         parallel compiles [2]
#   CROSS        1 when CC is a Linux-hosted x86_64 cross gcc (see
#                linuxkpi/kflags.sh): compile and export check only
set -u
PATH=/usr/gnu/bin:$PATH
export LC_ALL=C

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-$(cd "$HERE/../.." && pwd)}
: "${LINUX:?set LINUX to the patched Linux tree}"
CC=${CC:-gcc}
LD=${LD:-/usr/bin/ld}
SYSINC=${SYSINC:-/usr/include}
OUT=${OUT:-$PWD/out-drm_kms}
JOBS=${JOBS:-2}
CROSS=${CROSS:-0}
# GNU nm (BSD output format) for the export list: gnm on illumos.
if [ "$CROSS" = 1 ]; then
	NM=${NM:-x86_64-linux-gnu-nm}
else
	NM=${NM:-$(command -v gnm || echo /usr/gnu/bin/nm)}
fi
I915=$REPO/drivers/i915
mode=${1:-all}

die() {
	echo "drm_kms/build.sh: $*" >&2
	exit 1
}

[ -f "$LINUX/.illumos/applied" ] ||
    die "$LINUX is not prepared: run drivers/i915/fetch-and-patch.sh $LINUX"
mkdir -p "$OUT/obj" "$OUT/log"
OUT=$(cd "$OUT" && pwd)
# shellcheck disable=SC1091
. "$REPO/linuxkpi/kflags.sh"
CFLAGS=$LKPI_CFLAGS
CPPFLAGS="$LKPI_CPPFLAGS -DKBUILD_MODNAME=\"drm_kms\""
CPPFLAGS="$CPPFLAGS -nostdinc -I$REPO/linuxkpi/include -I$REPO/include"
CPPFLAGS="$CPPFLAGS -I$LINUX/.illumos/mit-include -I$LINUX/.illumos/mit-include/uapi"
CPPFLAGS="$CPPFLAGS -I$LINUX/drivers/gpu/drm -I$OUT/sysinc"
CPPFLAGS="$CPPFLAGS -include $I915/config.h -include $REPO/linuxkpi/include/linux/kconfig.h"
CPPFLAGS="$CPPFLAGS -include $REPO/linuxkpi/include/lkpi/prologue.h"
echo "$CC $CFLAGS $CPPFLAGS" > "$OUT/flags.txt"
"$CC" --version | sed -n 1p > "$OUT/compiler.txt"

# The TUs: Linux paths (tus.txt) and the illumos sources.
tus() {
	grep -v '^#' "$HERE/tus.txt" | grep .
	(cd "$HERE" && ls illumos/*.c)
}

compile_one() {
	tu=$1
	case "$tu" in
	illumos/*) src=$HERE/$tu ;;
	*) src=$LINUX/$tu ;;
	esac
	b=$(basename "${tu%.c}")
	obj=$OUT/obj/$b.o
	# shellcheck disable=SC2086
	if "$CC" $CFLAGS $CPPFLAGS -DKBUILD_BASENAME=\"$b\" \
	    -MD -MF "$OUT/obj/$b.d" -c "$src" -o "$obj" > "$OUT/log/$b.log" 2>&1; then
		echo "OK $tu"
	else
		rm -f "$obj"
		echo "FAIL $tu"
	fi
}
export -f compile_one
export CC CFLAGS CPPFLAGS OUT HERE LINUX

if [ "$mode" = compile ] || [ "$mode" = all ]; then
	# The basenames must be unique: objects are named after them.
	dup=$(tus | xargs -n1 basename | sort | uniq -d)
	[ -z "$dup" ] || die "TUs with the same basename: $dup"
	# A new compile's logs and objects only; `link` keeps them.
	rm -rf "$OUT/obj" "$OUT/log"
	mkdir -p "$OUT/obj" "$OUT/log"
	tus | xargs -P "$JOBS" -I{} bash -c 'compile_one {}' > "$OUT/results.txt"
	ok=$(grep -c '^OK' "$OUT/results.txt")
	fail=$(grep -c '^FAIL' "$OUT/results.txt")
	{
		echo "compiled: $ok ok, $fail failed ($(wc -l < "$OUT/results.txt") TUs)"
		grep '^FAIL' "$OUT/results.txt"
		echo "--- missing headers (count)"
		grep -ho 'fatal error: [^:]*: No such file' "$OUT"/log/*.log |
		    sed 's/fatal error: //; s/: No such file//' | sort | uniq -c | sort -rn
		echo "--- errors (count, location: message)"
		grep -h 'error:' "$OUT"/log/*.log | sed "s|$OUT/sysinc|SYS|; s|$LINUX/||; s|$REPO/||" |
		    sort | uniq -c | sort -rn | head -80
	} > "$OUT/summary.txt"
	head -1 "$OUT/summary.txt"
	[ "$fail" -eq 0 ] || exit 1
	# Every file of the Linux tree the compiler read, for the licence
	# check and the package's licence file.
	cat "$OUT"/obj/*.d | tr ' \\' '\n\n' | grep "^$LINUX/" | grep -v ':$' |
	    sed "s|^$LINUX/||" | sort -u > "$OUT/linux-inputs.txt"
	echo "linux inputs: $(wc -l < "$OUT/linux-inputs.txt") files"
	[ -s "$OUT/linux-inputs.txt" ] ||
	    die "no Linux inputs found in the dependency files"
	python3 "$I915/check-licences.py" "$LINUX" "$OUT/linux-inputs.txt" || exit 1
fi

# The export list: every name an EXPORT_SYMBOL* of a compiled source
# (Linux TU or illumos source) names and an object defines globally.
exports() {
	tus | while read -r tu; do
		case "$tu" in
		illumos/*) cat "$HERE/$tu" ;;
		*) cat "$LINUX/$tu" ;;
		esac
	done | sed -n 's/^[ \t]*EXPORT_SYMBOL[A-Z_]*(\([A-Za-z_][A-Za-z0-9_]*\)[,)].*/\1/p' |
	    sort -u > "$OUT/exported.txt"
	"$NM" -g --defined-only "$OUT"/obj/*.o 2>/dev/null | awk 'NF == 3 { print $3 }' |
	    sort -u > "$OUT/defined.txt"
	comm -12 "$OUT/exported.txt" "$OUT/defined.txt"
}

mapfiles() {
	exports > "$OUT/exports.txt"
	{
		echo '#'
		echo '# SPDX-License-Identifier: MIT'
		echo '# Generated by carried/drm_kms/build.sh (UPDATE_MAPFILE=1): the symbols'
		echo '# of misc/drm_kms a driver may use, i.e. what the carried Linux files'
		echo '# and the illumos sources export (EXPORT_SYMBOL*). Link with'
		echo '#   ld -ztype=kmod -N misc/drm_kms -z defs ... -M drm_kms.mapfile'
		echo '#'
		echo '$mapfile_version 2'
		echo ''
		echo 'SYMBOL_SCOPE {'
		echo '	global:'
		sed 's/^/		/; s/$/	{ FLAGS = EXTERN };/' "$OUT/exports.txt"
		echo '};'
	} > "$OUT/drm_kms.mapfile"
	{
		echo '# Generated by carried/drm_kms/build.sh: the symbol scope of misc/drm_kms.'
		echo '$mapfile_version 2'
		echo 'SYMBOL_SCOPE {'
		echo '	global:'
		printf '\t\t%s;\n' _init _fini _info
		sed 's/^/		/; s/$/;/' "$OUT/exports.txt"
		echo '	local:'
		echo '		*;'
		echo '};'
	} > "$OUT/scope.mapfile"
	if [ "${UPDATE_MAPFILE:-0}" = 1 ]; then
		cp "$OUT/drm_kms.mapfile" "$HERE/drm_kms.mapfile"
		echo "drm_kms.mapfile: updated ($(wc -l < "$OUT/exports.txt") symbols)"
	elif ! diff -u "$HERE/drm_kms.mapfile" "$OUT/drm_kms.mapfile" > "$OUT/mapfile.diff"; then
		cat "$OUT/mapfile.diff"
		die "drm_kms.mapfile differs from the exports of this build" \
		    "(review the diff; UPDATE_MAPFILE=1 refreshes it)"
	else
		echo "drm_kms.mapfile: $(wc -l < "$OUT/exports.txt") exported symbols, as committed"
	fi
}

if [ "$mode" = compile ] || [ "$mode" = all ]; then
	mapfiles
fi

if [ "$mode" = link ] || [ "$mode" = all ]; then
	[ "$CROSS" = 1 ] && { echo "drm_kms/build.sh: CROSS=1, not linked"; exit 0; }
	: "${DDI_MAPFILE:?set DDI_MAPFILE to the ddi.mapfile of illumos-gate}"
	: "${LKPI_MAPFILE:?set LKPI_MAPFILE to linuxkpi.mapfile from linuxkpi/build.sh}"
	HELPERS_MAPFILE=${HELPERS_MAPFILE:-$REPO/carried/drm_helpers/drm_helpers.mapfile}
	FACADE_MAPFILE=${FACADE_MAPFILE:-$REPO/include/drm/drm_facade.mapfile}
	[ -f "$OUT/scope.mapfile" ] || mapfiles
	objs=$(ls "$OUT"/obj/*.o)
	echo "\$ $LD -ztype=kmod -N misc/drm_core -N misc/linuxkpi -N misc/drm_helpers" \
	    "-z defs -B reduce -M $DDI_MAPFILE -M $FACADE_MAPFILE" \
	    "-M $LKPI_MAPFILE -M $HELPERS_MAPFILE -M $HERE/private.mapfile" \
	    "-M $OUT/scope.mapfile -o $OUT/drm_kms <$(echo "$objs" | wc -l) objects>" |
	    tee "$OUT/link.txt"
	# shellcheck disable=SC2086
	if "$LD" -ztype=kmod -N misc/drm_core -N misc/linuxkpi -N misc/drm_helpers \
	    -z defs -B reduce -M "$DDI_MAPFILE" -M "$FACADE_MAPFILE" \
	    -M "$LKPI_MAPFILE" -M "$HELPERS_MAPFILE" -M "$HERE/private.mapfile" \
	    -M "$OUT/scope.mapfile" -o "$OUT/drm_kms" $objs >> "$OUT/link.txt" 2>&1; then
		echo "link: OK, 0 unresolved" | tee -a "$OUT/link.txt"
	else
		sed -n '/Undefined/,/symbol referencing errors/p' "$OUT/link.txt" | head -80
		echo "link: FAILED (see $OUT/link.txt)"
		exit 1
	fi
	# The module's global definitions are exactly the exports.
	/usr/bin/nm "$OUT/drm_kms" | awk -F'|' '$5 ~ /GLOB/ && $7 !~ /UNDEF/ {
	    gsub(/ /, "", $8); print $8 }' | grep -v -x -e _init -e _fini -e _info |
	    sort > "$OUT/globals.txt"
	diff "$OUT/exports.txt" "$OUT/globals.txt" ||
	    die "misc/drm_kms' global symbols differ from drm_kms.mapfile"
	echo "misc/drm_kms: $(wc -l < "$OUT/globals.txt") global symbols = drm_kms.mapfile"
	if command -v elfdump >/dev/null 2>&1; then
		elfdump -d "$OUT/drm_kms" | grep -E 'NEEDED|SUNW_KMOD' || true
	fi
fi
