#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Links, loads, tests and unloads rdrmnull on an illumos test machine.
# Run as root in a directory that holds:
#
#   rdrmnull.a       from `cargo xtask build rdrmnull`
#   rdrmnull.conf    examples/rdrmnull/rdrmnull.conf
#   drm_r0_test.c    tools/test/drm_r0_test.c
#   drm/             the patched libdrm 2.4.109 headers (tools/uapi/generate.sh)
#   drm-test-lib.sh  tools/test/drm-test-lib.sh
#   drm.a            from `cargo xtask build drm`, and drm.mapfile
#                    (modules/drm/drm.mapfile), unless misc/drm
#                    is installed already
#
# rdrmnull's nodes are card<N> and renderD<128+N> for the device number N
# it gets from misc/drm (see run-r1-test.sh).
#
# The machine needs gcc and the libdrm 2.4.109 library in /usr/lib/xorg/amd64
# (x11/library/libdrm). The script changes nothing outside this directory
# except for installing the driver under /usr/kernel/drv, add_drv/rem_drv,
# and the /dev links devfsadm creates and removes; it undoes all of them.
# Kernel panics are possible with any kernel module under test: use a
# disposable VM.
set -u
cd "$(dirname "$0")" || exit 1

step() {
	echo
	echo "\$ $*"
	"$@"
	rc=$?
	echo "[exit $rc]"
	return $rc
}

. ./drm-test-lib.sh

fail=0
had_dri=no
[ -d /dev/dri ] && had_dri=yes

step uname -a
drm_core_install || exit 1
step digest -a sha256 rdrmnull.a
# -N fs/specfs: makespecvp, for the file descriptors of dma-bufs, sync
# objects and sync files (illumos-kernel-private-sys), lives in the specfs
# module, which krtld does not search unless the module depends on it.
# -N misc/drm: the DRM core, which every driver links against.
step ld -ztype=kmod -N misc/drm -z allextract rdrmnull.a -o rdrmnull || exit 1
step digest -a sha256 rdrmnull
step elfdump -d rdrmnull
echo
echo "\$ nm -u rdrmnull: every undefined symbol must resolve in the running kernel"
nm -u rdrmnull | awk 'NF == 1 && $1 ~ /^[A-Za-z_]/ {print $1}' | sort -u > undefined.txt
tr '\n' ' ' < undefined.txt; echo
unresolved=0
for s in $(cat undefined.txt); do
	w=$(echo "$s::whatis" | mdb -k 2>&1)
	case $w in
	*" is "*) ;;
	*) echo "unresolved: $s: $w"; unresolved=$((unresolved + 1)) ;;
	esac
done
echo "undefined symbols not exported by the kernel: $unresolved"
[ $unresolved -eq 0 ] || exit 1

step gcc -m64 -Wall -I drm -o drm_r0_test drm_r0_test.c \
    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lpthread || exit 1
step gcc -m32 -Wall -DPROBE32 -I drm -o drm_r0_probe32 drm_r0_test.c || exit 1

step cp rdrmnull /usr/kernel/drv/amd64/rdrmnull || exit 1
step cp rdrmnull.conf /usr/kernel/drv/rdrmnull.conf || exit 1
step add_drv -m '* 0666 root sys' rdrmnull || fail=1
if [ $fail -eq 0 ]; then
	step modinfo -c | grep rdrmnull
	step sh -c 'modinfo | grep rdrmnull'
	step ls -l /devices/pseudo/ | grep rdrmnull
	step ls -l /dev/dri
	step ls -lL /dev/dri
	rdrmnull_nodes || fail=1
	step sh -c 'tail -3 /var/adm/messages'
	step ./drm_r0_test || fail=1
	step ./drm_r0_probe32 || fail=1

	# The module must stay loaded while a file is open: modunload (the
	# unload rem_drv would attempt, without rem_drv's removal of the driver
	# configuration) has to fail, and succeed once the file is closed.
	# rem_drv itself is not used for the refusal because it removes the
	# /etc entries even when the unload fails.
	echo
	echo "\$ ./drm_r0_test hold $DRM_CARD & (open file held on a fifo)"
	rm -f hold.fifo && mkfifo hold.fifo
	./drm_r0_test hold "$DRM_CARD" < hold.fifo &
	holder=$!
	exec 9> hold.fifo
	sleep 1
	id=$(modinfo | awk '$6 == "rdrmnull" {print $1}')
	echo "rdrmnull module id: $id"
	if [ -z "$id" ]; then
		echo "FAIL rdrmnull is not loaded"
		fail=1
	elif step modunload -i "$id"; then
		echo "FAIL modunload succeeded while a file was open"
		fail=1
	else
		echo "PASS modunload refused while $DRM_CARD is open"
	fi
	step sh -c 'modinfo | grep rdrmnull'
	exec 9>&-
	wait $holder
	echo "holder exited with $?"
	rm -f hold.fifo
	if [ -n "$id" ] && step modunload -i "$id"; then
		echo "PASS modunload succeeded after the file was closed"
	else
		echo "FAIL modunload failed after the file was closed"
		fail=1
	fi
	step sh -c 'modinfo | grep rdrmnull || echo "rdrmnull not loaded"'
	# Open the device once more so that the driver attaches again, and
	# unload through rem_drv below.
	step ./drm_r0_test hold "$DRM_RENDER" < /dev/null || fail=1
	step sh -c 'modinfo | grep rdrmnull'
	step sh -c 'echo "::prtconf -d rdrmnull" | mdb -k'
fi

step rem_drv rdrmnull || fail=1
step sh -c 'modinfo | grep rdrmnull || echo "rdrmnull not loaded"'
step rm -f /usr/kernel/drv/amd64/rdrmnull /usr/kernel/drv/rdrmnull.conf
step devfsadm -C
if [ -d /dev/dri ]; then
	step ls -la /dev/dri
	# devfsadm -C only removes links that a link module claims: every link
	# to an rdrmnull node goes, the directory only if the test made it.
	remove_dri_links rdrmnull@
	[ $had_dri = no ] && step rmdir /dev/dri
fi
drm_core_remove || fail=1
step sh -c 'ls -la /dev/dri 2>&1; grep -n rdrmnull /etc/name_to_major /etc/minor_perm /etc/driver_aliases /etc/devlink.tab || echo "no rdrmnull entries in /etc"'
echo
echo "run-r0-test: fail=$fail"
exit $fail
