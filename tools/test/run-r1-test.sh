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
#                    PRIME, with the sync kstat recorded before unload)
#   drm/             the patched libdrm 2.4.109 headers (tools/uapi/generate.sh)
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
	echo "\$ kstat -p rdrmnull:0:sync ($1)"
	kstat -p rdrmnull:0:sync
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

step uname -a
# devfs drops the clone@0 nodes of idle drivers (bridge, llc1, ptm, ...) a
# few seconds after boot, and devfsadm -C at the end recreates them, which
# made the first run on a freshly booted machine report five new /devices
# entries that have nothing to do with rdrmnull. Take both snapshots after
# devfsadm -C.
step devfsadm -C
state before
step digest -a sha256 rdrmnull.a
# -N fs/specfs: makespecvp, for the file descriptors of dma-bufs, sync
# objects and sync files (illumos-kernel-private-sys), lives in the specfs
# module, which krtld does not search unless the module depends on it.
step ld -ztype=kmod -N fs/specfs -z allextract rdrmnull.a -o rdrmnull || exit 1
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
	for n in $(find /devices -name 'rdrmnull@*:card0' -o -name 'rdrmnull@*:renderD128'); do
		case $(ls -l "$n" | awk '{print $1, $3, $4}') in
		"crw-rw-rw- root sys") ;;
		*) step chmod 0666 "$n"; step chown root:sys "$n" ;;
		esac
	done
	step ls -lL /dev/dri
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
	echo "\$ ./drm_r0_test hold /dev/dri/card0 & (open file held on a fifo)"
	rm -f hold.fifo && mkfifo hold.fifo
	./drm_r0_test hold /dev/dri/card0 < hold.fifo &
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
		echo "PASS modunload refused while /dev/dri/card0 is open"
	fi
	exec 9>&-
	wait $holder
	echo "holder exited with $?"
	rm -f hold.fifo
	if [ $R2 = yes ]; then
		# A fence left pending with no file open: the detach must cancel
		# it (ECANCELED) instead of waiting 10 s or refusing.
		step ./drm_r2_test pending || fail=1
	fi
	gemstat "before unload"
	if [ -n "$id" ] && step modunload -i "$id"; then
		echo "PASS modunload succeeded after the file was closed"
	else
		echo "FAIL modunload failed after the file was closed"
		fail=1
	fi
	if [ $R2 = yes ]; then
		if tail -20 /var/adm/messages | grep -q "detach cancelled 1 pending test fences"; then
			echo "PASS the detach cancelled the pending fence"
		else
			echo "FAIL no cancelled fence was reported by the detach"
			fail=1
		fi
	fi
	step sh -c 'modinfo | grep rdrmnull || echo "rdrmnull not loaded"'
	step sh -c 'echo "::vmem" | mdb -k | awk "NR == 1 || /rdrmnull/"; echo "(no rdrmnull arenas above: all were destroyed)"'
	# Load once more so that rem_drv unloads a module that has run.
	step ./drm_r0_test hold /dev/dri/renderD128 < /dev/null || fail=1
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
	if [ $had_dri = no ]; then
		# devfsadm -C only removes links that a link module claims; the
		# directory itself did not exist before the test.
		# Every link to an rdrmnull node (card1 and renderD129 too with
		# rdrmnull-two.conf).
		for l in /dev/dri/*; do
			case $(ls -l "$l" 2>/dev/null) in
			*"-> "*/rdrmnull@*) step rm -f "$l" ;;
			esac
		done
		step rmdir /dev/dri
	fi
fi
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
