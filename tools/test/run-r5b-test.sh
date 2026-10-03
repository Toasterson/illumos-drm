#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The guest test of illumos-drm R5b: the public-layout base objects of
# facade 1.0.
#
#   A  run-r5a-test.sh: the R0 to R5a suites, G6 and R1 (cdrmnull with two
#      devices) on the three drivers on misc/drm_core, including the
#      facade mismatch probes (another major version, a larger
#      struct drm_device, a smaller private tail). SKIP_R5A=1 skips it.
#   B  cdrmnull alone: drm_r5b_test (fence ops, the driver's fences in the
#      core's sync objects, arrays, chains and sync files, handle walks,
#      the live GEM reference count, drm_printer); the messages of
#      PRINT_SELFTEST in the system log with drm_debug 0 and with
#      DRM_UT_DRIVER|DRM_UT_KMS set through mdb -kw; no timeline fence
#      released in interrupt context; the managed resources released in
#      reverse order when the device goes; cdrmnull's _fini.
#   C  run-g8-test.sh kernel (G8 on the three drivers). SKIP_G8=1 skips it.
#
# Run as root in a directory with everything run-r5a-test.sh and
# run-g8-test.sh want (see their headers) and drm_r5b_test.c. Everything it
# installs is removed again. Use a disposable VM.
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

modid() {
	modinfo | awk -v m="$1" '$6 == m {print $1}'
}

# mdb_dec SYMBOL: a 64-bit kernel variable in decimal.
mdb_dec() {
	echo "$1/E" | mdb -k 2>/dev/null | awk 'NF >= 2 {v = $NF} END {print v}'
}

[ -f REVISION ] && step cat REVISION
step uname -a

if [ "${SKIP_R5A:-0}" != 1 ]; then
	echo
	echo "=== A: run-r5a-test.sh"
	step ./run-r5a-test.sh > run-r5a-test.out 2>&1
	grep -E '^FAIL|^checks passed|run-r5a-test:' run-r5a-test.out
	if grep -q 'run-r5a-test: PASS' run-r5a-test.out; then
		pass "run-r5a-test.sh ($(grep '^checks passed' run-r5a-test.out))"
	else
		bad "run-r5a-test.sh"
	fi
	# Every detach of cdrmnull in that run released its managed
	# resources in reverse order.
	good=$(grep -c 'cdrmnull[0-9]*: drmm: released 2,4,3,1 (reverse order)' /var/adm/messages)
	wrong=$(grep -c 'cdrmnull[0-9]*: drmm: .*WRONG ORDER' /var/adm/messages)
	if [ "$good" -gt 0 ] && [ "$wrong" -eq 0 ]; then
		pass "managed resources released in reverse order at $good detaches of cdrmnull"
	else
		bad "managed resources: $good detaches in order, $wrong in the wrong order"
	fi
fi

echo
echo "=== B: cdrmnull alone"
step devfsadm -C
drm_core_install || exit 1
step gcc -m64 -Wall -I drm -I . -o drm_r5b_test drm_r5b_test.c \
    -L/usr/lib/xorg/amd64 -R/usr/lib/xorg/amd64 -ldrm -lkstat || exit 1
step cp cdrmnull /usr/kernel/drv/amd64/cdrmnull || exit 1
step cp cdrmnull.conf /usr/kernel/drv/cdrmnull.conf || exit 1
step add_drv -m '* 0666 root sys' cdrmnull || exit 1
step sh -c 'modinfo | grep -E "cdrmnull| drm_core "'
card=$(dri_link cdrmnull@0 card)
echo "cdrmnull card: $card"
if [ -z "$card" ]; then
	bad "no card node of cdrmnull"
else
	step sh -c 'kstat -p drm:0:sync | grep -E "fences|deferred|drains"'
	intr0=$(mdb_dec cdrmnull_tl_released_intr)
	rel0=$(mdb_dec cdrmnull_tl_released)
	token=$(printf '%x' "$(perl -e 'print time' 2>/dev/null || date +%s)")
	step env DRM_CARD="$card" ./drm_r5b_test "$token" > drm_r5b_test.out 2>&1
	cat drm_r5b_test.out
	if grep -q ', 0 failed' drm_r5b_test.out; then
		pass "drm_r5b_test: $(grep '^drm_r5b_test:' drm_r5b_test.out)"
	else
		bad "drm_r5b_test"
	fi
	step sh -c 'kstat -p drm:0:sync | grep -E "fences|deferred|drains"'

	# The messages of PRINT_SELFTEST with drm_debug 0: no debug message.
	wait_log "r5b-print $token long" 0
	count() { grep -c "r5b-print $1 $2\$" /var/adm/messages; }
	expect() {
		n=$(count "$1" "$2")
		if [ "$n" -eq "$3" ]; then
			pass "log ($4): \"$2\" $n time(s)"
		else
			grep "r5b-print $1 $2" /var/adm/messages
			bad "log ($4): \"$2\" $n time(s), expected $3"
		fi
	}
	echo "\$ grep 'r5b-print $token' /var/adm/messages"
	grep "r5b-print $token" /var/adm/messages | cut -c1-200
	for k in info notice warn err DRM_INFO DRM_ERROR printer-info printer-err once; do
		expect "$token" "$k" 1 "drm_debug 0"
	done
	for k in dbg dbg-kms debug-driver printer-dbg; do
		expect "$token" "$k" 0 "drm_debug 0"
	done
	grep "r5b-print $token err\$" /var/adm/messages | grep -q '\*ERROR\*' &&
	    pass "drm_err carries *ERROR*" || bad "drm_err without *ERROR*"
	grep "r5b-print $token printer-err\$" /var/adm/messages | grep -q '\[drm\] \*ERROR\* r5b ' &&
	    pass "drm_err_printer: [drm] *ERROR* and its prefix" || bad "drm_err_printer's prefix"
	long=$(grep "r5b-print $token long" /var/adm/messages | tail -1)
	case $long in
	*'xxx...') pass "a long message is cut and ends in ..." ;;
	*) echo "$long" | cut -c1-120; bad "a long message is not cut with ..." ;;
	esac

	# drm_debug: DRM_UT_DRIVER (1) and DRM_UT_KMS (2).
	step sh -c "echo 'drm_debug/W 0x6' | mdb -kw"
	token2=$(printf '%x' $(( $(perl -e 'print time' 2>/dev/null || date +%s) + 1 )))
	step env DRM_CARD="$card" ./drm_r5b_test "$token2" print > drm_r5b_print.out 2>&1
	grep -E '^FAIL|^drm_r5b_test:' drm_r5b_print.out
	grep -q ', 0 failed' drm_r5b_print.out && pass "drm_r5b_test print with drm_debug 0x6" ||
	    bad "drm_r5b_test print with drm_debug 0x6"
	step sh -c "echo 'drm_debug/W 0' | mdb -kw"
	wait_log "r5b-print $token2 long" 0
	echo "\$ grep 'r5b-print $token2' /var/adm/messages"
	grep "r5b-print $token2" /var/adm/messages | grep -v ' long ' | cut -c1-200
	for k in dbg dbg-kms debug-driver printer-dbg info; do
		expect "$token2" "$k" 1 "drm_debug 0x6"
	done
	grep "r5b-print $token2 printer-dbg\$" /var/adm/messages | grep -q '\[drm:dbg\] r5b ' &&
	    pass "drm_dbg_printer: [drm:dbg] and its prefix" || bad "drm_dbg_printer's prefix"

	# The timeline: releases ran, none in interrupt context.
	intr1=$(mdb_dec cdrmnull_tl_released_intr)
	rel1=$(mdb_dec cdrmnull_tl_released)
	echo "timeline fences released: $rel0 -> $rel1, in interrupt context: $intr0 -> $intr1"
	if [ -n "$rel1" ] && [ "$rel1" -gt "${rel0:-0}" ] && [ "$intr1" = "$intr0" ]; then
		pass "timeline fences released in thread context only ($((rel1 - rel0)) releases)"
	else
		bad "timeline fence releases: $rel0 -> $rel1, interrupt context $intr0 -> $intr1"
	fi
fi

# The device goes: its managed resources are released in reverse order.
n0=$(grep -c 'cdrmnull0: drmm: released 2,4,3,1 (reverse order)' /var/adm/messages)
id=$(modid cdrmnull)
if [ -n "$id" ] && step modunload -i "$id"; then
	pass "cdrmnull unloads (its _fini waited for queued fence releases)"
else
	bad "cdrmnull does not unload"
fi
wait_log 'cdrmnull0: drmm: released 2,4,3,1 (reverse order)' "$n0"
if [ "$(grep -c 'cdrmnull0: drmm: released 2,4,3,1 (reverse order)' /var/adm/messages)" -gt "$n0" ]; then
	pass "detach released the managed resources: 2 early, then 4, 3, 1"
else
	grep 'drmm:' /var/adm/messages | tail -3
	bad "no reverse-order release of the managed resources at detach"
fi
step rem_drv -C cdrmnull || fail=1
step rm -f /usr/kernel/drv/amd64/cdrmnull /usr/kernel/drv/cdrmnull.conf
step devfsadm -C
remove_dri_links cdrmnull@
drm_core_remove || fail=1

if [ "${SKIP_G8:-0}" != 1 ]; then
	echo
	echo "=== C: run-g8-test.sh kernel"
	step ./run-g8-test.sh kernel > run-g8-test.out 2>&1
	grep -E '^FAIL|^checks passed|run-g8-test' run-g8-test.out | tail -5
	if grep -q 'run-g8-test kernel: PASS' run-g8-test.out; then
		pass "run-g8-test.sh kernel ($(grep '^checks passed' run-g8-test.out))"
	else
		bad "run-g8-test.sh kernel"
	fi
fi

echo "checks passed: $npass"
[ $fail -eq 0 ] && echo "run-r5b-test: PASS" || echo "run-r5b-test: FAIL"
exit $fail
