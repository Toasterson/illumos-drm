#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Links, loads, tests and unloads rdrmnull on an illumos test machine: the
# R0 and R1 tests as regression (see run-r1-test.sh), the R3 test (mode
# objects, properties, blobs, framebuffers, atomic and legacy mode setting,
# vblanks, events through read(2) and poll(2), the software scanout), and
# libdrm's modetest against the null driver (listing, a legacy mode set
# with a dumb framebuffer, page flips with vblank events, an atomic mode
# set), a detach that is refused while rdrmnull holds a GEM object and
# must leave the display working, with the gem and kms kstats and the vmem
# arenas recorded before unload. Run as root in a directory that holds:
#
#   rdrmnull.a       from `cargo xtask build rdrmnull`
#   rdrmnull.conf    examples/rdrmnull/rdrmnull.conf
#   drm_r0_test.c    tools/test/drm_r0_test.c
#   drm_r1_test.c    tools/test/drm_r1_test.c
#   drm_r3_test.c    tools/test/drm_r3_test.c
#   drm/             the patched libdrm 2.4.109 headers (tools/uapi/generate.sh)
#   drm-test-lib.sh  tools/test/drm-test-lib.sh
#   drm_minor.a      from `cargo xtask build drm_minor`, and drm_minor.mapfile
#                    (modules/drm-minor/exports.mapfile), unless misc/drm_minor
#                    is installed already
#
# rdrmnull's nodes are card<N> and renderD<128+N> for the device number N
# it gets from misc/drm_minor (see run-r1-test.sh); modetest opens it by
# its bus ID, pseudo:rdrmnull@0.
#
# modetest is taken from $MODETEST, default /opt/drm-tests/amd64/modetest
# (package system/test/libdrm, gfx-drm's build of the libdrm 2.4.109
# tests); without it the modetest steps are skipped with a FAIL line.
#
# RDRMNULL_CONF names another .conf to install instead of rdrmnull.conf:
# the DMA drill variants tools/test/rdrmnull-dma-self.conf and
# rdrmnull-dma-root-node.conf (see examples/rdrmnull/src/lib.rs). With
# dma-mode "self" or "root-node" the R1 test expects every object to be
# bound for DMA. "self" attaches under the root nexus, which leaves the
# instance in /etc/path_to_inst after rem_drv (the /etc check fails), and
# "root-node" is expected to panic the machine.
#
# The machine needs gcc and the libdrm 2.4.109 library in /usr/lib/xorg/amd64
# (x11/library/libdrm). The script changes nothing outside this directory
# except for installing the driver under /usr/kernel/drv, add_drv/rem_drv,
# and the /dev links devfsadm creates and removes; it undoes all of them and
# fails unless the driver files in /etc and the /devices tree hash the same
# afterwards as before. Kernel panics are possible with any kernel module
# under test: use a disposable VM.
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

ETC="/etc/name_to_major /etc/minor_perm /etc/driver_aliases /etc/driver_classes /etc/devlink.tab
    /etc/security/device_policy /etc/path_to_inst"

state() {
	# $1: before or after
	digest -a sha256 $ETC > "state-$1.etc"
	find /devices -print | sort > "state-$1.devices"
	ls -la /dev/dri > "state-$1.dri" 2>&1
	echo "::vmem" | mdb -k 2>&1 | awk 'NR == 1 || /umem_np|rdrmnull/' > "state-$1.vmem"
	echo "\$ state $1"
	cat "state-$1.etc"
	echo "/devices entries: $(wc -l < "state-$1.devices")"
	cat "state-$1.dri"
	cat "state-$1.vmem"
}

gemstat() {
	echo
	echo "\$ kstat -p rdrmnull:0:gem ($1)"
	kstat -p rdrmnull:0:gem
	echo "\$ kstat -p rdrmnull:0:kms ($1)"
	kstat -p rdrmnull:0:kms
}

MODETEST=${MODETEST:-/opt/drm-tests/amd64/modetest}

# modetest NAME SECONDS ARGS...: runs modetest with stdin closed after
# SECONDS (it waits for a key press), output in modetest-NAME.out.
modetest_run() {
	name=$1 secs=$2
	shift 2
	echo
	echo "\$ (sleep $secs; echo) | $MODETEST $*"
	(sleep "$secs"; echo) | "$MODETEST" "$@" > "modetest-$name.out" 2>&1
	rc=$?
	cat "modetest-$name.out"
	echo "[exit $rc]"
	return $rc
}

modetest_steps() {
	if [ ! -x "$MODETEST" ]; then
		echo "FAIL modetest not found at $MODETEST"
		fail=1
		return
	fi
	if modetest_run list 0 -M rdrmnull -D pseudo:rdrmnull@0 &&
	    grep -q '^Connectors:' modetest-list.out && grep -q 'connected' modetest-list.out &&
	    grep -q '1024x768' modetest-list.out && grep -q 'IN_FORMATS' modetest-list.out; then
		echo "PASS modetest -M rdrmnull -D pseudo:rdrmnull@0 lists encoders, connectors (1024x768), CRTCs and planes"
	else
		echo "FAIL modetest -M rdrmnull -D pseudo:rdrmnull@0 listing"
		fail=1
	fi
	conn=$(awk '/^Connectors:/ {c = 1; next} c && $3 == "connected" {print $1; exit}' modetest-list.out)
	crtc=$(awk '/^CRTCs:/ {c = 1; next} c && $1 ~ /^[0-9]+$/ {print $1; exit}' modetest-list.out)
	echo "connector $conn, CRTC $crtc"
	if modetest_run set 3 -M rdrmnull -D pseudo:rdrmnull@0 -s "$conn@$crtc:1024x768" &&
	    grep -q 'setting mode 1024x768' modetest-set.out; then
		echo "PASS modetest -s sets 1024x768 with a dumb framebuffer"
	else
		echo "FAIL modetest -s"
		fail=1
	fi
	if modetest_run vsync 5 -M rdrmnull -D pseudo:rdrmnull@0 -s "$conn@$crtc:1024x768" -v &&
	    awk '/freq:/ {f = $2 + 0; if (f > 55 && f < 65) ok++} END {exit !(ok >= 2)}' modetest-vsync.out; then
		echo "PASS modetest -v page-flips with vblank events at about 60 Hz"
	else
		echo "FAIL modetest -v"
		fail=1
	fi
	if modetest_run atomic 3 -M rdrmnull -D pseudo:rdrmnull@0 -a -s "$conn@$crtc:1024x768" -P "$(awk '/^Planes:/ {p = 1; next} p && $1 ~ /^[0-9]+$/ {print $1; exit}' modetest-list.out)@$crtc:1024x768" &&
	    ! grep -qi 'failed' modetest-atomic.out; then
		echo "PASS modetest -a (atomic) sets the mode and the primary plane"
	else
		echo "FAIL modetest -a"
		fail=1
	fi
}

# A refused detach must leave the device working. rdrmnull keeps a
# reference to a GEM object after every file is closed (drm_r3_test hold),
# so detach(9E) refuses; before the core checked everything ahead of any
# teardown, the refused device had lost its mode-setting state and its
# vblank source. modetest must still list the display and page-flip with
# vblank events; drm_r3_test release drops the reference, and the unload
# at the end must succeed.
refused_detach_steps() {
	step ./drm_r3_test hold || fail=1
	refusals=$(grep -c 'detach refused: GEM state in use' /var/adm/messages)
	id=$(modinfo | awk '$6 == "rdrmnull" {print $1}')
	echo "rdrmnull module id: $id"
	if [ -z "$id" ]; then
		echo "FAIL rdrmnull is not loaded"
		fail=1
	elif step modunload -i "$id"; then
		echo "FAIL modunload succeeded while rdrmnull held a GEM object"
		fail=1
	else
		echo "PASS modunload refused while rdrmnull holds a GEM object"
	fi
	# syslogd writes the refusal to the file some time after modunload
	# returns.
	wait_log 'detach refused: GEM state in use' "$refusals"
	step sh -c 'grep "detach refused" /var/adm/messages | tail -2'
	if [ "$(grep -c 'detach refused: GEM state in use' /var/adm/messages)" -gt "$refusals" ]; then
		echo "PASS detach(9E) refused it: GEM state in use"
	else
		echo "FAIL no detach(9E) refusal was logged"
		fail=1
	fi
	step sh -c 'modinfo | grep rdrmnull'
	gemstat "after the refused detach"
	if [ -x "$MODETEST" ]; then
		if modetest_run refused-list 0 -M rdrmnull -D pseudo:rdrmnull@0 &&
		    grep -q '^Connectors:' modetest-refused-list.out &&
		    grep -q '1024x768' modetest-refused-list.out; then
			echo "PASS after the refused detach, modetest lists the display (1024x768)"
		else
			echo "FAIL after the refused detach, modetest listing"
			fail=1
		fi
		if modetest_run refused-vsync 5 -M rdrmnull -D pseudo:rdrmnull@0 -s "$conn@$crtc:1024x768" -v &&
		    awk '/freq:/ {f = $2 + 0; if (f > 55 && f < 65) ok++} END {exit !(ok >= 2)}' modetest-refused-vsync.out; then
			echo "PASS after the refused detach, modetest -v page-flips with vblank events at about 60 Hz"
		else
			echo "FAIL after the refused detach, modetest -v"
			fail=1
		fi
	fi
	step ./drm_r3_test release || fail=1
	gemstat "after the release"
}

CONF=${RDRMNULL_CONF:-rdrmnull.conf}
case $(sed -e 's/#.*//' "$CONF" | tr -d ' \t') in
*'dma-mode="self"'* | *'dma-mode="root-node"'*) RDRMNULL_DMA=1 ;;
*) RDRMNULL_DMA=0 ;;
esac
export RDRMNULL_DMA

fail=0
had_dri=no
[ -d /dev/dri ] && had_dri=yes

[ -f REVISION ] && step cat REVISION
step uname -a
# devfs drops the clone@0 nodes of idle drivers (bridge, llc1, ptm, ...) a
# few seconds after boot, and devfsadm -C at the end recreates them, which
# made the first run on a freshly booted machine report five new /devices
# entries that have nothing to do with rdrmnull. Take both snapshots after
# devfsadm -C.
step devfsadm -C
state before
drm_minor_install || exit 1
step digest -a sha256 rdrmnull.a
# -N fs/specfs: makespecvp, for the file descriptors of dma-bufs, sync
# objects and sync files (illumos-kernel-private-sys), lives in the specfs
# module, which krtld does not search unless the module depends on it.
# -N misc/drm_minor: the DRM device numbers every driver shares.
step ld -ztype=kmod -N fs/specfs -N misc/drm_minor -z allextract rdrmnull.a -o rdrmnull || exit 1
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
step gcc -m64 -Wall -I drm -o drm_r1_test drm_r1_test.c \
    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lkstat -lpthread || exit 1
step gcc -m32 -Wall -DPROBE32 -I drm -o drm_r1_probe32 drm_r1_test.c || exit 1
step gcc -m64 -Wall -I drm -I /usr/include/libdrm -o drm_r3_test drm_r3_test.c \
    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lkstat -lpthread || exit 1
step gcc -m32 -Wall -DPROBE32 -I drm -o drm_r3_probe32 drm_r3_test.c || exit 1

step cp rdrmnull /usr/kernel/drv/amd64/rdrmnull || exit 1
echo
echo "\$ driver configuration $CONF (RDRMNULL_DMA=$RDRMNULL_DMA)"
sed -e '/^#/d' "$CONF"
step cp "$CONF" /usr/kernel/drv/rdrmnull.conf || exit 1
step add_drv -m '* 0666 root sys' rdrmnull || fail=1
if [ $fail -eq 0 ]; then
	step modinfo -c | grep rdrmnull
	step sh -c 'modinfo | grep rdrmnull'
	step sh -c 'find /devices -name "rdrmnull*" | sort'
	step ls -l /dev/dri
	step ls -lL /dev/dri
	# The nodes must have the permissions add_drv -m gave them. A devfs
	# node that the kernel created while the driver was still registered
	# at boot (after a panic during an earlier run) keeps the default 0600
	# root root; set what minor_perm says, and rem_drv -C removes the
	# attribute nodes again at the end.
	for n in $(find /devices -name 'rdrmnull@*:card*' -o -name 'rdrmnull@*:renderD*'); do
		case $(ls -l "$n" | awk '{print $1, $3, $4}') in
		"crw-rw-rw- root sys") ;;
		*) step chmod 0666 "$n"; step chown root:sys "$n" ;;
		esac
	done
	step ls -lL /dev/dri
	rdrmnull_nodes || fail=1
	step sh -c 'tail -3 /var/adm/messages'
	gemstat "after add_drv, before any open"
	step ./drm_r0_test || fail=1
	step ./drm_r0_probe32 || fail=1
	step ./drm_r1_test || fail=1
	step ./drm_r1_probe32 || fail=1
	gemstat "after the R0 and R1 tests"
	step ./drm_r3_test || fail=1
	step ./drm_r3_probe32 || fail=1
	gemstat "after the R3 test"
	modetest_steps
	gemstat "after the tests"
	refused_detach_steps
	step sh -c 'echo "::vmem" | mdb -k | awk "NR == 1 || /rdrmnull|umem_np/"'
	step sh -c 'tail -5 /var/adm/messages'

	# The module must stay loaded while a file is open: modunload has to
	# fail, and succeed once the file is closed (as in run-r0-test.sh).
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
	exec 9>&-
	wait $holder
	echo "holder exited with $?"
	rm -f hold.fifo
	gemstat "before unload"
	if [ -n "$id" ] && step modunload -i "$id"; then
		echo "PASS modunload succeeded after the file was closed"
	else
		echo "FAIL modunload failed after the file was closed"
		fail=1
	fi
	step sh -c 'modinfo | grep rdrmnull || echo "rdrmnull not loaded"'
	step sh -c 'echo "::vmem" | mdb -k | awk "NR == 1 || /rdrmnull/"; echo "(no rdrmnull arenas above: all were destroyed)"'
	# Load once more so that rem_drv unloads a module that has run.
	step ./drm_r0_test hold "$DRM_RENDER" < /dev/null || fail=1
	step sh -c 'echo "::prtconf -d rdrmnull" | mdb -k'
fi

# -C also removes devfs attribute nodes of the driver, so that no
# permission set on its nodes survives the test.
step rem_drv -C rdrmnull || fail=1
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
drm_minor_remove || fail=1
state after
for f in etc devices dri; do
	if cmp -s "state-before.$f" "state-after.$f"; then
		echo "PASS state $f unchanged"
	else
		echo "FAIL state $f changed:"
		diff "state-before.$f" "state-after.$f"
		fail=1
	fi
done
echo
echo "run-r3-test: fail=$fail"
exit $fail
