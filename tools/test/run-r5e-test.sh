#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The guest test of illumos-drm R5e: misc/drm_helpers, the carried Linux
# helpers (drm_mm, drm_cache's drm_clflush_virt_range()), and its
# self-tests in the test driver drm_helpers_test.
#
#   1  misc/drm_helpers loads alone (krtld resolves every symbol it
#      imports) and unloads again.
#   2  for every run in RUNS ("seed:scale ..."): add_drv drm_helpers_test
#      with that seed and scale in its .conf; attaching it loads
#      misc/drm_helpers as its dependency and runs the drm_mm and
#      drm_cache self-tests; the kstat drm_helpers_test:0:selftest must
#      report EXPECT_TESTS tests, no failed test or check, and the
#      requested seed and scale; misc/drm_helpers refuses to unload while
#      the driver is loaded; rem_drv; then misc/drm_helpers unloads.
#   3  a run with inject_failure=1 adds a test that fails on purpose: the
#      attach still succeeds, the kstat counts exactly one failed test and
#      one failed check, and the failure is logged (CE_WARN).
#   4  the driver files in /etc and the /devices/pseudo entries are as
#      before, and everything installed is removed again.
#   5  with PKG=<archive.p5p> (packaging/drm-helpers.p5m published into a
#      p5p archive): pkg install driver/graphics/drm-helpers from it, the
#      installed module is the tested one and loads and unloads, pkg
#      verify is clean, pkg uninstall removes it.
#
# Run as root on a disposable illumos VM, in a directory with drm_helpers
# and drm_helpers_test (carried/build.sh) and drm_helpers_test.conf
# (carried/test/). It refuses to run where a misc/drm_helpers is installed
# already (a package), so that it never replaces one. Environment: RUNS
# [20261004:1 1:2 424242:3], EXPECT_TESTS [16], PKG [none].
set -u
cd "$(dirname "$0")" || exit 1

RUNS=${RUNS:-"20261004:1 1:2 424242:3"}
EXPECT_TESTS=${EXPECT_TESTS:-16}
MOD=/kernel/misc/amd64/drm_helpers
DRV=/usr/kernel/drv/amd64/drm_helpers_test
CONF=/usr/kernel/drv/drm_helpers_test.conf
ETC="/etc/name_to_major /etc/driver_aliases /etc/driver_classes
/etc/minor_perm /etc/security/device_policy /etc/security/extra_privs"

fail=0
npass=0
pass() { echo "PASS $*"; npass=$((npass + 1)); }
bad() { echo "FAIL $*"; fail=1; }

step() {
	echo
	echo "\$ $*"
	"$@"
	rc=$?
	echo "[exit $rc]"
	return $rc
}

modid() {
	modinfo | awk -v m="$1" '$6 == m { print $1 }'
}

state() {
	# shellcheck disable=SC2086 # the list of /etc files
	digest -v -a sha256 $ETC 2>&1
	for d in /devices/pseudo/*drm_helpers*; do
		[ -e "$d" ] && echo "$d"
	done
}

for f in drm_helpers drm_helpers_test drm_helpers_test.conf; do
	[ -f "$f" ] || { echo "run-r5e-test: missing $f"; exit 1; }
done
if [ -e "$MOD" ] || [ -e /usr/kernel/misc/amd64/drm_helpers ] ||
    [ -e "$DRV" ] || grep -q '^drm_helpers_test ' /etc/name_to_major; then
	echo "run-r5e-test: drm_helpers or drm_helpers_test is installed" \
	    "already; not touching it"
	exit 1
fi

[ -f REVISION ] && step cat REVISION
step uname -a
step digest -v -a sha256 drm_helpers drm_helpers_test
state > state.before

echo
echo "=== 1: misc/drm_helpers alone"
step cp drm_helpers "$MOD"
step chmod 0755 "$MOD"
if step modload -p misc/drm_helpers && [ -n "$(modid drm_helpers)" ]; then
	modinfo | awk '$6 == "drm_helpers"'
	pass "misc/drm_helpers loads alone"
	if step modunload -i "$(modid drm_helpers)" &&
	    [ -z "$(modid drm_helpers)" ]; then
		pass "misc/drm_helpers unloads"
	else
		bad "misc/drm_helpers does not unload"
	fi
else
	bad "misc/drm_helpers does not load"
fi

echo
echo "=== 2: drm_helpers_test"
step cp drm_helpers_test "$DRV"
step chmod 0755 "$DRV"
for run in $RUNS; do
	seed=${run%%:*}
	scale=${run#*:}
	echo
	echo "--- seed $seed, scale $scale"
	sed -e "s/seed=[0-9]*/seed=$seed/" -e "s/scale=[0-9]*/scale=$scale/" \
	    drm_helpers_test.conf > "$CONF"
	grep '^name=' "$CONF"
	[ -z "$(modid drm_helpers)" ] ||
	    bad "misc/drm_helpers loaded before the run"
	mark=$(wc -l < /var/adm/messages)
	if ! step add_drv drm_helpers_test; then
		bad "add_drv drm_helpers_test (seed $seed)"
		continue
	fi
	step kstat -p drm_helpers_test:0:selftest
	tp=$(kstat -p drm_helpers_test:0:selftest:tests_passed | awk '{print $2}')
	tf=$(kstat -p drm_helpers_test:0:selftest:tests_failed | awk '{print $2}')
	cp=$(kstat -p drm_helpers_test:0:selftest:checks_passed | awk '{print $2}')
	cf=$(kstat -p drm_helpers_test:0:selftest:checks_failed | awk '{print $2}')
	ks=$(kstat -p drm_helpers_test:0:selftest:seed | awk '{print $2}')
	kx=$(kstat -p drm_helpers_test:0:selftest:scale | awk '{print $2}')
	echo "messages:"
	tail -n +"$((mark + 1))" /var/adm/messages | grep 'drm_helpers_test' |
	    sed 's/^.*drm_helpers_test/drm_helpers_test/'
	if [ "${tp:-0}" -eq "$EXPECT_TESTS" ] && [ "${tf:-1}" -eq 0 ] &&
	    [ "${cf:-1}" -eq 0 ] && [ "${cp:-0}" -gt 0 ] &&
	    [ "${ks:-}" = "$seed" ] && [ "${kx:-}" = "$scale" ]; then
		pass "self-tests, seed $seed scale $scale: $tp tests, $cp checks"
	else
		bad "self-tests, seed $seed scale $scale: passed $tp, failed" \
		    "$tf, checks $cp passed, $cf failed (seed $ks, scale $kx)"
	fi
	if [ -n "$(modid drm_helpers)" ]; then
		pass "misc/drm_helpers loaded as drm_helpers_test's dependency"
		if modunload -i "$(modid drm_helpers)" 2>/dev/null; then
			bad "misc/drm_helpers unloaded under its dependent"
		else
			pass "misc/drm_helpers refuses to unload under its dependent"
		fi
	else
		bad "misc/drm_helpers not loaded with drm_helpers_test"
	fi
	step rem_drv drm_helpers_test || bad "rem_drv drm_helpers_test"
	id=$(modid drm_helpers_test)
	[ -z "$id" ] || step modunload -i "$id"
	id=$(modid drm_helpers)
	if [ -n "$id" ] && step modunload -i "$id" &&
	    [ -z "$(modid drm_helpers)" ]; then
		pass "misc/drm_helpers unloads after rem_drv"
	else
		bad "misc/drm_helpers still loaded after rem_drv"
	fi
done

echo
echo "=== 3: the failure path (inject_failure=1)"
sed -e 's/scale=[0-9]*;/scale=1 inject_failure=1;/' drm_helpers_test.conf \
    > "$CONF"
grep '^name=' "$CONF"
mark=$(wc -l < /var/adm/messages)
if step add_drv drm_helpers_test; then
	step kstat -p drm_helpers_test:0:selftest
	tp=$(kstat -p drm_helpers_test:0:selftest:tests_passed | awk '{print $2}')
	tf=$(kstat -p drm_helpers_test:0:selftest:tests_failed | awk '{print $2}')
	cf=$(kstat -p drm_helpers_test:0:selftest:checks_failed | awk '{print $2}')
	warned=$(tail -n +"$((mark + 1))" /var/adm/messages |
	    grep -c 'drm_helpers_test: selftest/inject_failure: FAILED')
	if [ "${tp:-0}" -eq "$EXPECT_TESTS" ] && [ "${tf:-0}" -eq 1 ] &&
	    [ "${cf:-0}" -eq 1 ] && [ "$warned" -eq 1 ]; then
		pass "an injected failure is counted (1 test, 1 check) and logged"
	else
		bad "injected failure: passed $tp, failed $tf, checks failed" \
		    "$cf, logged $warned"
	fi
	step rem_drv drm_helpers_test || bad "rem_drv drm_helpers_test"
	id=$(modid drm_helpers_test)
	[ -z "$id" ] || step modunload -i "$id"
	id=$(modid drm_helpers)
	[ -z "$id" ] || step modunload -i "$id"
else
	bad "add_drv drm_helpers_test with inject_failure=1 (attach must succeed)"
fi

echo
echo "=== 4: cleanup"
step rm -f "$DRV" "$CONF" "$MOD"
state > state.after
if step diff state.before state.after; then
	pass "/etc driver files and /devices/pseudo as before"
else
	bad "/etc driver files or /devices/pseudo changed"
fi

if [ -n "${PKG:-}" ]; then
	echo
	echo "=== 5: the package ($PKG)"
	want=$(digest -a sha256 drm_helpers)
	if step pkg install -g "$PKG" driver/graphics/drm-helpers; then
		step pkg contents -m driver/graphics/drm-helpers
		got=$(digest -a sha256 "$MOD" 2>/dev/null)
		if [ "$got" = "$want" ]; then
			pass "the package installs the tested $MOD"
		else
			bad "the package's $MOD is not the tested module"
		fi
		if step modload -p misc/drm_helpers &&
		    step modunload -i "$(modid drm_helpers)"; then
			pass "the packaged module loads and unloads"
		else
			bad "the packaged module does not load or unload"
		fi
		if step pkg verify driver/graphics/drm-helpers; then
			pass "pkg verify driver/graphics/drm-helpers"
		else
			bad "pkg verify driver/graphics/drm-helpers"
		fi
		if step pkg uninstall driver/graphics/drm-helpers &&
		    [ ! -e "$MOD" ]; then
			pass "pkg uninstall removes the module"
		else
			bad "pkg uninstall left $MOD"
		fi
	else
		bad "pkg install -g $PKG driver/graphics/drm-helpers"
	fi
fi

echo
echo "checks passed: $npass"
if [ "$fail" -eq 0 ]; then
	echo "run-r5e-test: PASS"
	exit 0
fi
echo "run-r5e-test: FAIL"
exit 1
