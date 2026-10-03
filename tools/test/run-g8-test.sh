#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The guest test of illumos-drm G8: importing or opening an object that a
# file holds already leaves that file's handle working (no driver open or
# close hook runs for it), on cdrmnull, rdrmnull and virtio_gpu.
#
#   run-g8-test.sh kernel      links misc/drm_core (drm_module.a) and the
#                              three drivers, attaches them, runs
#                              drm_g8_test on all three (G8_HOLD=<s> keeps
#                              the DRI3 texture on the display that long),
#                              records the gem kstats, and removes
#                              everything again unless KEEP=1
#   run-g8-test.sh reproducer  G3's two-process dma-buf reproducer
#                              (dmabuf_import_test.c) on virtio_gpu's render
#                              node, with the drivers kept by "kernel KEEP=1";
#                              needs a Mesa with dma-buf import (G3's 25.3.6)
#   run-g8-test.sh cleanup     removes what "kernel KEEP=1" left
#
# Run as root on a disposable guest with a virtio-gpu device (virgl), in a
# directory that holds drm_module.a and drm_core.mapfile, rdrmnull.a,
# rdrmnull.conf, virtio_gpu.a, cdrmnull.conf and either the cdrmnull module
# or the sources to build it (src/include, src/drivers/cdrmnull and the
# gate's ddi.mapfile, as drivers/cdrmnull/build.sh wants them),
# drm_g8_test.c, dmabuf_import_test.c, drm-test-lib.sh and the uAPI headers
# in drm/. Needs gcc, libdrm 2.4.134 (or 2.4.109) and its headers.
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
npass=0
pass() { echo "PASS $*"; npass=$((npass + 1)); }
bad() { echo "FAIL $*"; fail=1; }

ETC="/etc/name_to_major /etc/minor_perm /etc/driver_aliases /etc/driver_classes /etc/devlink.tab
    /etc/security/device_policy"

state() {
	digest -a sha256 $ETC > "g8-state-$1.etc"
	find /devices -print | grep -v "^/devices/pseudo/clone@0:" | sort > "g8-state-$1.devices"
	echo "\$ state $1"
	cat "g8-state-$1.etc"
	echo "/devices entries: $(wc -l < "g8-state-$1.devices")"
}

modid() {
	modinfo | awk -v m="$1" '$6 == m {print $1}'
}

gem_kstats() {
	step sh -c "kstat -p 'cdrmnull:*:gem' 'rdrmnull:*:gem' 'virtio_gpu:*:gem' |
	    grep -E ':(objects|umem_bytes|dma_segments|handles|attached|names|mappings)	'"
}

kernel() {
	[ -f REVISION ] && step cat REVISION
	step uname -a
	step sh -c 'pkg list -H x11/library/libdrm x11/library/mesa 2>&1'
	step devfsadm -C
	state before
	DRM_CORE_DIR=/kernel/misc/amd64
	drm_core_install || exit 1
	# A misc/drm_core left installed by an earlier run is not relinked by
	# drm_core_install: make sure the loaded one is this directory's.
	if cmp -s drm_core $DRM_CORE_DIR/drm_core; then
		pass "the installed misc/drm_core is the one linked here"
	else
		bad "the installed misc/drm_core is not the one linked here"
		exit 1
	fi
	step ld -ztype=kmod -N misc/drm_core -z allextract rdrmnull.a -o rdrmnull || exit 1
	step modload /kernel/misc/amd64/virtio
	step ld -ztype=kmod -N misc/virtio -N misc/drm_core -z allextract virtio_gpu.a -o virtio_gpu || exit 1
	if [ ! -f cdrmnull ]; then
		step env REPO="$PWD/src" DDI_MAPFILE="$PWD/ddi.mapfile" sh src/drivers/cdrmnull/build.sh || exit 1
	fi
	for m in drm_core rdrmnull virtio_gpu cdrmnull; do
		step digest -a sha256 $m
	done
	step gcc -m64 -Wall -I drm -I /usr/include/libdrm -o drm_g8_test drm_g8_test.c \
	    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lkstat || exit 1

	step cp cdrmnull /usr/kernel/drv/amd64/cdrmnull || exit 1
	step cp cdrmnull.conf /usr/kernel/drv/cdrmnull.conf || exit 1
	step add_drv -m '* 0666 root sys' cdrmnull || exit 1
	step cp rdrmnull /usr/kernel/drv/amd64/rdrmnull || exit 1
	step cp rdrmnull.conf /usr/kernel/drv/rdrmnull.conf || exit 1
	step add_drv -m '* 0666 root sys' rdrmnull || exit 1
	step cp virtio_gpu /kernel/drv/amd64/virtio_gpu || exit 1
	step add_drv -i '"pci1af4,1050"' -m '* 0666 root sys' virtio_gpu || exit 1
	step sh -c 'modinfo | grep -E "cdrmnull|rdrmnull|virtio_gpu| drm_core "'
	step ls -l /dev/dri
	c=$(dri_link cdrmnull@ card) || bad "no card node for cdrmnull"
	r=$(dri_link rdrmnull@ card) || bad "no card node for rdrmnull"
	v=$(dri_link pci1af4,1100@ card) || bad "no card node for virtio_gpu"
	vr=$(dri_link pci1af4,1100@ renderD) || bad "no render node for virtio_gpu"
	echo "cdrmnull $c, rdrmnull $r, virtio_gpu $v $vr"
	gem_kstats
	step env DRM_CARD_C="$c" DRM_CARD_R="$r" DRM_CARD_V="$v" DRM_RENDER_V="$vr" ./drm_g8_test > drm_g8_test.out 2>&1
	cat drm_g8_test.out
	if grep -q '^drm_g8_test: [0-9]* passed, 0 failed' drm_g8_test.out; then
		pass "drm_g8_test: $(grep '^drm_g8_test:' drm_g8_test.out)"
	else
		bad "drm_g8_test: $(grep '^drm_g8_test:' drm_g8_test.out)"
	fi
	gem_kstats
	step sh -c "tail -20 /var/adm/messages | grep -E 'virtio_gpu|cdrmnull|rdrmnull|drm_core'"
	echo "$c $r $v $vr" > g8-nodes
	if [ "${KEEP:-0}" != 1 ]; then
		cleanup
	fi
}

reproducer() {
	set -- $(cat g8-nodes)
	step sh -c 'pkg list -H x11/library/mesa 2>&1'
	step gcc -m64 -Wall -D_XOPEN_SOURCE=600 -D__EXTENSIONS__ -o dmabuf_import_test \
	    dmabuf_import_test.c -L/usr/lib/amd64 -R/usr/lib/amd64 -lEGL -lGL -lgbm -lsocket || exit 1
	for mode in twice once draw-first; do
		case $mode in
		twice) extra= ;;
		once) extra="SKIP_A=1" ;;
		draw-first) extra="IMPORTER_DRAW_FIRST=1" ;;
		esac
		# shellcheck disable=SC2086 # $extra is empty or one assignment
		step env $extra DRM_RENDER="$4" ./dmabuf_import_test > dmabuf-$mode.out 2>&1
		cat dmabuf-$mode.out
		if [ $rc -eq 0 ] && ! grep -q '^FAIL' dmabuf-$mode.out; then
			pass "dmabuf_import_test ($mode): $(grep -c '^PASS' dmabuf-$mode.out) checks"
		else
			bad "dmabuf_import_test ($mode)"
		fi
	done
	gem_kstats
}

cleanup() {
	for d in cdrmnull rdrmnull virtio_gpu; do
		id=$(modid $d)
		if [ -n "$id" ] && step modunload -i "$id"; then
			pass "$d unloads"
		else
			bad "$d does not unload (id '$id')"
		fi
	done
	step sh -c "grep -E 'virtio_gpu.*detach:' /var/adm/messages | tail -1"
	step rem_drv -C cdrmnull || fail=1
	step rem_drv -C rdrmnull || fail=1
	step rem_drv virtio_gpu || fail=1
	step rm -f /usr/kernel/drv/amd64/cdrmnull /usr/kernel/drv/cdrmnull.conf /usr/kernel/drv/amd64/rdrmnull \
	    /usr/kernel/drv/rdrmnull.conf /kernel/drv/amd64/virtio_gpu
	step devfsadm -C
	remove_dri_links cdrmnull@
	remove_dri_links rdrmnull@
	remove_dri_links pci1af4,1100@
	DRM_CORE_OURS=yes
	DRM_CORE_DIR=/kernel/misc/amd64
	drm_core_remove || fail=1
	step devfsadm -C
	if [ -f g8-state-before.etc ]; then
		state after
		for f in etc devices; do
			if cmp -s "g8-state-before.$f" "g8-state-after.$f"; then
				pass "state $f unchanged"
			else
				diff "g8-state-before.$f" "g8-state-after.$f"
				bad "state $f changed"
			fi
		done
	fi
}

case ${1:-kernel} in
kernel) kernel ;;
reproducer) reproducer ;;
cleanup) cleanup ;;
*) echo "usage: run-g8-test.sh [kernel|reproducer|cleanup]" >&2; exit 2 ;;
esac
echo "checks passed: $npass"
[ $fail -eq 0 ] && echo "run-g8-test ${1:-kernel}: PASS" || echo "run-g8-test ${1:-kernel}: FAIL"
exit $fail
