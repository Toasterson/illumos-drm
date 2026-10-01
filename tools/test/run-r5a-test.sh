#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The guest test of illumos-drm R5a: the DRM core as the module misc/drm
# with three drivers on its facade at once, rdrmnull and virtio_gpu (Rust,
# through drm-facade) and cdrmnull (C, drivers/cdrmnull).
#
#   A  misc/drm alone: its global symbols are exactly drm.mapfile's, the
#      facade's structure layouts as gcc compiles include/drm match the
#      Rust bindings (tools/facade/facade_layout.c, compared by the caller
#      with `cargo xtask facade-check`), and it loads and unloads.
#   B  The three drivers attached together: the device numbers, the R0 to
#      R2 tests on cdrmnull and on rdrmnull, R3 on rdrmnull, R4 on
#      virtio_gpu, drm_r5a_test (dma-bufs, sync files and cdrmnull's
#      PRIME_SELFTEST across drivers), misc/drm refusing to unload while
#      any driver depends on it, then the drivers unloading and coming back
#      in three orders.
#   C  run-g6-test.sh (rdrmnull twice and virtio_gpu), and run-r1-test.sh
#      for cdrmnull with two devices.
#
# Run as root in a directory that holds drm.a and drm.mapfile, rdrmnull.a,
# rdrmnull.conf, rdrmnull-two.conf, virtio_gpu.a, cdrmnull (the module of
# drivers/cdrmnull/build.sh), cdrmnull.conf, cdrmnull-two.conf,
# cdrmnull_ioctl.h, facade_mismatch.c, drm_facade_ctf.o (optional, from
# modules/drm/ctf/build-ctf.sh), the tests drm_r0_test.c to drm_r4_test.c, drm_g6_test.c
# and drm_r5a_test.c, drm-test-lib.sh, run-r1-test.sh, run-r3-test.sh,
# run-r4-test.sh, run-g6-test.sh, the uAPI headers in drm/ and the facade
# headers in include/drm/ with tools/facade/facade_layout.c. The machine
# needs a virtio-gpu device (pci1af4,1050), gcc, libdrm 2.4.134 with its
# tests (/opt/drm-tests/amd64) and truss(1). Everything it installs is
# removed at the end. Use a disposable VM.
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

ETC="/etc/name_to_major /etc/minor_perm /etc/driver_aliases /etc/driver_classes /etc/devlink.tab
    /etc/security/device_policy"

state() {
	digest -a sha256 $ETC > "r5a-state-$1.etc"
	find /devices -print | grep -v "^/devices/pseudo/clone@0:" | sort > "r5a-state-$1.devices"
	echo "\$ state $1"
	cat "r5a-state-$1.etc"
	echo "/devices entries: $(wc -l < "r5a-state-$1.devices")"
}

modid() {
	modinfo | awk -v m="$1" '$6 == m {print $1}'
}

# The /dev/dri card and render nodes of NODE (rdrmnull@0, cdrmnull@0,
# pci1af4,1100@).
nodes() {
	c=$(dri_link "$1" card) || return 1
	r=$(dri_link "$1" renderD) || return 1
	echo "$c $r"
}

unload() {
	id=$(modid "$1")
	if [ -n "$id" ] && step modunload -i "$id"; then
		pass "$1 unloads"
	else
		bad "$1 does not unload (id '$id')"
	fi
	remove_dri_links "$2"
}

attach() {
	step devfsadm -i "$1"
	step sh -c "modinfo | grep -E '$1| drm '"
}

core_refuses() {
	id=$(modid drm)
	if [ -z "$id" ]; then
		bad "misc/drm is not loaded while $1 is"
	elif step modunload -i "$id"; then
		bad "misc/drm unloaded while $1 depends on it"
	else
		pass "misc/drm refuses to unload while $1 depends on it"
	fi
}

# run_test NAME DRIVER CARD RENDER: one test program on one device.
run_test() {
	out="$1-$2.out"
	step env DRM_DRIVER="$2" DRM_CARD="$3" DRM_RENDER="$4" DRM_INSTANCE=0 ./$1 > "$out" 2>&1
	rc=$?
	grep -E '^FAIL|failures|passed|failed' "$out" | tail -5
	if [ $rc -eq 0 ]; then
		pass "$1 on $2 ($3)"
	else
		cat "$out"
		bad "$1 on $2 ($3)"
	fi
}

all_tests() {
	set -- $(nodes cdrmnull@0) $(nodes rdrmnull@0) $(nodes pci1af4,1100@)
	echo "cdrmnull: $1 $2, rdrmnull: $3 $4, virtio_gpu: $5 $6"
	for t in drm_r0_test drm_r0_probe32 drm_r1_test drm_r1_probe32 drm_r2_test drm_r2_probe32; do
		run_test $t cdrmnull "$1" "$2"
		run_test $t rdrmnull "$3" "$4"
	done
	run_test drm_r3_test rdrmnull "$3" "$4"
	step ./drm_r4_test "$5" "$6" > drm_r4_test.out 2>&1
	tail -3 drm_r4_test.out
	if grep -q ', 0 failed' drm_r4_test.out; then
		pass "drm_r4_test on virtio_gpu: $(grep '^drm_r4_test:' drm_r4_test.out)"
	else
		cat drm_r4_test.out
		bad "drm_r4_test on virtio_gpu"
	fi
	step env DRM_CARD_C="$1" DRM_CARD_R="$3" DRM_CARD_V="$5" ./drm_r5a_test > drm_r5a_test.out 2>&1
	cat drm_r5a_test.out
	if grep -q ', 0 failed' drm_r5a_test.out; then
		pass "drm_r5a_test: $(grep '^drm_r5a_test:' drm_r5a_test.out)"
	else
		bad "drm_r5a_test"
	fi
}

# ctf_check: with the CTF shadow linked into misc/drm, mdb -k knows the
# facade's structures and prints cdrmnull's live drm_device (the first
# field of its soft state's structure).
ctf_check() {
	if [ ! -f drm_facade_ctf.o ]; then
		echo "no drm_facade_ctf.o: misc/drm has no CTF, no mdb check"
		return 0
	fi
	echo "\$ mdb -k: ::print -t struct drm_device, ::sizeof of every facade structure"
	echo "::print -t struct drm_device" | mdb -k > ctf-drm_device.out 2>&1
	head -12 ctf-drm_device.out
	grep -q 'const struct drm_driver \*driver' ctf-drm_device.out &&
	    pass "mdb -k ::print -t struct drm_device" || bad "mdb -k ::print -t struct drm_device"
	n=0
	for t in drm_device drm_minor drm_file drm_driver drm_gem_object \
	    drm_gem_object_funcs dma_fence dma_fence_ops dma_resv dma_buf \
	    drm_facade_version; do
		z=$(echo "::sizeof struct $t" | mdb -k 2>&1)
		echo "$z"
		case $z in *"= 0x"*) n=$((n + 1)) ;; esac
	done
	[ $n -eq 11 ] && pass "mdb -k knows 11 facade structures" || bad "mdb -k knows $n of 11 facade structures"
	slot=$(echo "*cdrmnull_state::softstate 0" | mdb -k)
	dev=$(echo "$slot/K" | mdb -k | awk 'NF >= 2 {v = $NF} END {print v}')
	echo "\$ mdb -k: $dev::print struct drm_device (cdrmnull instance 0)"
	echo "$dev::print struct drm_device unique registered driver_features" | mdb -k > ctf-live.out 2>&1
	echo "$dev::print struct drm_device driver | ::print struct drm_driver name illumos_facade_major illumos_facade_minor" |
	    mdb -k >> ctf-live.out 2>&1
	cat ctf-live.out
	grep -q '"pseudo:cdrmnull@0"' ctf-live.out && grep -q '"cdrmnull"' ctf-live.out &&
	    pass "mdb -k prints cdrmnull's live drm_device" || bad "mdb -k prints cdrmnull's live drm_device"
}

[ -f REVISION ] && step cat REVISION
step uname -a
step sh -c 'pkg list -H x11/library/libdrm system/test/libdrm 2>&1'
step devfsadm -C
state before
had_dri=no
[ -d /dev/dri ] && had_dri=yes

echo
echo "=== A: misc/drm alone"
drm_core_install || exit 1
echo "\$ gcc -S facade_layout.c: the facade's layouts as gcc compiles them"
gcc -m64 -D_KERNEL -std=gnu99 -I include -S -o - facade_layout.c 2> facade_layout.err |
    sed -n 's/^.*@@ //p' | tr -d '"' > facade-gcc.out
cat facade_layout.err
echo "$(grep -c '^size ' facade-gcc.out) sizes, $(grep -c '^offset ' facade-gcc.out) offsets (facade-gcc.out)"
[ -s facade-gcc.out ] && pass "facade_layout.c compiles with gcc" || bad "facade_layout.c"
step sh -c "kstat -p drm:0:sync | grep -E 'fences|syncobjs'"
step sh -c 'echo "drm_facade_version::whatis" | mdb -k'
# A driver built against another facade must refuse to load: the
# facade_mismatch module runs drm_facade_check() in _init, built once
# against a copy of the headers with major version 1 and once with a
# larger struct drm_device.
for v in major size; do
	rm -rf mismatch-$v && mkdir -p mismatch-$v && cp -r include mismatch-$v/
	case $v in
	major) sed 's/^#define	DRM_FACADE_MAJOR	0$/#define	DRM_FACADE_MAJOR	1/' \
	    include/drm/drm_facade_version.h > mismatch-$v/include/drm/drm_facade_version.h ;;
	size) sed 's/uint64_t __core\[16\];/uint64_t __core[17];/' \
	    include/drm/drm_device.h > mismatch-$v/include/drm/drm_device.h ;;
	esac
	cmp -s include/drm/drm_facade_version.h mismatch-$v/include/drm/drm_facade_version.h &&
	    cmp -s include/drm/drm_device.h mismatch-$v/include/drm/drm_device.h && bad "mismatch-$v: no change"
	n=$(grep -c facade_mismatch /var/adm/messages)
	if step gcc -m64 -std=gnu99 -O2 -D_KERNEL -ffreestanding -mcmodel=kernel -mno-red-zone \
	    -fno-builtin -mno-mmx -mno-sse -I mismatch-$v/include -c facade_mismatch.c -o mismatch-$v/fm.o &&
	    step ld -ztype=kmod -N misc/drm -o mismatch-$v/facade_mismatch mismatch-$v/fm.o; then
		if step modload ./mismatch-$v/facade_mismatch; then
			bad "facade_mismatch ($v) loaded"
			id=$(modid facade_mismatch)
			[ -n "$id" ] && modunload -i "$id"
		else
			wait_log facade_mismatch "$n"
			grep facade_mismatch /var/adm/messages | tail -1
			pass "a driver built for another facade ($v) refuses to load"
		fi
	else
		bad "facade_mismatch ($v) does not build"
	fi
done
id=$(modid drm)
if [ -n "$id" ] && step modunload -i "$id"; then
	pass "misc/drm unloads with no driver"
else
	bad "misc/drm does not unload with no driver"
fi

echo
echo "=== Link the Rust drivers, build the tests"
step ld -ztype=kmod -N misc/drm -z allextract rdrmnull.a -o rdrmnull || exit 1
step ld -ztype=kmod -N misc/virtio -N misc/drm -z allextract virtio_gpu.a -o virtio_gpu || exit 1
step modload -p misc/drm
step modload /kernel/misc/amd64/virtio
for m in rdrmnull virtio_gpu cdrmnull; do
	step digest -a sha256 $m
	step sh -c "elfdump -d $m | grep NEEDED"
	nm -u $m | awk 'NF == 1 && $1 ~ /^[A-Za-z_]/ {print $1}' | sort -u > undefined-$m.txt
	unresolved=0
	from_drm=0
	for s in $(cat undefined-$m.txt); do
		w=$(echo "$s::whatis" | mdb -k 2>&1)
		case $w in
		*"in drm's"*) from_drm=$((from_drm + 1)) ;;
		*" is "*) ;;
		*) echo "unresolved: $s"; unresolved=$((unresolved + 1)) ;;
		esac
	done
	echo "$m: $(wc -l < undefined-$m.txt) undefined symbols, $from_drm from misc/drm, $unresolved not in the running kernel"
	[ $unresolved -eq 0 ] && pass "$m: every undefined symbol resolves" || bad "$m: unresolved symbols"
	# A driver must not carry the core: it may compile drm-core's value
	# helpers (modes, dumb layouts, kmem), but no symbol of the stateful
	# modules that only misc/drm builds (feature "kernel").
	core='8drm_core(3gem|5fence|9fence_dev|4resv|5slots|7syncobj|14syncobj_ioctl|5prime|5ioctl|6device|5state|8anon_fd|5minor|7objects|6idmap|7dmaerr|4caps|9sync_file|8uaccess|7wwmutex|6driver)'
	n=$(nm $m | awk -F'|' -v re="$core" '$5 ~ /GLOB/ && $7 !~ /UNDEF/ && $8 ~ re' | wc -l)
	[ "$n" -eq 0 ] && pass "$m defines no core symbol" || bad "$m defines core symbols"
done
for t in r0 r1 r2 r3; do
	step gcc -m64 -Wall -I drm -I /usr/include/libdrm -o drm_${t}_test drm_${t}_test.c \
	    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lkstat -lpthread -lsocket || exit 1
	step gcc -m32 -Wall -DPROBE32 -I drm -o drm_${t}_probe32 drm_${t}_test.c || exit 1
done
step gcc -m64 -Wall -I drm -I /usr/include/libdrm -o drm_r4_test drm_r4_test.c \
    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lpthread || exit 1
step gcc -m64 -Wall -I drm -I . -o drm_r5a_test drm_r5a_test.c \
    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lkstat || exit 1

echo
echo "=== B: cdrmnull, rdrmnull and virtio_gpu together"
step cp cdrmnull /usr/kernel/drv/amd64/cdrmnull || exit 1
step cp cdrmnull.conf /usr/kernel/drv/cdrmnull.conf || exit 1
step add_drv -m '* 0666 root sys' cdrmnull || exit 1
step cp rdrmnull /usr/kernel/drv/amd64/rdrmnull || exit 1
step cp rdrmnull.conf /usr/kernel/drv/rdrmnull.conf || exit 1
step add_drv -m '* 0666 root sys' rdrmnull || exit 1
step cp virtio_gpu /kernel/drv/amd64/virtio_gpu || exit 1
step add_drv -i '"pci1af4,1050"' -m '* 0666 root sys' virtio_gpu || exit 1
step sh -c 'modinfo | grep -E "cdrmnull|rdrmnull|virtio_gpu| drm "'
step ls -l /dev/dri
core_refuses "three drivers"
ctf_check
all_tests

for order in "cdrmnull rdrmnull virtio_gpu" "virtio_gpu cdrmnull rdrmnull" "rdrmnull virtio_gpu cdrmnull"; do
	echo
	echo "=== B: unload in the order $order, then attach again"
	set -- $order
	for d in "$@"; do
		case $d in
		virtio_gpu) unload virtio_gpu pci1af4,1100@ ;;
		*) unload $d $d@ ;;
		esac
		left=$(modinfo | awk '$6 ~ /^(cdrmnull|rdrmnull|virtio_gpu)$/' | wc -l)
		[ "$left" -gt 0 ] && core_refuses "$left driver(s)"
	done
	id=$(modid drm)
	if [ -n "$id" ] && step modunload -i "$id"; then
		pass "misc/drm unloads after the drivers ($order)"
	else
		bad "misc/drm does not unload after the drivers ($order)"
	fi
	for d in "$@"; do
		attach $d
	done
	step ls -l /dev/dri
	set -- $(nodes cdrmnull@0) $(nodes rdrmnull@0) $(nodes pci1af4,1100@)
	run_test drm_r1_test cdrmnull "$1" "$2"
	step env DRM_CARD_C="$1" DRM_CARD_R="$3" DRM_CARD_V="$5" ./drm_r5a_test > drm_r5a_test.out 2>&1
	grep -E '^FAIL|^drm_r5a_test:' drm_r5a_test.out
	grep -q ', 0 failed' drm_r5a_test.out && pass "drm_r5a_test after reattach" || bad "drm_r5a_test after reattach"
done

echo
echo "=== B: remove the drivers"
unload cdrmnull cdrmnull@
unload rdrmnull rdrmnull@
unload virtio_gpu pci1af4,1100@
step rem_drv -C cdrmnull || fail=1
step rem_drv -C rdrmnull || fail=1
step rem_drv virtio_gpu || fail=1
step rm -f /usr/kernel/drv/amd64/cdrmnull /usr/kernel/drv/cdrmnull.conf /usr/kernel/drv/amd64/rdrmnull \
    /usr/kernel/drv/rdrmnull.conf /kernel/drv/amd64/virtio_gpu
step devfsadm -C
remove_dri_links cdrmnull@
remove_dri_links rdrmnull@
remove_dri_links pci1af4,1100@

echo
echo "=== C: run-g6-test.sh and run-r1-test.sh for cdrmnull"
step ./run-g6-test.sh > run-g6-test.out 2>&1
grep -E '^FAIL|run-g6-test:' run-g6-test.out
grep -q 'run-g6-test: PASS' run-g6-test.out && pass "run-g6-test.sh" || bad "run-g6-test.sh"
step env DRM_DRIVER=cdrmnull RDRMNULL_CONF=cdrmnull-two.conf ./run-r1-test.sh > run-r1-cdrmnull.out 2>&1
grep -E '^FAIL|failures$|passed,|run-r1-test:' run-r1-cdrmnull.out
grep -q 'run-r1-test: fail=0' run-r1-cdrmnull.out && pass "run-r1-test.sh for cdrmnull (two devices)" ||
    bad "run-r1-test.sh for cdrmnull"

echo
echo "=== Clean up"
DRM_CORE_OURS=yes
drm_core_remove || fail=1
[ $had_dri = no ] && [ -d /dev/dri ] && step rmdir /dev/dri
step devfsadm -C
state after
for f in etc devices; do
	if cmp -s "r5a-state-before.$f" "r5a-state-after.$f"; then
		pass "state $f unchanged"
	else
		diff "r5a-state-before.$f" "r5a-state-after.$f"
		bad "state $f changed"
	fi
done
echo "checks: $(grep -c '^PASS' "$0.log" 2>/dev/null || echo see output)"
[ $fail -eq 0 ] && echo "run-r5a-test: PASS" || echo "run-r5a-test: FAIL"
exit $fail
