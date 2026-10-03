#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# G10: the device nodes of the packaged illumos-drm drivers after a cold
# boot. Each driver action gives its card and render nodes
# "* 0666 root sys" (/etc/minor_perm), but ttymon(8) of console-login
# applies the /etc/logindevperm rule "/dev/vt/console_user 0600 /dev/dri/*"
# when it opens the console (di_devperm_logout(), at every boot and after
# every console logout): it sets every /dev/dri node to 0600 root root, and
# devfs keeps that in the node's shadow on the root file system across
# reboots, so unprivileged processes could not open the render nodes.
# svc:/system/graphics/drm-perms (driver/graphics/drmadm) runs
# "drmadm perms" before console-login: it restricts the rule to the
# legacy drivers and resets nodes left so to their minor_perm values.
#
#   coldboot   right after a boot, before anything opened a DRM node:
#              drm-perms is online and ran before console-login; every
#              /etc/logindevperm rule for /dev/dri leaves the new stack's
#              drivers out; every /dev/dri node of a new-stack driver is
#              0666 root sys, as /etc/minor_perm says, for the devices in
#              EXPECT (default "virtio_gpu cdrmnull"; one instance each);
#              nobody opens every card and render node read-write; after
#              a restart of console-login (what a console logout does)
#              ttymon read /etc/logindevperm and changed no DRM node; and
#              drmadm status reports no node off its minor_perm entry.
#              On packages without the fix the node checks fail.
#   driver-filter
#              a temporary /etc/logindevperm rule restricted to cdrmnull
#              (FILTER) sets cdrmnull's nodes to 0600 root root at a
#              console-login restart and leaves virtio_gpu's (OTHER)
#              alone, as the restricted rule does for the legacy i915; the
#              rule is removed again and drmadm perms resets the nodes.
#   upgrade    on a system whose nodes the old packages left at 0600 root
#              root: publishes driver/graphics/drmadm from this directory
#              (drmadm, drmadm.8, drm-perms.xml, drmadm.p5m, LICENSE) to
#              the repository REPO, updates the package in the running
#              boot environment, and checks that the service imported with
#              it restricted the rule and reset the nodes at once.
#
# Run as root in a directory that holds drm-test-lib.sh (and, for upgrade,
# the files above), on a disposable VM with the packages of run-g7-test.sh
# install (or later ones) and the virtio-gpu device of the G7 runs.
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
info() { echo "INFO $*"; }

SVC=svc:/system/graphics/drm-perms:default
CONSOLE=svc:/system/console-login:default
NEW_DRIVERS="i915n virtio_gpu cdrmnull rdrmnull"
EXPECT=${EXPECT:-virtio_gpu cdrmnull}
DRMADM=${DRMADM:-/usr/sbin/drmadm}

new_driver() {
	case " $NEW_DRIVERS " in
	*" $1 "*) return 0 ;;
	esac
	return 1
}

# driver_of_major MAJOR: the driver name of a major number.
driver_of_major() {
	awk -v m="$1" '$2 == m { print $1; exit }' /etc/name_to_major
}

# device_prefix DRIVER: the /devices node of its instance 0 (dri_link).
device_prefix() {
	case $1 in
	virtio_gpu) echo "pci1af4,1100@" ;;
	*) echo "$1@0" ;;
	esac
}

# rules: the /etc/logindevperm rules that cover /dev/dri, as libdevinfo
# reads them (comment from the first '#' dropped; "driver" anywhere in
# the rest restricts the rule to the drivers listed after "driver="),
# one per line: "all|legacy|new LINE".
rules() {
	awk -v new="$NEW_DRIVERS" '
	    BEGIN { n = split(new, a, " "); for (i = 1; i <= n; i++) isnew[a[i]] = 1 }
	    {
		body = $0
		if ((i = index(body, "#")) > 0)
			body = substr(body, 1, i - 1)
		k = split(body, w, /[ \t]+/)
		c = 0
		for (j = 1; j <= k; j++)
			if (w[j] != "")
				t[++c] = w[j]
		if (c < 3)
			next
		dri = 0
		nd = split(t[3], dev, ":")
		for (j = 1; j <= nd; j++)
			if (dev[j] ~ /^\/dev\/dri(\/|$)/)
				dri = 1
		if (!dri)
			next
		s = "all"
		if ((i = index(body, "driver")) > 0) {
			s = "legacy"
			l = substr(body, i)
			sub(/^[^=]*=/, "", l)
			nl = split(l, d, /[, \t]+/)
			for (j = 1; j <= nl; j++)
				if (d[j] in isnew)
					s = "new"
		}
		print s, $0
	    }' /etc/logindevperm
}

check_rules() {
	step sh -c 'grep -n "/dev/dri" /etc/logindevperm'
	r=$(rules)
	n=$(echo "$r" | grep -c .)
	if [ -z "$r" ]; then
		info "/etc/logindevperm has no rule for /dev/dri"
	elif echo "$r" | grep -q '^all '; then
		bad "an /etc/logindevperm rule for /dev/dri applies to every driver: $(echo "$r" | grep '^all ' | head -1)"
	elif echo "$r" | grep -q '^new '; then
		bad "an /etc/logindevperm rule for /dev/dri names a new-stack driver"
	else
		pass "the $n /etc/logindevperm rule(s) for /dev/dri apply to the legacy drivers only"
	fi
}

# check_nodes WHEN: every /dev/dri node of a new-stack driver is
# crw-rw-rw- root sys (uid 0, gid 3), and the devices in EXPECT have their
# card and render nodes.
check_nodes() {
	when=$1
	step ls -lL /dev/dri
	for d in $EXPECT; do
		p=$(device_prefix "$d")
		c=$(dri_link "$p" card) && r=$(dri_link "$p" renderD) &&
		    pass "$d has /dev/dri links $c and $r ($when)" ||
		    bad "$d has no card and render link in /dev/dri ($when)"
	done
	nodes=0 good=0
	for l in /dev/dri/*; do
		[ -h "$l" ] || continue
		# shellcheck disable=SC2046 # the fields of ls -lLn
		set -- $(ls -lLn "$l" 2>/dev/null) ""
		[ -n "$1" ] || { info "$l is dangling"; continue; }
		mode=$1 uid=$3 gid=$4
		major=${5%%,*}
		d=$(driver_of_major "$major")
		if ! new_driver "$d"; then
			info "$l belongs to $d, not to the new stack: $mode $uid $gid"
			continue
		fi
		nodes=$((nodes + 1))
		dev=$(ls -l "$l" | sed -n 's/^.* -> \.\.\/\.\.//p')
		if [ "$mode $uid $gid" = "crw-rw-rw- 0 3" ]; then
			good=$((good + 1))
			pass "$l ($d, $dev) is 0666 root sys"
		else
			bad "$l ($d, $dev) is $mode uid $uid gid $gid, not crw-rw-rw- 0 3"
		fi
	done
	want=$(($(echo "$EXPECT" | wc -w) * 2))
	[ $nodes -ge "$want" ] && [ $good -eq $nodes ] &&
	    pass "all $nodes DRM nodes of the new stack are 0666 root sys ($when)" ||
	    bad "$good of $nodes DRM nodes of the new stack are 0666 root sys, $want expected at least ($when)"
}

# check_opens: nobody opens every card and render node of the new stack
# read-write (sh's <> is open(O_RDWR)).
check_opens() {
	for l in /dev/dri/card* /dev/dri/renderD*; do
		[ -h "$l" ] || continue
		major=$(ls -lLn "$l" 2>/dev/null | awk '{ print $5 }')
		new_driver "$(driver_of_major "${major%%,*}")" || continue
		if su nobody -c "/bin/sh -c ': <> $l'" 2>open.err; then
			pass "nobody opens $l read-write"
		else
			bad "nobody cannot open $l read-write: $(cat open.err)"
		fi
	done
}

# restart_console: restarts console-login, whose ttymon opens the console
# and runs di_devperm_logout() as after a console logout, with DTrace
# watching ttymon's opens of /etc/logindevperm and its changes of DRM
# nodes (in logout.d.out); returns once the new ttymon read the file.
restart_console() {
	old=$(pgrep -f 'ttymon -g')
	dtrace -q -Z -n '
	    BEGIN { printf("armed\n"); }
	    syscall::open:entry, syscall::open64:entry
	    /execname == "ttymon"/ { self->path = arg0; }
	    syscall::openat:entry, syscall::openat64:entry
	    /execname == "ttymon"/ { self->path = arg1; }
	    syscall::open*:entry
	    /self->path && strstr(copyinstr(self->path), "logindevperm") != NULL/
	    { printf("ttymon %d opens %s\n", pid, copyinstr(self->path)); }
	    syscall::open*:entry { self->path = 0; }
	    fbt:devfs:devfs_setattr:entry, fbt:devfs:devfs_setsecattr:entry
	    /execname == "ttymon" &&
	     (strstr(stringof(((struct dv_node *)args[0]->v_data)->dv_name), "card") != NULL ||
	      strstr(stringof(((struct dv_node *)args[0]->v_data)->dv_name), "renderD") != NULL)/
	    { printf("ttymon %d %s %s\n", pid, probefunc,
	        stringof(((struct dv_node *)args[0]->v_data)->dv_name)); }' > logout.d.out 2>&1 &
	dpid=$!
	i=0
	while ! grep -q armed logout.d.out && [ $i -lt 30 ]; do sleep 0.5; i=$((i + 1)); done
	step svcadm restart "$CONSOLE"
	i=0
	while [ $i -lt 30 ]; do
		new=$(pgrep -f 'ttymon -g')
		[ -n "$new" ] && [ "$new" != "$old" ] && grep -q "opens" logout.d.out && break
		sleep 0.5
		i=$((i + 1))
	done
	# di_devperm_logout() walks the rules after the open of the file.
	sleep 2
	kill $dpid 2>/dev/null
	wait $dpid 2>/dev/null
	cat logout.d.out
	echo "ttymon -g: pid $old before, $(pgrep -f 'ttymon -g') after"
	grep -q "ttymon .* opens /etc/logindevperm" logout.d.out &&
	    pass "the restarted ttymon read /etc/logindevperm (di_devperm_logout ran)" ||
	    bad "the restarted ttymon did not read /etc/logindevperm: the logout path was not exercised"
}

# check_logout: after a console-login restart, ttymon changed no DRM node.
check_logout() {
	restart_console
	grep -q "devfs_set" logout.d.out &&
	    bad "ttymon changed DRM nodes: $(grep devfs_set logout.d.out | head -2)" ||
	    pass "ttymon changed no DRM node"
}

# node_attrs DRIVER: "MODE UID GID" of each /dev/dri node of DRIVER.
node_attrs() {
	p=$(device_prefix "$1")
	for l in /dev/dri/*; do
		case $(ls -l "$l" 2>/dev/null) in
		*"/$p"*) ls -lLn "$l" | awk '{ print $1, $3, $4 }' ;;
		esac
	done | sort -u
}

phase_coldboot() {
	echo "=== coldboot: the DRM nodes after a cold boot"
	step uptime
	step sh -c 'kstat -p unix:0:system_misc:boot_time; date +%s'
	step sh -c "svcs -o state,stime,fmri $SVC $CONSOLE svc:/system/device/local:default"
	[ "$(svcs -H -o state $SVC 2>/dev/null)" = online ] && pass "$SVC is online" ||
	    bad "$SVC is not online"
	step sh -c "svcs -d $CONSOLE"
	svcs -H -d "$CONSOLE" | grep -q "$SVC" && pass "console-login depends on drm-perms" ||
	    bad "console-login does not depend on drm-perms"
	t1=$(svcprop -p restarter/state_timestamp "$SVC" 2>/dev/null)
	t2=$(svcprop -p restarter/start_method_timestamp "$CONSOLE" 2>/dev/null)
	echo "drm-perms online at $t1, console-login's start method at $t2"
	[ -n "$t1" ] && [ -n "$t2" ] && awk -v a="$t1" -v b="$t2" 'BEGIN { exit !(a + 0 <= b + 0) }' &&
	    pass "drm-perms was online before console-login started ttymon" ||
	    bad "drm-perms did not finish before console-login started"
	log=$(svcs -L "$SVC" 2>/dev/null)
	[ -n "$log" ] && step tail -20 "$log"
	check_rules
	check_nodes "after the boot, before any open"
	check_opens
	check_logout
	check_nodes "after the console-login restart"
	step "$DRMADM" status
	"$DRMADM" status > drmadm-status.out 2>&1
	grep -q '/etc/minor_perm:' drmadm-status.out &&
	    bad "drmadm status reports nodes off their /etc/minor_perm entry" ||
	    pass "drmadm status reports no node off its /etc/minor_perm entry"
}

phase_driver_filter() {
	echo "=== driver-filter: a logindevperm rule with a driver list"
	# The restriction drmadm writes relies on libdevinfo's driver lists:
	# a rule restricted to FILTER (default cdrmnull) must still reach
	# FILTER's nodes, as the restricted rule reaches the legacy i915's, and
	# must leave OTHER's (default virtio_gpu) alone. The rule is added
	# for the test and removed again; drmadm perms then resets FILTER's
	# nodes.
	f=${FILTER:-cdrmnull} o=${OTHER:-virtio_gpu}
	[ -n "$(node_attrs "$f")" ] && [ -n "$(node_attrs "$o")" ] ||
	    { bad "$f or $o has no /dev/dri node"; return 1; }
	step cp -p /etc/logindevperm logindevperm.saved || exit 1
	printf '/dev/vt/console_user\t0600\t/dev/dri/*\tdriver=%s\t# run-g10-test\n' "$f" >> /etc/logindevperm
	step tail -2 /etc/logindevperm
	restart_console
	step ls -lL /dev/dri
	a=$(node_attrs "$f")
	b=$(node_attrs "$o")
	echo "$f: $a; $o: $b"
	[ "$a" = "crw------- 0 0" ] && pass "the rule restricted to $f set $f's nodes to 0600 root root" ||
	    bad "the rule restricted to $f left $f's nodes at $a"
	[ "$b" = "crw-rw-rw- 0 3" ] && pass "and left $o's at 0666 root sys" ||
	    bad "and changed $o's to $b"
	step cp -p logindevperm.saved /etc/logindevperm || exit 1
	step "$DRMADM" perms
	a=$(node_attrs "$f")
	[ "$a" = "crw-rw-rw- 0 3" ] && pass "drmadm perms reset $f's nodes to 0666 root sys" ||
	    bad "drmadm perms left $f's nodes at $a"
}

phase_upgrade() {
	echo "=== upgrade: the fixed drmadm package on a system with clobbered nodes"
	: "${REPO:?set REPO to the file repository of the illumos-drm publisher}"
	step ls -lL /dev/dri
	info "before the update: /etc/logindevperm rules for /dev/dri: $(rules | awk '{ print $1 }' | tr '\n' ' ')"
	P=$PWD/proto-drmadm
	rm -rf "$P"
	mkdir -p "$P/usr/sbin" "$P/usr/share/man/man8" "$P/lib/svc/manifest/system"
	step cp drmadm "$P/usr/sbin/drmadm" || exit 1
	step cp drmadm.8 "$P/usr/share/man/man8/drmadm.8" || exit 1
	step cp drm-perms.xml "$P/lib/svc/manifest/system/drm-perms.xml" || exit 1
	step svccfg validate drm-perms.xml && pass "svccfg validates drm-perms.xml" ||
	    bad "svccfg does not validate drm-perms.xml"
	step pkgsend publish -s "$REPO" -d "$P" -d . drmadm.p5m || exit 1
	step pkg refresh illumos-drm
	step pkg update -v driver/graphics/drmadm || bad "pkg update driver/graphics/drmadm failed"
	step pkg contents -m driver/graphics/drmadm
	pkg contents -m driver/graphics/drmadm | grep -q 'drm-perms.xml' &&
	    pass "the installed drmadm package delivers drm-perms.xml" ||
	    bad "the installed drmadm package has no drm-perms.xml"
	i=0
	while [ "$(svcs -H -o state $SVC 2>/dev/null)" != online ] && [ $i -lt 60 ]; do
		sleep 1
		i=$((i + 1))
	done
	step svcs -x "$SVC"
	[ "$(svcs -H -o state $SVC 2>/dev/null)" = online ] && pass "$SVC came online with the package" ||
	    bad "$SVC is not online after the update"
	log=$(svcs -L "$SVC" 2>/dev/null)
	[ -n "$log" ] && step cat "$log"
	check_rules
	check_nodes "after the package update, no reboot"
	check_opens
}

case "${1:-}" in
coldboot) phase_coldboot ;;
driver-filter) phase_driver_filter ;;
upgrade) phase_upgrade ;;
*) echo "usage: run-g10-test.sh coldboot|driver-filter|upgrade" >&2
   exit 2 ;;
esac
echo
echo "checks passed: $npass"
[ $fail -eq 0 ] && echo "run-g10-test $1: PASS" || echo "run-g10-test $1: FAIL"
exit $fail
