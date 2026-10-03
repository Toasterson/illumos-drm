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
#   arm        arms anonymous DTrace for the next boot (DRM node changes,
#              ttymon's reads of /etc/logindevperm, renames onto it); with
#              REVERT=1 it first restores the packaged /etc/logindevperm
#              (pkg revert), whose rule applies to every driver, as on the
#              first boot after an installation. Reboot afterwards.
#   coldboot   right after a boot, before anything opened a DRM node:
#              drm-perms is online and came online in this boot before
#              console-login started; with a trace from arm, ttymon -g
#              read /etc/logindevperm and changed no DRM node, and
#              drm-perms' rename of the file came before that read; every
#              /etc/logindevperm rule for /dev/dri leaves the new stack's
#              drivers out; every /dev/dri node of a new-stack driver is
#              0666 root sys, as /etc/minor_perm says, for the devices in
#              EXPECT (default "virtio_gpu cdrmnull"; one instance each);
#              nobody opens every card and render node read-write; after
#              a restart of console-login (what a console logout does)
#              ttymon read /etc/logindevperm and changed no DRM node; and
#              drmadm status lists each new-stack node at its minor_perm
#              values. On packages without the fix the node checks fail.
#   rewrite    drmadm -R perms on scratch roots: the packaged rule and a
#              mixed rule are restricted (the mixed one split), rules with
#              a driver list and pattern rules are left and reported, the
#              file keeps its owner, group and mode, a second run and 20
#              concurrent runs give the same file and leave no temporary
#              file, and a rule that would exceed libdevinfo's line length
#              is refused with the file unchanged.
#   minorperm  two matching /etc/minor_perm patterns for virtio_gpu's card
#              node (D): drmadm status expects the mode the kernel applied
#              (the later pattern's); the entries are removed again.
#   repair     the packaged /etc/logindevperm back (pkg revert) and a
#              console-login restart reproduce the fault (ttymon sets the
#              nodes to 0600 root root); a restart of drm-perms then
#              restricts the rule and resets the nodes to 0666 root sys.
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

# rules: the /etc/logindevperm rules that reach /dev/dri nodes, as
# libdevinfo reads them (comment from the first '#' dropped; "driver"
# anywhere in the rest restricts the rule to the drivers listed after
# "driver="; each component of a device is an anchored regular expression
# or "*"), one per line: "all|pattern|legacy|new LINE", for FILE (default
# /etc/logindevperm). drmadm has its own reader; this one is the test's.
rules() {
	awk -v new="$NEW_DRIVERS" '
	    function cm(re, name) {
		if (re == "*")
			return 1
		return name ~ ("^(" re ")$")
	    }
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
		dri = maybe = 0
		nd = split(t[3], dev, ":")
		for (j = 1; j <= nd; j++) {
			m = split(dev[j], p, "/")
			if (p[1] != "" || m < 4)
				continue
			if (p[2] == "dev" && p[3] == "dri")
				dri = 1
			else if (cm(p[2], "dev") && cm(p[3], "dri"))
				maybe = 1
		}
		if (!dri && !maybe)
			next
		s = dri ? "all" : "pattern"
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
	    }' "${1:-/etc/logindevperm}"
}

check_rules() {
	step sh -c 'grep -n "/dev/dri" /etc/logindevperm'
	r=$(rules)
	n=$(echo "$r" | grep -c .)
	if [ -z "$r" ]; then
		info "/etc/logindevperm has no rule for /dev/dri"
	elif echo "$r" | grep -q -E '^(all|pattern) '; then
		bad "an /etc/logindevperm rule for /dev/dri applies to every driver: $(echo "$r" | grep -E '^(all|pattern) ' | head -1)"
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

# BOOT_D: the anonymous DTrace program of "arm" for the next boot: every
# change of a DRM node's owner, group, mode or ACL in devfs, ttymon's opens
# of /etc/logindevperm and every rename onto it, with nanosecond wall-clock
# times, process and parent.
BOOT_D='
#pragma D option quiet
#pragma D option bufsize=4m
fbt:devfs:devfs_setattr:entry, fbt:devfs:devfs_setsecattr:entry
/strstr(stringof(((struct dv_node *)args[0]->v_data)->dv_name), "card") != NULL ||
 strstr(stringof(((struct dv_node *)args[0]->v_data)->dv_name), "renderD") != NULL/
{
	printf("%d %s %s by %s pid %d ppid %d\n", walltimestamp, probefunc,
	    stringof(((struct dv_node *)args[0]->v_data)->dv_name), execname, pid, ppid);
}
syscall::open:entry /execname == "ttymon"/ { self->o = arg0; }
syscall::openat:entry /execname == "ttymon"/ { self->o = arg1; }
syscall::open:entry, syscall::openat:entry
/self->o && strstr(copyinstr(self->o), "logindevperm") != NULL/
{
	printf("%d open %s by %s pid %d ppid %d\n", walltimestamp, copyinstr(self->o),
	    execname, pid, ppid);
}
syscall::open:entry, syscall::openat:entry { self->o = 0; }
syscall::rename:entry { self->r = arg1; }
syscall::renameat:entry { self->r = arg3; }
syscall::rename:entry, syscall::renameat:entry
/self->r && strstr(copyinstr(self->r), "logindevperm") != NULL/
{
	printf("%d rename %s by %s pid %d ppid %d\n", walltimestamp, copyinstr(self->r),
	    execname, pid, ppid);
}
syscall::rename:entry, syscall::renameat:entry { self->r = 0; }
proc:::exec-success /execname == "ttymon"/
{
	printf("%d exec ttymon pid %d ppid %d: %s\n", walltimestamp, pid, ppid,
	    curpsinfo->pr_psargs);
}
'

# check_boot_trace: if "arm" armed anonymous tracing for this boot, reads
# it: console-login's ttymon (ttymon -g) ran, read /etc/logindevperm and
# changed no DRM node, and a rename onto /etc/logindevperm in this boot
# (drm-perms restricting the rule) came before ttymon read it; then
# removes the anonymous enabling.
check_boot_trace() {
	if ! dtrace -ae > boot-trace.out 2> boot-trace.err; then
		cat boot-trace.err
		info "no anonymous trace of this boot (run the arm phase before the reboot)"
		return 0
	fi
	step cat boot-trace.out
	# "TIME exec ttymon pid PID ppid PPID: ARGS"
	tt=$(awk '$2 == "exec" && /ttymon -g / { print $5; exit }' boot-trace.out)
	[ -n "$tt" ] && pass "the boot trace saw console-login's ttymon -g (pid $tt)" ||
	    bad "the boot trace has no ttymon -g"
	# "TIME open PATH by ttymon pid PID ppid PPID"
	topen=$(awk -v p="$tt" '$2 == "open" && $7 == p { print $1; exit }' boot-trace.out)
	[ -n "$topen" ] && pass "console-login's ttymon read /etc/logindevperm at $topen" ||
	    bad "console-login's ttymon did not read /etc/logindevperm during the boot"
	# "TIME devfs_set... NODE by EXEC pid PID ppid PPID"
	n=$(awk '$2 ~ /^devfs_set/ && $5 == "ttymon"' boot-trace.out | grep -c .)
	[ "$n" -eq 0 ] && pass "ttymon changed no DRM node during the boot" ||
	    bad "ttymon changed DRM nodes $n times during the boot"
	trn=$(awk '$2 == "rename" { print $1 }' boot-trace.out | tail -1)
	if [ -z "$trn" ]; then
		info "nothing renamed onto /etc/logindevperm during the boot (the rule was restricted before it)"
	elif [ -n "$topen" ] && awk -v a="$trn" -v b="$topen" 'BEGIN { exit !(a + 0 < b + 0) }'; then
		pass "drm-perms replaced /etc/logindevperm ($trn) before ttymon read it ($topen)"
	else
		bad "the rename onto /etc/logindevperm ($trn) did not come before ttymon read it (${topen:-never})"
	fi
	step dtrace -A
	step bootadm update-archive
}

phase_arm() {
	echo "=== arm: anonymous DTrace for the next boot"
	if [ "${REVERT:-0}" = 1 ]; then
		# The packaged /etc/logindevperm, with its rule for every driver,
		# as on the first boot after an installation.
		step pkg revert /etc/logindevperm || bad "pkg revert /etc/logindevperm failed"
		step grep -n /dev/dri /etc/logindevperm
		rules | grep -q '^all ' && pass "/etc/logindevperm has the packaged rule for every driver again" ||
		    bad "pkg revert did not bring back the rule for every driver"
	fi
	echo "$BOOT_D" > g10-boot.d
	step dtrace -A -s g10-boot.d || { bad "dtrace -A failed"; return 1; }
	step bootadm update-archive
	pass "anonymous tracing armed for the next boot (reboot now)"
}

phase_coldboot() {
	echo "=== coldboot: the DRM nodes after a cold boot"
	step uptime
	bt=$(kstat -p unix:0:system_misc:boot_time | awk '{ print $2 }')
	step sh -c "svcs -o state,stime,fmri $SVC $CONSOLE svc:/system/device/local:default"
	[ "$(svcs -H -o state $SVC 2>/dev/null)" = online ] && pass "$SVC is online" ||
	    bad "$SVC is not online"
	step sh -c "svcs -d $CONSOLE"
	svcs -H -d "$CONSOLE" | grep -q "$SVC" && pass "console-login depends on drm-perms" ||
	    bad "console-login does not depend on drm-perms"
	t1=$(svcprop -p restarter/state_timestamp "$SVC" 2>/dev/null)
	t2=$(svcprop -p restarter/start_method_timestamp "$CONSOLE" 2>/dev/null)
	echo "boot at $bt, drm-perms online at $t1, console-login's start method at $t2"
	# Only the boot's own start of console-login tells the order; a
	# restart since (driver-filter, a logout) would always come later.
	if [ -z "$t2" ] || awk -v b="$bt" -v c="$t2" 'BEGIN { exit !(c - b > 300) }'; then
		bad "console-login started more than 300 s after the boot (restarted since?): run coldboot right after a boot"
	elif [ -n "$t1" ] && awk -v a="$t1" -v b="$bt" -v c="$t2" 'BEGIN { exit !(a >= b && a <= c) }'; then
		pass "drm-perms came online in this boot before console-login started ttymon"
	else
		bad "drm-perms did not come online in this boot before console-login started"
	fi
	log=$(svcs -L "$SVC" 2>/dev/null)
	[ -n "$log" ] && step tail -20 "$log"
	check_boot_trace
	check_rules
	check_nodes "after the boot, before any open"
	check_opens
	check_logout
	check_nodes "after the console-login restart"
	step "$DRMADM" status
	"$DRMADM" status > drmadm-status.out 2>&1
	listed=0 want=0
	for l in /dev/dri/*; do
		[ -h "$l" ] || continue
		major=$(ls -lLn "$l" 2>/dev/null | awk '{ print $5 }')
		new_driver "$(driver_of_major "${major%%,*}")" || continue
		want=$((want + 1))
		grep -q "^  $l .* 0666 root sys\$" drmadm-status.out && listed=$((listed + 1)) ||
		    bad "drmadm status does not list $l as 0666 root sys without a /etc/minor_perm difference"
	done
	[ $want -gt 0 ] && [ $listed -eq $want ] &&
	    pass "drmadm status lists all $listed new-stack nodes at their /etc/minor_perm values" ||
	    bad "drmadm status lists $listed of $want new-stack nodes at their /etc/minor_perm values"
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

# ldp_case NAME: copies ldp-in to the scratch root R (0640 root bin) and
# runs drmadm -R R perms there; the output is ldp-NAME.out, the exit
# status ldp_rc.
ldp_case() {
	rm -rf "$R"
	mkdir -p "$R/etc"
	cp ldp-in "$R/etc/logindevperm"
	chown root:bin "$R/etc/logindevperm"
	chmod 0640 "$R/etc/logindevperm"
	"$DRMADM" -R "$R" perms > "ldp-$1.out" 2>&1
	ldp_rc=$?
	cat "ldp-$1.out"
	echo "[drmadm -R perms exit $ldp_rc]"
}

# temp_left: the temporary files of drmadm perms left in R/etc.
temp_left() {
	ls -a "$R/etc" | grep '^\.logindevperm'
}

phase_rewrite() {
	echo "=== rewrite: drmadm perms on /etc/logindevperm of scratch roots"
	R=$PWD/ldp-root
	tab=$(printf '\t')
	cat > ldp-in <<EOT
# comment line
/dev/vt/console_user${tab}0600${tab}/dev/fbs/*${tab}${tab}# frame buffers
/dev/vt/console_user${tab}0600${tab}/dev/dri/*${tab}${tab}# dri devices
/dev/console${tab}0600${tab}/dev/sound/*:/dev/dri/card[0-9]+:/dev/mouse${tab}# mixed
/dev/console${tab}0600${tab}/dev/dri/*${tab}driver=cdrmnull${tab}# admin
/dev/console${tab}0600${tab}/dev/*/*${tab}# pattern
EOT
	step cat ldp-in
	ldp_case default
	cp "$R/etc/logindevperm" ldp-first
	step cat ldp-first
	[ $ldp_rc -eq 0 ] && pass "drmadm -R perms exits 0" || bad "drmadm -R perms exits $ldp_rc"
	r=$(rules ldp-first)
	echo "$r"
	[ "$(echo "$r" | grep -c '^legacy ')" -eq 2 ] && ! echo "$r" | grep -q '^all ' &&
	    pass "both rules for every driver now name the legacy drivers" ||
	    bad "the rules for every driver were not both restricted"
	grep -q "^/dev/vt/console_user${tab}0600${tab}/dev/dri/\*${tab}driver=i915,radeon\$" ldp-first &&
	    pass "the packaged rule became \"/dev/vt/console_user 0600 /dev/dri/* driver=i915,radeon\"" ||
	    bad "the packaged rule was not rewritten as expected"
	grep -q "^/dev/console${tab}0600${tab}/dev/sound/\*:/dev/mouse${tab}# mixed\$" ldp-first &&
	    grep -q "^/dev/console${tab}0600${tab}/dev/dri/card\[0-9\]+${tab}driver=i915,radeon\$" ldp-first &&
	    pass "a mixed rule keeps its other devices for every driver; its /dev/dri device moved to a restricted rule" ||
	    bad "the mixed rule was not split as expected"
	[ "$(grep -c '^# drmadm(8): the next rule applies to the legacy DRM drivers only$' ldp-first)" -eq 2 ] &&
	    pass "each restricted rule follows a drmadm comment line" || bad "the drmadm comment lines are missing"
	grep -q '^# dri devices$' ldp-first && pass "the rule's own comment is kept on a line of its own" ||
	    bad "the rule's own comment is lost"
	grep -q "^/dev/console${tab}0600${tab}/dev/dri/\*${tab}driver=cdrmnull${tab}# admin\$" ldp-first &&
	    grep -q "Left as written: .*:[0-9]* names a driver of the new stack" ldp-default.out &&
	    pass "a rule naming a new-stack driver is left as written and reported" ||
	    bad "the rule naming cdrmnull was changed or not reported"
	grep -q "^/dev/console${tab}0600${tab}/dev/\*/\*${tab}# pattern\$" ldp-first &&
	    grep -q "Warning: .*:[0-9]* reaches /dev/dri through a pattern" ldp-default.out &&
	    pass "a pattern rule reaching /dev/dri is left and reported" ||
	    bad "the pattern rule was changed or not reported"
	head -1 ldp-first | grep -q '^# comment line$' &&
	    grep -q "^/dev/vt/console_user${tab}0600${tab}/dev/fbs/\*${tab}${tab}# frame buffers\$" ldp-first &&
	    pass "the other lines are unchanged" || bad "other lines changed"
	# shellcheck disable=SC2046 # the fields of ls -ln
	set -- $(ls -ln "$R/etc/logindevperm")
	[ "$1 $3 $4" = "-rw-r----- 0 2" ] && pass "the file keeps its owner, group and mode (0640 root bin)" ||
	    bad "the file is now $1 uid $3 gid $4"
	"$DRMADM" -R "$R" perms > ldp-second.out 2>&1
	cat ldp-second.out
	cmp -s ldp-first "$R/etc/logindevperm" && grep -q "no /dev/dri rule applies to every driver" ldp-second.out &&
	    pass "a second run changes nothing" || bad "a second run changed the file"
	[ -z "$(temp_left)" ] && pass "no temporary file is left" || bad "temporary files are left: $(temp_left)"

	echo
	echo "--- 20 runs at once on one file"
	ldp_case parallel-setup > /dev/null
	cp ldp-in "$R/etc/logindevperm"
	i=0
	while [ $i -lt 20 ]; do
		"$DRMADM" -R "$R" perms > "ldp-par-$i.out" 2>&1 &
		i=$((i + 1))
	done
	wait
	cmp -s ldp-first "$R/etc/logindevperm" && pass "20 concurrent runs leave the same file as one run" ||
	    { diff ldp-first "$R/etc/logindevperm"; bad "20 concurrent runs left another file"; }
	[ -z "$(temp_left)" ] && pass "and no temporary file" ||
	    bad "temporary files are left after the concurrent runs: $(temp_left)"
	n=$(cat ldp-par-*.out | grep -c '^drmadm: ')
	[ "$n" -eq 0 ] && pass "none of the 20 runs failed" ||
	    bad "$n of the concurrent runs failed: $(cat ldp-par-*.out | grep '^drmadm: ' | head -2)"

	echo
	echo "--- a rule that would get longer than libdevinfo reads"
	e=$(i=0; while [ $i -lt 14 ]; do printf '/dev/dri/card%s:' $((i + 10)); i=$((i + 1)); done)
	printf '/dev/vt/console_user\t0600\t%s\n' "${e%:}" > ldp-in
	awk '{ print "input line of", length($0), "bytes" }' ldp-in
	ldp_case long
	[ $ldp_rc -ne 0 ] && cmp -s ldp-in "$R/etc/logindevperm" &&
	    pass "drmadm refuses (exit $ldp_rc) and leaves the file unchanged" ||
	    bad "drmadm rewrote a rule into a line longer than libdevinfo reads"
	[ -z "$(temp_left)" ] && pass "no temporary file is left after the refusal" ||
	    bad "a temporary file is left after the refusal"
	rm -rf "$R"
}

phase_minorperm() {
	echo "=== minorperm: drmadm's /etc/minor_perm choice is the kernel's"
	# Two matching patterns for D's (default virtio_gpu) card node: the
	# kernel takes the later one (add_minorperm() inserts at the head).
	# update_drv -m makes the kernel apply minor_perm to nodes without a
	# shadow at once; drmadm status must then report no difference. D's
	# nodes must never have been changed with chmod (no shadow).
	d=${D:-virtio_gpu}
	c=$(dri_link "$(device_prefix "$d")" card) || { bad "$d has no card node"; return 1; }
	step grep "^$d:" /etc/minor_perm
	step update_drv -a -m 'card* 0640 root sys' "$d" || bad "update_drv -a -m 'card*' failed"
	step update_drv -a -m 'card? 0644 root sys' "$d" || bad "update_drv -a -m 'card?' failed"
	step grep "^$d:" /etc/minor_perm
	sleep 1
	step ls -lL /dev/dri
	"$DRMADM" status > drmadm-status.out 2>&1
	step sed -n '/^Device node permissions/,/^$/p' drmadm-status.out
	if ls -lL "$c" | grep -q '^crw-r--r--'; then
		pass "the kernel gave $c the later pattern's mode 0644"
		grep -q "^  $c .* 0644 root sys\$" drmadm-status.out &&
		    pass "drmadm status expects the same and reports no difference" ||
		    bad "drmadm status reports a difference or another mode for $c"
	else
		bad "the kernel did not apply 0644 to $c (a shadow node?): $(ls -lL "$c")"
	fi
	step update_drv -d -m 'card? 0644 root sys' "$d"
	step update_drv -d -m 'card* 0640 root sys' "$d"
	step grep "^$d:" /etc/minor_perm
	sleep 1
	ls -lL "$c" | grep -q '^crw-rw-rw-' && pass "$c is back at 0666 after the entries are removed" ||
	    bad "$c is $(ls -lL "$c") after the entries are removed"
}

phase_repair() {
	echo "=== repair: nodes the packaged rule left at 0600 root root"
	# Reproduces the state of a machine that booted with the packaged rule
	# before drm-perms existed (the G7 guest): the packaged
	# /etc/logindevperm back, and a console-login restart, whose ttymon
	# applies its rule to every /dev/dri node; then drm-perms runs
	# (svcadm restart), as it does at the next boot or when the package
	# is installed, and must restrict the rule and reset the nodes.
	step pkg revert /etc/logindevperm || bad "pkg revert /etc/logindevperm failed"
	rules | grep -q '^all ' && pass "/etc/logindevperm has the packaged rule for every driver again" ||
	    bad "pkg revert did not bring back the rule for every driver"
	restart_console
	step ls -lL /dev/dri
	n=$(grep -c devfs_set logout.d.out)
	[ "$n" -gt 0 ] && pass "with the packaged rule, ttymon changed DRM nodes ($n changes): the fault is reproduced" ||
	    bad "ttymon changed no DRM node with the packaged rule: the fault is not reproduced"
	for d in $EXPECT; do
		a=$(node_attrs "$d")
		[ "$a" = "crw------- 0 0" ] && pass "$d's nodes are 0600 root root now" ||
		    bad "$d's nodes are $a, not 0600 root root"
	done
	t0=$(svcprop -p restarter/state_timestamp "$SVC")
	step svcadm restart "$SVC"
	i=0
	while [ $i -lt 30 ]; do
		[ "$(svcs -H -o state $SVC)" = online ] &&
		    [ "$(svcprop -p restarter/state_timestamp $SVC)" != "$t0" ] && break
		sleep 1
		i=$((i + 1))
	done
	log=$(svcs -L "$SVC" 2>/dev/null)
	[ -n "$log" ] && step tail -12 "$log"
	check_rules
	check_nodes "after drm-perms ran"
	check_opens
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
arm) phase_arm ;;
coldboot) phase_coldboot ;;
rewrite) phase_rewrite ;;
minorperm) phase_minorperm ;;
repair) phase_repair ;;
driver-filter) phase_driver_filter ;;
upgrade) phase_upgrade ;;
*) echo "usage: run-g10-test.sh arm|coldboot|rewrite|minorperm|repair|driver-filter|upgrade" >&2
   exit 2 ;;
esac
echo
echo "checks passed: $npass"
[ $fail -eq 0 ] && echo "run-g10-test $1: PASS" || echo "run-g10-test $1: FAIL"
exit $fail
