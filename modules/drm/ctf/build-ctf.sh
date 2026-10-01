#!/bin/sh
#
# SPDX-License-Identifier: MIT
# Copyright 2026 The illumos-drm authors
#
# Builds drm_facade_ctf.o, the CTF shadow of misc/drm (drm_facade_ctf.c):
# compiles the translation unit with DWARF and turns the DWARF into CTF
# with the illumos-gate's ctfconvert (package developer/build/onbld). The
# object is then linked into misc/drm next to the Rust archive:
#
#   ld -ztype=kmod -N fs/specfs -B reduce -M modules/drm/drm.mapfile \
#       -z allextract drm.a -z defaultextract drm_facade_ctf.o -o drm
#
# Its variables become local with the rest of the module; its .SUNW_ctf
# section is the module's CTF, a standalone container (no parent: it
# carries the kernel types the facade uses, kmutex_t and the like), so
# mdb -k resolves `::print -t struct drm_device` or `::print -t
# "drm`struct dma_fence"` from it.
#
# Environment (defaults in brackets):
#   CC          the compiler [gcc]
#   CTFCONVERT  ctfconvert [/opt/onbld/bin/i386/ctfconvert]
#   REPO        the illumos-drm checkout [three directories above this script]
#   SYSINC      the kernel headers [/usr/include]
#   OUT         where drm_facade_ctf.o goes [.]
#
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-$(cd "$HERE/../../.." && pwd)}
CC=${CC:-gcc}
CTFCONVERT=${CTFCONVERT:-/opt/onbld/bin/i386/ctfconvert}
SYSINC=${SYSINC:-/usr/include}
OUT=${OUT:-.}

[ -x "$CTFCONVERT" ] || { echo "build-ctf.sh: no $CTFCONVERT" >&2; exit 1; }
mkdir -p "$OUT"

# The kernel's preprocessor flags (as drivers/cdrmnull/build.sh) and
# DWARF 4 that ctfconvert reads; no optimisation, nothing is executed.
set -x
"$CC" -m64 -std=gnu99 -O0 -gdwarf-4 -fno-eliminate-unused-debug-types \
    -ffreestanding -mcmodel=kernel -mno-red-zone -fno-builtin \
    -D_KERNEL -D_SYSCALL32 -D_ELF64 -D_DDI_STRICT -Dsun -D__sun -D__SVR4 \
    -nostdinc -I"$REPO/include" -I"$SYSINC" \
    -c "$HERE/drm_facade_ctf.c" -o "$OUT/drm_facade_ctf.o"
"$CTFCONVERT" -l drm "$OUT/drm_facade_ctf.o"
# The DWARF has served its purpose (300 KB of it); keep the CTF.
for s in $(elfdump -c "$OUT/drm_facade_ctf.o" |
    awk '/sh_name: \.debug/ {print $4}'); do
	mcs -d -n "$s" "$OUT/drm_facade_ctf.o"
done
