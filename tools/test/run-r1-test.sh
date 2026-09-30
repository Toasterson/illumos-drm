#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Links, loads, tests and unloads rdrmnull on an illumos test machine: the
# R0 test (core ioctls, authentication, master, file limits) and the R1
# test (GEM objects, dumb buffers, mmap, FLINK/OPEN, the refault experiment
# and the release from interrupt context), with the gem kstat and the vmem
# arenas recorded before unload. Run as root in a directory that holds:
#
#   rdrmnull.a       from `cargo xtask build rdrmnull`
#   rdrmnull.conf    examples/rdrmnull/rdrmnull.conf
#   drm_r0_test.c    tools/test/drm_r0_test.c
#   drm_r1_test.c    tools/test/drm_r1_test.c
#   drm_r2_test.c    tools/test/drm_r2_test.c (optional: the R2 test runs
#                    when it is present; fences, sync objects, sync files,
#                    PRIME, with the sync kstat recorded before unload,
#                    and a refused detach that must leave the display and
#                    the sync paths working, checked with modetest from
#                    $MODETEST, default /opt/drm-tests/amd64/modetest,
#                    package system/test/libdrm)
#   drm/             the patched libdrm 2.4.109 headers (tools/uapi/generate.sh)
#   drm-test-lib.sh  tools/test/drm-test-lib.sh
#   drm_minor.a      from `cargo xtask build drm_minor`, and drm_minor.mapfile
#                    (modules/drm-minor/exports.mapfile), unless misc/drm_minor
#                    is installed already
#
# rdrmnull's nodes are card<N> and renderD<128+N> for the device number N
# it gets from misc/drm_minor, which other DRM drivers attached at the same
# time (virtio_gpu) share; the script finds them through /dev/dri and hands
# them to the tests in DRM_CARD, DRM_RENDER and DRM_CARD1.
#
# RDRMNULL_CONF names another .conf to install instead of rdrmnull.conf:
# tools/test/rdrmnull-two.conf (two devices, for the R2 cross-device
# checks), or
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
	# The links only: the directory's own line changes whenever a link
	# comes and goes, and it may exist before the test (another driver).
	ls -l /dev/dri 2>&1 | grep -v '^total' > "state-$1.dri"
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
	echo "\$ kstat -p rdrmnull:0:sync ($1)"
	kstat -p rdrmnull:0:sync
}

CONF=${RDRMNULL_CONF:-rdrmnull.conf}
case $(sed -e 's/#.*//' "$CONF" | tr -d ' \t') in
*'dma-mode="self"'* | *'dma-mode="root-node"'*) RDRMNULL_DMA=1 ;;
*) RDRMNULL_DMA=0 ;;
esac
export RDRMNULL_DMA

MODETEST=${MODETEST:-/opt/drm-tests/amd64/modetest}

# A refused detach must leave the device working (R2). drm_r2_test
# pending-hold leaves a 10 s fence pending in the reservation of a buffer
# that rdrmnull keeps a reference to after every file is closed; the detach
# cancels the fence (ECANCELED), then is refused, because the buffer and
# the fence are alive. Every detach check runs before anything is torn
# down, so the display (modetest, from package system/test/libdrm) and the
# sync paths (drm_r2_test after-refusal) must still work; drm_r2_test
# release then drops the buffer.
refused_detach_steps() {
	step ./drm_r2_test pending-hold || fail=1
	refusals=$(grep -c 'detach refused' /var/adm/messages)
	cancels=$(grep -c "detach cancelled 1 pending test fences" /var/adm/messages)
	if step modunload -i "$id"; then
		echo "FAIL modunload succeeded while rdrmnull held a buffer with a pending fence"
		fail=1
	else
		echo "PASS modunload refused while rdrmnull holds a buffer with a pending fence"
	fi
	# syslogd writes the refusal to the file some time after modunload
	# returns.
	wait_log 'detach refused' "$refusals"
	wait_log "detach cancelled 1 pending test fences" "$cancels"
	step sh -c 'grep "detach" /var/adm/messages | tail -3'
	if [ "$(grep -c 'detach refused: GEM state in use' /var/adm/messages)" -gt 0 ] &&
	    [ "$(grep -c 'detach refused' /var/adm/messages)" -gt "$refusals" ]; then
		echo "PASS detach(9E) refused it: GEM state in use"
	else
		echo "FAIL no detach(9E) refusal was logged"
		fail=1
	fi
	if [ "$(grep -c "detach cancelled 1 pending test fences" /var/adm/messages)" -gt "$cancels" ]; then
		echo "PASS the refused detach cancelled the pending fence first"
	else
		echo "FAIL the refused detach did not cancel the pending fence"
		fail=1
	fi
	step sh -c 'modinfo | grep rdrmnull'
	gemstat "after the refused detach"
	if [ ! -x "$MODETEST" ]; then
		echo "FAIL modetest not found at $MODETEST"
		fail=1
	elif step sh -c "$MODETEST -M rdrmnull -D pseudo:rdrmnull@0 -c > modetest-refused.out 2>&1" &&
	    grep -q '^Connectors:' modetest-refused.out && grep -q 'connected' modetest-refused.out &&
	    grep -q '1024x768' modetest-refused.out; then
		echo "PASS after the refused detach, modetest lists the connector and its modes"
	else
		cat modetest-refused.out
		echo "FAIL after the refused detach, modetest listing"
		fail=1
	fi
	step ./drm_r2_test after-refusal || fail=1
	step ./drm_r2_test release || fail=1
}

fail=0
had_dri=no
[ -d /dev/dri ] && had_dri=yes

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
R2=no
if [ -f drm_r2_test.c ]; then
	R2=yes
	step gcc -m64 -Wall -I drm -o drm_r2_test drm_r2_test.c \
	    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lkstat -lpthread -lsocket || exit 1
	step gcc -m32 -Wall -DPROBE32 -I drm -o drm_r2_probe32 drm_r2_test.c || exit 1
fi

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
	if [ $R2 = yes ]; then
		step ./drm_r2_test || fail=1
		step ./drm_r2_probe32 || fail=1
	fi
	gemstat "after the tests"
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
	if [ $R2 = yes ] && [ -n "$id" ]; then
		refused_detach_steps
	fi
	if [ $R2 = yes ]; then
		# A fence left pending with no file open: the detach must cancel
		# it (ECANCELED) instead of waiting 10 s or refusing.
		step ./drm_r2_test pending || fail=1
	fi
	cancels=$(grep -c "detach cancelled 1 pending test fences" /var/adm/messages)
	gemstat "before unload"
	if [ -n "$id" ] && step modunload -i "$id"; then
		echo "PASS modunload succeeded after the file was closed"
	else
		echo "FAIL modunload failed after the file was closed"
		fail=1
	fi
	wait_log "detach cancelled 1 pending test fences" "$cancels"
	if [ $R2 = yes ]; then
		if [ "$(grep -c "detach cancelled 1 pending test fences" /var/adm/messages)" -gt "$cancels" ]; then
			echo "PASS the detach cancelled the pending fence"
		else
			echo "FAIL no cancelled fence was reported by the detach"
			fail=1
		fi
	fi
	step sh -c 'modinfo | grep rdrmnull || echo "rdrmnull not loaded"'
	step sh -c 'echo "::vmem" | mdb -k | awk "NR == 1 || /rdrmnull/"; echo "(no rdrmnull arenas above: all were destroyed)"'
	# Load once more so that rem_drv unloads a module that has run: the
	# link still names the device's node, whose open attaches it again
	# (with the lowest free device number, the one it had if no other
	# device took it meanwhile).
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
echo "run-r1-test: fail=$fail"
exit $fail
