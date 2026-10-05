#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# G6: one DRM device numbering for every driver of the core, and Linux's
# ioctl argument size rule, on an illumos guest with a virtio-gpu device
# (QEMU -device virtio-gpu-gl-pci). Two rdrmnull devices and virtio_gpu
# are attached together in both orders; every device must get its own
# number N (the lowest free one), card<N> and renderD<128+N> under
# /dev/dri, libdrm must list them all (drmdevice, getsundev), the tests of
# R0 to R4 and drm_g6_test must pass on each, the drivers must unload in
# every order without a panic, misc/drm_minor must refuse to unload while
# a driver depends on it, and numbers must be reused after a reload.
# Then the per-milestone scripts run in the mixed configuration:
# run-r4-test.sh (KEEP=1), run-r1-test.sh with two rdrmnull devices and
# run-r3-test.sh while virtio_gpu stays attached.
#
# Run as root in a directory that holds rdrmnull.a, virtio_gpu.a and
# drm_minor.a (`cargo xtask build ...`), drm_minor.mapfile
# (modules/drm-minor/exports.mapfile), rdrmnull.conf,
# tools/test/rdrmnull-two.conf, the tests drm_r0_test.c to drm_r4_test.c
# and drm_g6_test.c, drm-test-lib.sh, run-r1-test.sh, run-r3-test.sh,
# run-r4-test.sh and the headers of tools/uapi/generate.sh in drm/. The
# machine needs gcc, libdrm 2.4.134 with its tests (/opt/drm-tests/amd64:
# drmdevice, getsundev, modetest; gfx-drm's system/test/libdrm) and
# truss(1). Everything the script installs is removed at the end, and it
# fails unless the driver files in /etc and the /devices tree are as
# before. Use a disposable VM: kernel modules under test can panic it.
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
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=1; }

T=/opt/drm-tests/amd64
# /etc/path_to_inst is left out, as in run-r4-test.sh: rem_drv keeps the
# instance number of the virtio-gpu PCI function (its diff is shown).
ETC="/etc/name_to_major /etc/minor_perm /etc/driver_aliases /etc/driver_classes /etc/devlink.tab
    /etc/security/device_policy"

state() {
	digest -a sha256 $ETC > "g6-state-$1.etc"
	find /devices -print | sort > "g6-state-$1.devices"
	cp /etc/path_to_inst "g6-state-$1.path_to_inst"
	echo "\$ state $1"
	cat "g6-state-$1.etc"
	echo "/devices entries: $(wc -l < "g6-state-$1.devices")"
}

modid() {
	modinfo | awk -v m="$1" '$6 == m {print $1}'
}

# minor_of NODE: the minor number of a device node.
minor_of() {
	ls -lL "$1" | awk '{print $6}'
}

# expect DEVICE N: DEVICE (rdrmnull@0, pci1af4,1100@) has card<N> and
# renderD<128+N>, whose minor numbers are N and 128 + N.
expect() {
	c=$(dri_link "$1" card)
	r=$(dri_link "$1" renderD)
	if [ "$c" = "/dev/dri/card$2" ] && [ "$r" = "/dev/dri/renderD$((128 + $2))" ] &&
	    [ "$(minor_of "$c")" = "$2" ] && [ "$(minor_of "$r")" = "$((128 + $2))" ]; then
		pass "$1 has card$2 and renderD$((128 + $2)) (minors $2 and $((128 + $2)))"
	else
		bad "$1: expected card$2 and renderD$((128 + $2)), found '$c' '$r'"
	fi
}

# listing COUNT: drmdevice and getsundev see COUNT devices, and getsundev
# passes.
listing() {
	step ls -l /dev/dri
	step ls -lL /dev/dri
	"$T/drmdevice" > drmdevice.out 2>&1
	rc=$?
	cat drmdevice.out
	echo "[drmdevice exit $rc]"
	if [ $rc -eq 0 ] && grep -q "Devices reported $1 " drmdevice.out; then
		pass "drmdevice reports $1 devices"
	else
		bad "drmdevice does not report $1 devices"
	fi
	"$T/getsundev" > getsundev.out 2>&1
	rc=$?
	grep -v '^PASS' getsundev.out
	echo "[getsundev exit $rc]"
	if [ $rc -eq 0 ] && grep -q "drmGetDevices2 counts $1 devices, returns $1" getsundev.out &&
	    grep -q ', 0 failed' getsundev.out; then
		pass "getsundev: $(grep 'passed' getsundev.out | tail -1)"
	else
		bad "getsundev with $1 devices"
	fi
}

# g6 CARD RENDER: drm_g6_test on one device.
g6() {
	step ./drm_g6_test "$1" "$2" > "g6-$(basename "$1").out" 2>&1
	cat "g6-$(basename "$1").out"
	if grep -q ', 0 failed' "g6-$(basename "$1").out"; then
		pass "drm_g6_test on $1: $(grep '^drm_g6_test:' "g6-$(basename "$1").out")"
	else
		bad "drm_g6_test on $1"
	fi
}

# truss_check RENDER: libdrm 2.4.134 calls each grown command once, at its
# own size, and never retries at the 2.4.109 size.
truss_check() {
	truss -t ioctl ./drm_g6_test libdrm "$1" > truss.out 2>&1
	rc=$?
	grep -E "_IOWRN\('d', (19[1-5]|202)," truss.out
	echo "[truss exit $rc]"
	ok=1
	for c in "193 16 24" "194 16 24" "195 32 40" "202 40 48"; do
		set -- $c
		new=$(grep -c "_IOWRN('d', $1, $3), .*= 0" truss.out)
		old=$(grep -c "_IOWRN('d', $1, $2)," truss.out)
		[ "$new" -eq 1 ] && [ "$old" -eq 0 ] || ok=0
		echo "command $1: $new call(s) at $3 bytes, $old at $2 bytes"
	done
	# (stdio's isatty() is a TCGETA on fd 1, ENOTTY by design.)
	if [ $ok -eq 1 ] && ! grep "('d'," truss.out | grep -q ENOTTY && [ $rc -eq 0 ]; then
		pass "libdrm 2.4.134 sends the grown commands once at their new size, no ENOTTY, no retry"
	else
		bad "libdrm's grown commands under truss"
	fi
}

# rtests CARD RENDER INSTANCE CARD1: the R0 to R3 tests on one rdrmnull
# device (CARD1: the other rdrmnull device, for R2's cross-device checks).
rtests() {
	echo
	echo "=== R0 to R3 on $1 $2 (rdrmnull@$3; second device $4)"
	export DRM_CARD="$1" DRM_RENDER="$2" DRM_INSTANCE="$3" DRM_CARD1="$4"
	for t in drm_r0_test drm_r0_probe32 drm_r1_test drm_r1_probe32 drm_r2_test drm_r2_probe32 \
	    drm_r3_test drm_r3_probe32; do
		step ./$t > "$t-$3.out" 2>&1
		rc=$?
		grep -E '^FAIL|failures|passed|failed' "$t-$3.out" | tail -5
		if [ $rc -eq 0 ]; then
			pass "$t on rdrmnull@$3 ($1)"
		else
			cat "$t-$3.out"
			bad "$t on rdrmnull@$3 ($1)"
		fi
	done
	unset DRM_CARD DRM_RENDER DRM_INSTANCE DRM_CARD1
}

# r4test CARD RENDER: drm_r4_test on virtio_gpu.
r4test() {
	step ./drm_r4_test "$1" "$2" > "drm_r4_test-$(basename "$1").out" 2>&1
	tail -3 "drm_r4_test-$(basename "$1").out"
	if grep -q ', 0 failed' "drm_r4_test-$(basename "$1").out"; then
		pass "drm_r4_test on virtio_gpu ($1): $(grep '^drm_r4_test:' "drm_r4_test-$(basename "$1").out")"
	else
		cat "drm_r4_test-$(basename "$1").out"
		bad "drm_r4_test on virtio_gpu ($1)"
	fi
}

# unload DRIVER NODE: unloads DRIVER and removes the /dev/dri links to its
# device nodes (NODE: rdrmnull@ or pci1af4,1100@). The links would
# otherwise stay, and any stat(2) of one (ls -L, libdrm's device listing)
# would attach the driver again through devfs. devfsadm -C is not used
# while a driver stays installed, because it attaches every driver.
unload() {
	id=$(modid "$1")
	if [ -n "$id" ] && step modunload -i "$id"; then
		pass "$1 unloads"
	else
		bad "$1 does not unload (id '$id')"
	fi
	remove_dri_links "$2"
}

minor_refuses() {
	id=$(modid drm_minor)
	if [ -z "$id" ]; then
		bad "misc/drm_minor is not loaded while $1 is"
	elif step modunload -i "$id"; then
		bad "misc/drm_minor unloaded while $1 depends on it"
	else
		pass "misc/drm_minor refuses to unload while $1 depends on it"
	fi
}

attach() {
	step devfsadm -i "$1"
	step sh -c "modinfo | grep -E '$1|drm_minor'"
}

[ -f REVISION ] && step cat REVISION
step uname -a
step sh -c 'pkg list -H x11/library/libdrm system/test/libdrm 2>&1'
step devfsadm -C
state before
had_dri=no
[ -d /dev/dri ] && had_dri=yes

echo
echo "=== Link and check the modules"
drm_minor_install || exit 1
step ld -ztype=kmod -N fs/specfs -N misc/drm_minor -z allextract rdrmnull.a -o rdrmnull || exit 1
step ld -ztype=kmod -N misc/virtio -N fs/specfs -N misc/drm_minor -z allextract virtio_gpu.a -o virtio_gpu || exit 1
step modload /kernel/misc/amd64/virtio
for m in rdrmnull virtio_gpu; do
	step digest -a sha256 $m.a $m
	step sh -c "elfdump -d $m | grep NEEDED"
	nm -u $m | awk 'NF == 1 && $1 ~ /^[A-Za-z_]/ {print $1}' | sort -u > undefined-$m.txt
	unresolved=0
	for s in $(cat undefined-$m.txt); do
		case $(echo "$s::whatis" | mdb -k 2>&1) in
		*" is "*) ;;
		*) echo "unresolved: $s"; unresolved=$((unresolved + 1)) ;;
		esac
	done
	echo "$m: $(wc -l < undefined-$m.txt) undefined symbols, $unresolved not in the running kernel"
	[ $unresolved -eq 0 ] || exit 1
	for s in drm_minor_alloc drm_minor_free; do
		w=$(echo "$s::whatis" | mdb -k 2>&1)
		echo "$w"
		case $w in
		*"in drm_minor's"*) ;;
		*) bad "$s does not resolve from drm_minor" ;;
		esac
	done
done

echo
echo "=== Build the tests"
for t in r0 r1 r2 r3; do
	step gcc -m64 -Wall -I drm -I /usr/include/libdrm -o drm_${t}_test drm_${t}_test.c \
	    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lkstat -lpthread -lsocket || exit 1
	step gcc -m32 -Wall -DPROBE32 -I drm -o drm_${t}_probe32 drm_${t}_test.c || exit 1
done
step gcc -m64 -Wall -I drm -I /usr/include/libdrm -o drm_r4_test drm_r4_test.c \
    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lpthread || exit 1
step gcc -m64 -Wall -I drm -o drm_g6_test drm_g6_test.c \
    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm || exit 1

echo
echo "=== A: virtio_gpu first, then two rdrmnull devices"
step cp virtio_gpu /kernel/drv/amd64/virtio_gpu || exit 1
step add_drv -i '"pci1af4,1050"' -m '* 0666 root sys' virtio_gpu || exit 1
expect pci1af4,1100@ 0
step cp rdrmnull /usr/kernel/drv/amd64/rdrmnull || exit 1
step cp rdrmnull-two.conf /usr/kernel/drv/rdrmnull.conf || exit 1
step add_drv -m '* 0666 root sys' rdrmnull || exit 1
step devfsadm -i rdrmnull
expect pci1af4,1100@ 0
expect rdrmnull@0 1
expect rdrmnull@1 2
minor_refuses "rdrmnull and virtio_gpu"
listing 3
for n in 0 1 2; do
	g6 /dev/dri/card$n /dev/dri/renderD$((128 + n))
done
truss_check /dev/dri/renderD129
truss_check /dev/dri/renderD128
rtests /dev/dri/card1 /dev/dri/renderD129 0 /dev/dri/card2
rtests /dev/dri/card2 /dev/dri/renderD130 1 /dev/dri/card1
r4test /dev/dri/card0 /dev/dri/renderD128
for d in "rdrmnull -D pseudo:rdrmnull@0" "rdrmnull -D pseudo:rdrmnull@1" "virtio_gpu"; do
	step sh -c "$T/modetest -M $d -c > modetest-a.out 2>&1"
	if grep -q '^Connectors:' modetest-a.out && grep -q '	connected' modetest-a.out; then
		pass "modetest -M $d lists a connected connector"
	else
		cat modetest-a.out
		bad "modetest -M $d"
	fi
done
step sh -c 'kstat -p rdrmnull:0:gem rdrmnull:1:gem virtio_gpu:0:gem | grep -E "objects|handles"'

echo
echo "=== A: rdrmnull unloads first; virtio_gpu keeps card0"
unload rdrmnull rdrmnull@
listing 1
g6 /dev/dri/card0 /dev/dri/renderD128
minor_refuses virtio_gpu
echo
echo "=== A: rdrmnull again: its numbers come back"
attach rdrmnull
expect rdrmnull@0 1
expect rdrmnull@1 2
listing 3
echo
echo "=== A: virtio_gpu unloads; rdrmnull keeps card1 and card2"
unload virtio_gpu pci1af4,1100@
expect rdrmnull@0 1
expect rdrmnull@1 2
listing 2
g6 /dev/dri/card1 /dev/dri/renderD129
g6 /dev/dri/card2 /dev/dri/renderD130
echo
echo "=== A: virtio_gpu again: the lowest free number, 0"
attach virtio_gpu
expect pci1af4,1100@ 0
listing 3
r4test /dev/dri/card0 /dev/dri/renderD128
unload rdrmnull rdrmnull@
unload virtio_gpu pci1af4,1100@
id=$(modid drm_minor)
if [ -n "$id" ] && step modunload -i "$id"; then
	pass "misc/drm_minor unloads once no driver depends on it"
else
	bad "misc/drm_minor does not unload with no driver loaded"
fi

echo
echo "=== B: two rdrmnull devices first (misc/drm_minor loads with them), then virtio_gpu"
attach rdrmnull
if [ -n "$(modid drm_minor)" ]; then
	pass "loading rdrmnull loaded misc/drm_minor (its -N dependency)"
else
	bad "misc/drm_minor was not loaded with rdrmnull"
fi
expect rdrmnull@0 0
expect rdrmnull@1 1
attach virtio_gpu
expect pci1af4,1100@ 2
expect rdrmnull@0 0
listing 3
for n in 0 1 2; do
	g6 /dev/dri/card$n /dev/dri/renderD$((128 + n))
done
truss_check /dev/dri/renderD130
rtests /dev/dri/card0 /dev/dri/renderD128 0 /dev/dri/card1
r4test /dev/dri/card2 /dev/dri/renderD130
minor_refuses "rdrmnull and virtio_gpu"
echo
echo "=== B: virtio_gpu unloads first, then rdrmnull, then misc/drm_minor"
unload virtio_gpu pci1af4,1100@
minor_refuses rdrmnull
unload rdrmnull rdrmnull@
id=$(modid drm_minor)
if [ -n "$id" ] && step modunload -i "$id"; then
	pass "misc/drm_minor unloads after both drivers"
else
	bad "misc/drm_minor does not unload after both drivers"
fi

echo
echo "=== Remove the drivers of A and B"
step rem_drv -C rdrmnull || fail=1
step rem_drv virtio_gpu || fail=1
step rm -f /usr/kernel/drv/amd64/rdrmnull /usr/kernel/drv/rdrmnull.conf /kernel/drv/amd64/virtio_gpu
step devfsadm -C
remove_dri_links rdrmnull@
remove_dri_links pci1af4,1100@
step sh -c 'modinfo | grep -E "rdrmnull|virtio_gpu|drm_minor" || echo "none loaded"'
step sh -c 'dmesg 2>/dev/null | grep -i drm_minor | tail -5; grep -i "drm_minor" /var/adm/messages | tail -5'

echo
echo "=== C: the milestone scripts with virtio_gpu and rdrmnull attached together"
step env KEEP=1 ./run-r4-test.sh > run-r4-test.out 2>&1
tail -3 run-r4-test.out
grep -q 'run-r4-test: PASS' run-r4-test.out && pass "run-r4-test.sh (KEEP=1)" || bad "run-r4-test.sh"
step ls -l /dev/dri
step env RDRMNULL_CONF=rdrmnull-two.conf ./run-r1-test.sh > run-r1-test.out 2>&1
grep -E '^(PASS|FAIL) (state|modunload)|^rdrmnull nodes|run-r1-test:' run-r1-test.out
grep -q 'run-r1-test: fail=0' run-r1-test.out && pass "run-r1-test.sh with two rdrmnull devices beside virtio_gpu" ||
    bad "run-r1-test.sh"
step ./run-r3-test.sh > run-r3-test.out 2>&1
grep -E '^(PASS|FAIL) (state|modunload)|^rdrmnull nodes|run-r3-test:' run-r3-test.out
grep -q 'run-r3-test: fail=0' run-r3-test.out && pass "run-r3-test.sh beside virtio_gpu" || bad "run-r3-test.sh"
unload virtio_gpu pci1af4,1100@
step rem_drv virtio_gpu || fail=1
step rm -f /kernel/drv/amd64/virtio_gpu
step devfsadm -C
remove_dri_links pci1af4,1100@

echo
echo "=== Clean up"
DRM_MINOR_OURS=yes
drm_minor_remove || fail=1
[ $had_dri = no ] && [ -d /dev/dri ] && step rmdir /dev/dri
# devfs drops the clone@0 nodes of idle drivers a while after devfsadm
# made them; make them again before both snapshots (as run-r1-test.sh).
step devfsadm -C
state after
step sh -c 'diff g6-state-before.path_to_inst g6-state-after.path_to_inst'
for f in etc devices; do
	if cmp -s "g6-state-before.$f" "g6-state-after.$f"; then
		pass "state $f unchanged"
	else
		diff "g6-state-before.$f" "g6-state-after.$f"
		bad "state $f changed"
	fi
done
[ $fail -eq 0 ] && echo "run-g6-test: PASS" || echo "run-g6-test: FAIL"
exit $fail
