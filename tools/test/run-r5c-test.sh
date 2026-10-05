#!/bin/sh
#
# SPDX-License-Identifier: MIT
# Copyright 2026 The illumos-drm authors
#
# The guest test of illumos-drm R5c: misc/linuxkpi, the Linux compatibility
# layer, its kernel self-test, misc/drm_helpers on top of it, and the
# loading of i915n (Linux i915, headless).
#
#   0  none of the modules defines a global symbol the running kernel
#      has (krtld would bind the module's callers to the kernel's).
#   A  misc/drm_core (drm-test-lib.sh) and misc/linuxkpi load: krtld
#      resolves every symbol the layer imports, and the layer's _init (CPU
#      facts, tasks, RCU, timers, workqueues, DRM glue, PCI) succeeds.
#   B  linuxkpi_selftest, bound to QEMU's PCI test device (pci1b36,5; the
#      guest needs -device pci-testdev), runs its checks from probe():
#      locks, workqueues, timers, completions, wait queues, RCU, idr/xarray,
#      slab caches, pages, DMA scatterlists and coherent memory. The log
#      line "linuxkpi_selftest: done <n> passed 0 failed" and the device
#      property linuxkpi-selftest-failed=0 are required; then rem_drv.
#   C  misc/drm_helpers (now on misc/linuxkpi) loads and unloads.
#   D  i915n: add_drv without aliases (the package's driver action) loads
#      the module: krtld resolves its imports from misc/drm_core,
#      misc/linuxkpi and misc/drm_helpers, i915's module_init registers its
#      PCI driver with the layer, nothing attaches (QEMU has no Intel GPU);
#      modunload and rem_drv.
#   E  the layer unloads after its dependents, with no WARN logged during
#      the run (the layer's lkpi_warnings counter, read with mdb -k), and
#      the /etc driver files are as before.
#
# Run as root on a disposable illumos VM, in a directory with linuxkpi,
# linuxkpi_selftest, drm_helpers and i915n (linuxkpi/build.sh,
# carried/build.sh, drivers/i915/build.sh), drm-test-lib.sh, and
# drm_module.a plus drm_core.mapfile unless misc/drm_core is installed.
# It refuses to run where any of the R5c modules is installed already.
set -u
cd "$(dirname "$0")" || exit 1

MISC=/kernel/misc/amd64
DRV=/usr/kernel/drv/amd64
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

etc_state() {
	# shellcheck disable=SC2086 # the list of /etc files
	digest -v -a sha256 $ETC 2>&1
}

# shellcheck disable=SC1091
. ./drm-test-lib.sh

for f in linuxkpi linuxkpi_selftest drm_helpers i915n; do
	[ -f "$f" ] || { echo "run-r5c-test: missing $f"; exit 1; }
done
for p in $MISC/linuxkpi $MISC/drm_helpers /usr/kernel/misc/amd64/linuxkpi \
    $DRV/linuxkpi_selftest /kernel/drv/amd64/i915n $DRV/i915n; do
	[ -e "$p" ] && { echo "run-r5c-test: $p is installed already; not touching it"; exit 1; }
done
for d in linuxkpi_selftest i915n; do
	grep -q "^$d " /etc/name_to_major &&
	    { echo "run-r5c-test: driver $d is added already"; exit 1; }
done

echo "=== R5c guest test: $(uname -v), $(date -u +%FT%TZ)"
step digest -a sha256 linuxkpi linuxkpi_selftest drm_helpers i915n
for m in linuxkpi linuxkpi_selftest drm_helpers i915n; do
	step elfdump -d "$m" | grep NEEDED
done
etc_state > /tmp/r5c-etc.before
# The system log from here on (kernel messages; line count, since a
# logger(1) mark would be a user.* message).
log0=$(wc -l < /var/adm/messages)
newlog() {
	tail +$((log0 + 1)) /var/adm/messages
}

echo
echo "=== 0: no global symbol of these modules is the kernel's too"
# krtld reports such a symbol multiply defined and binds the module's own
# callers to the kernel's.
nm -p /dev/ksyms | awk 'NF >= 3 { print $NF }' | sort -u > /tmp/r5c-ksyms
for m in linuxkpi drm_helpers i915n linuxkpi_selftest; do
	nm "$m" | awk -F'|' '$5 ~ /GLOB/ && $7 !~ /UNDEF/ { gsub(/ /, "", $8); print $8 }' |
	    grep -v -x -e _init -e _fini -e _info | sort -u > /tmp/r5c-defs
	both=$(comm -12 /tmp/r5c-defs /tmp/r5c-ksyms | tr '\n' ' ')
	if [ -z "$both" ]; then
		pass "$m defines no kernel symbol"
	else
		bad "$m defines kernel symbols: $both"
	fi
done

echo
echo "=== A: misc/drm_core and misc/linuxkpi"
drm_core_install || { bad "misc/drm_core does not load"; exit 1; }
step cp linuxkpi $MISC/linuxkpi
if step modload -p misc/linuxkpi; then
	pass "misc/linuxkpi loads (every import resolved, _init succeeded)"
else
	bad "misc/linuxkpi does not load"
	step tail -30 /var/adm/messages
	rm -f $MISC/linuxkpi
	drm_core_remove
	exit 1
fi
step modinfo -c -i "$(modid linuxkpi)"
warn0=$(echo 'lkpi_warnings/E' | mdb -k 2>/dev/null | awk 'NF == 2 { print $2 }')
echo "lkpi_warnings at start: ${warn0:-unknown}"

echo
echo "=== B: linuxkpi_selftest on the PCI test device"
if ! prtconf -pv | grep -q "pci1b36,5"; then
	bad "no QEMU pci-testdev (pci1b36,5) in this guest"
else
	step cp linuxkpi_selftest $DRV/linuxkpi_selftest
	# WARNs inside the self-test are its own (it provokes some on
	# purpose); the run's no-WARN check leaves its window out.
	warn_st0=$(echo 'lkpi_warnings/E' | mdb -k 2>/dev/null | awk 'NF == 2 { print $2 }')
	find /devices -name '*:card*' 2>/dev/null | sort > /tmp/r5c-cards.before
	if step add_drv -i '"pci1b36,5"' linuxkpi_selftest; then
		pass "add_drv linuxkpi_selftest"
	else
		bad "add_drv linuxkpi_selftest"
	fi
	# probe() runs the checks at attach; wait for the summary line.
	i=0
	while [ $i -lt 60 ]; do
		newlog | grep -q 'linuxkpi_selftest: done' && break
		sleep 1
		i=$((i + 1))
	done
	newlog | grep 'linuxkpi_selftest' > /tmp/r5c-selftest.log
	cat /tmp/r5c-selftest.log
	done_line=$(grep 'linuxkpi_selftest: done' /tmp/r5c-selftest.log | tail -1)
	ok=$(echo "$done_line" | sed -n 's/.*done \([0-9]*\) passed \([0-9]*\) failed.*/\1/p')
	nf=$(echo "$done_line" | sed -n 's/.*done \([0-9]*\) passed \([0-9]*\) failed.*/\2/p')
	if [ -n "$ok" ] && [ "$nf" = 0 ] && [ "$ok" -gt 0 ]; then
		pass "linuxkpi_selftest: $ok checks passed, 0 failed"
	else
		bad "linuxkpi_selftest: ${done_line:-no summary line}"
	fi
	prop=$(prtconf -v | awk '/linuxkpi-selftest-failed/ { getline; print }' | sed -n "s/.*value=\(.*\)/\1/p" | head -1)
	echo "property linuxkpi-selftest-failed: ${prop:-absent}"
	case "$prop" in
	00000000|0) pass "device property linuxkpi-selftest-failed=0" ;;
	*) bad "device property linuxkpi-selftest-failed=${prop:-absent}" ;;
	esac
	# The DRM device the self-test registered: detach is refused while a
	# node is open, and succeeds once it is closed, running the driver's
	# release (which frees a GEM object made before drm_dev_register())
	# before the core unregisters.
	# The node the self-test's device added (QEMU names the test device's
	# node after its subsystem IDs).
	node=$(find /devices -name '*:card*' 2>/dev/null | sort |
	    comm -13 /tmp/r5c-cards.before - | head -1)
	echo "DRM node: ${node:-none}"
	if [ -n "$node" ] && exec 3<"$node"; then
		id=$(modid linuxkpi_selftest)
		if [ -n "$id" ] && modunload -i "$id" 2>/dev/null; then
			bad "detach succeeded with a DRM node open"
		else
			pass "detach refused while a DRM node is open"
		fi
		exec 3<&-
	else
		bad "no openable DRM node of the self-test"
	fi
	mark=$(wc -l < /var/adm/messages)
	if step rem_drv linuxkpi_selftest; then
		pass "rem_drv linuxkpi_selftest (detach through the layer)"
	else
		bad "rem_drv linuxkpi_selftest"
	fi
	if tail +$((mark + 1)) /var/adm/messages | grep -q 'DRM release ran, GEM object freed'; then
		pass "the DRM device's release ran before the core unregistered it"
	else
		bad "no DRM release at detach"
	fi
	id=$(modid linuxkpi_selftest)
	[ -n "$id" ] && step modunload -i "$id"
	step rm -f $DRV/linuxkpi_selftest
	warn_st1=$(echo 'lkpi_warnings/E' | mdb -k 2>/dev/null | awk 'NF == 2 { print $2 }')
	echo "lkpi_warnings during the self-test: ${warn_st0:-?} -> ${warn_st1:-?}"
fi

echo
echo "=== C: misc/drm_helpers on misc/linuxkpi"
step cp drm_helpers $MISC/drm_helpers
if step modload -p misc/drm_helpers; then
	pass "misc/drm_helpers loads on misc/linuxkpi"
	id=$(modid drm_helpers)
	if [ -n "$id" ] && step modunload -i "$id"; then
		pass "misc/drm_helpers unloads"
	else
		bad "misc/drm_helpers does not unload"
	fi
else
	bad "misc/drm_helpers does not load"
fi

echo
echo "=== D: i915n loads without a device"
step cp i915n $DRV/i915n
# The package's driver action: no aliases (drmadm select new adds them).
# add_drv binds the major number in the running kernel and loads the
# module (no device to attach: it may say so).
step add_drv i915n
if grep -q '^i915n ' /etc/name_to_major; then
	pass "add_drv i915n (no aliases)"
else
	bad "add_drv i915n"
fi
# Twice: module_exit must unregister the PCI driver so that the second
# module_init can register it again.
step modload -p drv/i915n
id=$(modid i915n)
if [ -n "$id" ] && step modunload -i "$id"; then
	pass "i915n loads and unloads (first time)"
else
	bad "i915n first load/unload"
fi
step modload -p drv/i915n
if [ -n "$(modid i915n)" ]; then
	pass "i915n loads: imports resolved, i915 module_init registered its PCI driver"
	step modinfo -c -i "$(modid i915n)"
	# misc/linuxkpi must refuse to unload while i915n depends on it.
	lid=$(modid linuxkpi)
	if modunload -i "$lid" 2>/dev/null; then
		bad "misc/linuxkpi unloaded while i915n is loaded"
	else
		pass "misc/linuxkpi stays while i915n is loaded"
	fi
	id=$(modid i915n)
	if [ -n "$id" ] && step modunload -i "$id"; then
		pass "i915n unloads (module_exit unregistered its PCI driver)"
	else
		bad "i915n does not unload"
	fi
else
	bad "i915n does not load"
	step tail -40 /var/adm/messages
fi
# The attach-failure path: bound to the PCI test device, which is not in
# i915's ID table, i915n refuses the attach and nothing stays behind.
if prtconf -pv | grep -q "pci1b36,5"; then
	step update_drv -a -i '"pci1b36,5"' i915n
	step prtconf -D | grep 'pci1b36,5'
	if prtconf -D | grep 'pci1b36,5' | grep -q 'driver name: i915n'; then
		bad "i915n attached to the PCI test device"
	else
		pass "i915n refuses a device outside its ID table"
	fi
	step update_drv -d -i '"pci1b36,5"' i915n
fi
step rem_drv i915n
step rm -f $DRV/i915n

echo
echo "=== E: unload and cleanup"
warn1=$(echo 'lkpi_warnings/E' | mdb -k 2>/dev/null | awk 'NF == 2 { print $2 }')
echo "lkpi_warnings at end: ${warn1:-unknown}"
outside=$(( ${warn1:-0} - ${warn0:-0} - (${warn_st1:-0} - ${warn_st0:-0}) ))
if [ -n "$warn0" ] && [ -n "$warn1" ] && [ "$outside" -eq 0 ]; then
	pass "no WARN fired in the layer outside the self-test"
else
	bad "WARNs fired in the layer outside the self-test ($outside)"
	newlog | grep "WARNING at" | head -20
fi
for m in drm_helpers linuxkpi; do
	id=$(modid $m)
	[ -n "$id" ] && step modunload -i "$id"
done
if [ -z "$(modid linuxkpi)" ]; then
	pass "misc/linuxkpi unloads after its dependents"
else
	bad "misc/linuxkpi stays loaded"
fi
step rm -f $MISC/drm_helpers $MISC/linuxkpi
drm_core_remove
etc_state > /tmp/r5c-etc.after
if cmp -s /tmp/r5c-etc.before /tmp/r5c-etc.after; then
	pass "the /etc driver files are as before"
else
	bad "the /etc driver files changed"
	diff /tmp/r5c-etc.before /tmp/r5c-etc.after
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "R5c guest test: PASS ($npass checks)"
else
	echo "R5c guest test: FAIL ($npass passed)"
fi
exit "$fail"
