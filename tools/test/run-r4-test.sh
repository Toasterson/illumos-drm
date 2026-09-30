#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Links, loads, tests and unloads virtio_gpu on an illumos guest with a
# virtio-gpu device (QEMU -device virtio-gpu-gl-pci for virgl, or
# virtio-gpu-pci for 2D only): the attach messages, the /dev/dri links,
# drm_r4_test (the virtio-gpu ioctls, virgl through them), libdrm's
# modetest (listing, a mode set with a dumb framebuffer, page flips with
# vblank events, an atomic mode set), and a detach refused while a process
# holds a file and then while only a dma-buf of the device is left, before
# the module unloads. Run as root in a directory that holds:
#
#   virtio_gpu.a      from `cargo xtask build virtio_gpu`
#   drm_r4_test.c     tools/test/drm_r4_test.c
#   drm/              the headers of tools/uapi/generate.sh
#
# The machine needs gcc and libdrm 2.4.109 (x11/library/libdrm); modetest
# comes from $MODETEST, default /opt/drm-tests/amd64/modetest. With
# KEEP=1 the driver stays installed at the end (for the Xorg and Mesa
# steps), otherwise the script undoes everything it did and fails unless
# the driver files in /etc and the /devices tree hash the same afterwards.
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
    /etc/security/device_policy"

state() {
	digest -a sha256 $ETC > "state-$1.etc"
	find /devices -print | grep -v virtio_gpu | sort > "state-$1.devices"
	echo "\$ state $1"
	cat "state-$1.etc"
	echo "/devices entries (without virtio_gpu nodes): $(wc -l < "state-$1.devices")"
}

gemstat() {
	echo
	echo "\$ kstat -p virtio_gpu:0:gem virtio_gpu:0:sync ($1)"
	kstat -p virtio_gpu:0:gem virtio_gpu:0:sync
}

MODETEST=${MODETEST:-/opt/drm-tests/amd64/modetest}

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
	if modetest_run list 0 -M virtio_gpu && grep -q '^Connectors:' modetest-list.out &&
	    grep -q '	connected' modetest-list.out; then
		echo "PASS modetest -M virtio_gpu lists a connected connector, CRTCs and planes"
	else
		echo "FAIL modetest -M virtio_gpu listing"
		fail=1
	fi
	conn=$(awk '/^Connectors:/ {c = 1; next} c && $3 == "connected" {print $1; exit}' modetest-list.out)
	crtc=$(awk '/^CRTCs:/ {c = 1; next} c && $1 ~ /^[0-9]+$/ {print $1; exit}' modetest-list.out)
	plane=$(awk '/^Planes:/ {p = 1; next} p && $1 ~ /^[0-9]+$/ {print $1; exit}' modetest-list.out)
	mode=${MODE:-1024x768}
	echo "connector $conn, CRTC $crtc, primary plane $plane, mode $mode"
	if modetest_run set "${HOLD:-3}" -M virtio_gpu -s "$conn@$crtc:$mode" &&
	    grep -q "setting mode $mode" modetest-set.out && ! grep -qi 'failed' modetest-set.out; then
		echo "PASS modetest -s sets $mode with a dumb framebuffer"
	else
		echo "FAIL modetest -s"
		fail=1
	fi
	# The rate is the mode's: QEMU's EDID prefers 1024x768 at 75 Hz.
	if modetest_run vsync 5 -M virtio_gpu -s "$conn@$crtc:$mode" -v &&
	    awk '/setting mode/ {split($3, a, "-"); r = a[2] + 0}
	        /freq:/ {f = $2 + 0; n++; if (r > 0 && f > r * 0.95 && f < r * 1.05) ok++}
	        END {exit !(ok >= 2)}' modetest-vsync.out; then
		echo "PASS modetest -v page-flips with vblank events at the mode's refresh rate"
	else
		echo "FAIL modetest -v"
		fail=1
	fi
	if modetest_run cursor 3 -M virtio_gpu -s "$conn@$crtc:$mode" -C && ! grep -qi 'failed' modetest-cursor.out; then
		echo "PASS modetest -C moves a cursor over the mode"
	else
		echo "FAIL modetest -C"
		fail=1
	fi
	if modetest_run atomic 3 -M virtio_gpu -a -s "$conn@$crtc:$mode" -P "$plane@$crtc:$mode" &&
	    ! grep -qi 'failed' modetest-atomic.out; then
		echo "PASS modetest -a (atomic) sets the mode and the primary plane"
	else
		echo "FAIL modetest -a"
		fail=1
	fi
}

# The device must refuse to detach while a file is open, and while a
# dma-buf keeps an object alive after every file is closed; it must work
# after each refusal and unload at the end.
refused_detach_steps() {
	id=$(modinfo | awk '$6 == "virtio_gpu" {print $1}')
	echo "virtio_gpu module id: $id"
	rm -f hold.next hold.release
	./drm_r4_test hold > hold.out 2>&1 &
	holder=$!
	sleep 2
	cat hold.out
	if step modunload -i "$id"; then
		echo "FAIL modunload succeeded while a process held a file and an object"
		fail=1
	else
		echo "PASS modunload refused while a process holds a file, a dumb buffer and a dma-buf"
	fi
	touch hold.next
	sleep 1
	cat hold.out
	if step modunload -i "$id"; then
		echo "FAIL modunload succeeded while a dma-buf kept an object"
		fail=1
	else
		echo "PASS modunload refused while only a dma-buf of the device is left"
	fi
	step sh -c 'grep "virtio_gpu.*detach refused" /var/adm/messages | tail -3'
	touch hold.release
	wait $holder
	cat hold.out
	step rm -f hold.next hold.release
	if modetest_run after-refusal 0 -M virtio_gpu && grep -q '	connected' modetest-after-refusal.out; then
		echo "PASS after the refused detach, modetest lists the display"
	else
		echo "FAIL after the refused detach, modetest listing"
		fail=1
	fi
	step ./drm_r4_test >> drm_r4_test-again.out 2>&1
	if grep -q ', 0 failed' drm_r4_test-again.out; then
		echo "PASS after the refused detach, drm_r4_test passes again"
	else
		echo "FAIL after the refused detach, drm_r4_test"
		tail -20 drm_r4_test-again.out
		fail=1
	fi
	gemstat "after the holder exited"
}

fail=0
[ -f REVISION ] && step cat REVISION
step uname -a
step sh -c 'prtconf -v 2>/dev/null | grep -i -B2 -A2 "pci1af4,1050" | head -20'
step devfsadm -C
state before
step digest -a sha256 virtio_gpu.a
# -N misc/virtio: the virtio framework (illumos-kernel-private-virtio-sys);
# -N fs/specfs: makespecvp, for dma-bufs, sync objects and sync files.
step ld -ztype=kmod -N misc/virtio -N fs/specfs -z allextract virtio_gpu.a -o virtio_gpu || exit 1
step digest -a sha256 virtio_gpu
step elfdump -d virtio_gpu
echo
echo "\$ nm -u virtio_gpu: every undefined symbol must resolve in the running kernel"
step modload /kernel/misc/amd64/virtio
nm -u virtio_gpu | awk 'NF == 1 && $1 ~ /^[A-Za-z_]/ {print $1}' | sort -u > undefined.txt
unresolved=0
for s in $(cat undefined.txt); do
	w=$(echo "$s::whatis" | mdb -k 2>&1)
	case $w in
	*" is "*) ;;
	*) echo "unresolved: $s: $w"; unresolved=$((unresolved + 1)) ;;
	esac
done
echo "$(wc -l < undefined.txt) undefined symbols, not exported by the kernel: $unresolved"
[ $unresolved -eq 0 ] || exit 1

step gcc -m64 -Wall -I drm -I /usr/include/libdrm -o drm_r4_test drm_r4_test.c \
    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lpthread || exit 1

msgs=$(wc -l < /var/adm/messages)
step cp virtio_gpu /kernel/drv/amd64/virtio_gpu || exit 1
step add_drv -i '"pci1af4,1050"' -m '* 0666 root sys' virtio_gpu || fail=1
sleep 2
step sh -c "tail -n +$((msgs + 1)) /var/adm/messages | grep -i -E 'virtio|drm'"
step sh -c 'modinfo | grep -E "virtio|drm"'
step sh -c 'find /devices -name "*virtio_gpu*" | sort'
step ls -lL /dev/dri
gemstat "after attach"

step ./drm_r4_test > drm_r4_test.out 2>&1
cat drm_r4_test.out
grep -q ', 0 failed' drm_r4_test.out || fail=1
gemstat "after drm_r4_test"

modetest_steps
refused_detach_steps

if [ "${KEEP:-0}" = 1 ]; then
	echo "KEEP=1: virtio_gpu stays installed"
	[ $fail -eq 0 ] && echo "run-r4-test: PASS" || echo "run-r4-test: FAIL"
	exit $fail
fi
id=$(modinfo | awk '$6 == "virtio_gpu" {print $1}')
if step modunload -i "$id"; then
	echo "PASS virtio_gpu unloads once nothing holds it"
else
	echo "FAIL virtio_gpu does not unload"
	fail=1
fi
step sh -c "tail -n +$((msgs + 1)) /var/adm/messages | grep -i 'virtio_gpu' | tail -5"
step rem_drv virtio_gpu || fail=1
step rm -f /kernel/drv/amd64/virtio_gpu
step devfsadm -C
state after
if cmp -s state-before.etc state-after.etc && cmp -s state-before.devices state-after.devices; then
	echo "PASS /etc driver files and /devices are as before"
else
	echo "FAIL /etc or /devices changed"
	diff state-before.etc state-after.etc
	diff state-before.devices state-after.devices
	fail=1
fi
[ $fail -eq 0 ] && echo "run-r4-test: PASS" || echo "run-r4-test: FAIL"
exit $fail
