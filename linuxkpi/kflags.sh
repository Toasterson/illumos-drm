# SPDX-License-Identifier: MIT
# Copyright 2026 The illumos-drm authors
#
# The compiler flags of everything built against the LinuxKPI layer (the
# layer itself and the Linux drivers): the illumos-gate's 64-bit kernel
# flags as drivers/cdrmnull/build.sh uses them, plus the Linux dialect
# (gnu11, -fms-extensions, unsigned char, no strict aliasing/overflow).
# Sourced by linuxkpi/build.sh and drivers/i915/build.sh with CC, CROSS,
# SYSINC, REPO and OUT set; sets LKPI_CFLAGS and LKPI_CPPFLAGS.
#
#   CROSS=1   CC is a Linux-hosted x86_64 cross gcc (development only):
#             drop illumos-only flags, undefine the Linux target macros.

mkdir -p "$OUT/sysinc"
# Only the kernel header directories of the system include tree, so a
# Linux header the layer lacks is an error and not a userland header.
for d in sys vm asm ia32 amd64 netinet inet net rpc; do
	[ -e "$SYSINC/$d" ] && ln -sfn "$SYSINC/$d" "$OUT/sysinc/$d"
done

_lkpi_tmp=$(mktemp -d /tmp/lkpi-flags.XXXXXX)
printf 'int probe;\n' > "$_lkpi_tmp/probe.c"
_lkpi_probe() {
	"$CC" -m64 -Werror "$@" -c -o "$_lkpi_tmp/probe.o" "$_lkpi_tmp/probe.c" >/dev/null 2>&1
}

# The flags a kernel module cannot do without: a compiler that refuses
# one of them cannot build the layer (checked below, not dropped).
_lkpi_must="-m64 -O2 -std=gnu11 -fno-strict-aliasing -fno-strict-overflow"
_lkpi_must="$_lkpi_must -ffreestanding -mcmodel=kernel -fno-pie -fno-pic -mno-red-zone"
_lkpi_must="$_lkpi_must -mno-mmx -mno-sse -mno-sse2 -mno-avx -mno-80387 -mno-fp-ret-in-387"
LKPI_CFLAGS="$_lkpi_must -fno-optimize-sibling-calls"
LKPI_CFLAGS="$LKPI_CFLAGS -fno-builtin -fno-common -fno-asynchronous-unwind-tables"
LKPI_CFLAGS="$LKPI_CFLAGS -fms-extensions -funsigned-char -fno-delete-null-pointer-checks"
LKPI_CFLAGS="$LKPI_CFLAGS -fno-jump-tables -fdiagnostics-show-option"
LKPI_CFLAGS="$LKPI_CFLAGS -Wall -Wno-unused-parameter -Wno-sign-compare"
LKPI_CFLAGS="$LKPI_CFLAGS -Wno-missing-field-initializers -Wno-unused-function"
LKPI_CFLAGS="$LKPI_CFLAGS -Wno-address-of-packed-member -Wno-format-truncation"
LKPI_CFLAGS="$LKPI_CFLAGS -Wno-microsoft-anon-tag -Wno-override-init -Wno-comment"
# Linux builds with -Wno-pointer-sign too (char * vs u8 * throughout).
LKPI_CFLAGS="$LKPI_CFLAGS -Wno-pointer-sign"
LKPI_CFLAGS="$LKPI_CFLAGS -Werror=implicit-function-declaration -Werror=implicit-int"
LKPI_CFLAGS="$LKPI_CFLAGS -Werror=incompatible-pointer-types -Werror=int-conversion"
LKPI_CFLAGS="$LKPI_CFLAGS -Werror=return-type"
# The gate's flags for DTrace fbt and stack traces (as cw passes them),
# where the compiler takes them.
for _f in -msave-args "-mindirect-branch=thunk-extern -mindirect-branch-register" \
    -fno-shrink-wrap -fno-ipa-cp -fno-ipa-icf -fno-clone-functions \
    -fno-reorder-functions -fno-reorder-blocks-and-partition \
    -fno-inline-small-functions -fno-inline-functions-called-once \
    -fno-aggressive-loop-optimizations -fstack-protector-strong; do
	if [ "${CROSS:-0}" = 1 ] && [ "$_f" = -msave-args ]; then continue; fi
	# shellcheck disable=SC2086
	_lkpi_probe $_f && LKPI_CFLAGS="$LKPI_CFLAGS $_f"
done
# shellcheck disable=SC2086
if ! _lkpi_probe $_lkpi_must; then
	echo "kflags.sh: $CC does not take the kernel flags $_lkpi_must" >&2
	rm -rf "$_lkpi_tmp"
	exit 1
fi
# The warning and dialect flags: kept where the compiler takes them
# (each probed with the kernel flags, which some depend on).
# shellcheck disable=SC2086
LKPI_CFLAGS=$(for _f in $LKPI_CFLAGS; do _lkpi_probe $_lkpi_must "$_f" && printf '%s ' "$_f"; done)
rm -rf "$_lkpi_tmp"

LKPI_CPPFLAGS="-D_KERNEL -D_SYSCALL32 -D_SYSCALL32_IMPL -D_ELF64 -D_DDI_STRICT"
LKPI_CPPFLAGS="$LKPI_CPPFLAGS -Dsun -D__sun -D__SVR4 -D__illumos__ -D_ASM_INLINES"
LKPI_CPPFLAGS="$LKPI_CPPFLAGS -D__KERNEL__ -DMODULE"
if [ "${CROSS:-0}" = 1 ]; then
	LKPI_CPPFLAGS="$LKPI_CPPFLAGS -U__linux__ -U__linux -Ulinux -U__gnu_linux__ -U__unix__ -Uunix"
fi
