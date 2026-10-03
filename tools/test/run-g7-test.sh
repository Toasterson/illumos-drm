#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# G7: the legacy DRM stack of gfx-drm (misc/drm, i915; package
# driver/graphics/drm) and this repository's stack (misc/drm_core and its
# drivers) installed on one machine, never loaded together, and selected
# with drmadm(8). The test spans reboots, so it runs in phases, one per
# boot, in this order:
#
#   baseline       on the golden image (OpenIndiana's driver/graphics/drm
#                  without the reverse guard): misc/drm_core refuses beside
#                  the legacy misc/drm, but the legacy misc/drm still loads
#                  beside misc/drm_core; and the old SUNW_drm_link leaves a
#                  dangling /dev/dri/card<N> after devfsadm -C.
#   install        publishes drm-core, drmadm, virtio-gpu and cdrmnull from
#                  this directory and installs them with gfx-drm's packages
#                  (driver/graphics/drm with the reverse guard,
#                  drm-devlink, libdrm 2.4.134) into a new boot environment
#                  g7 (GFXDRM=<p5p or repository>). Reboot afterwards.
#   guards         each core refuses to load while the other is loaded,
#                  explicit paths and dependent drivers included, and
#                  RACES pairs of concurrent loads never leave both loaded.
#   select-new     drmadm refuses while a DRM module is loaded and while
#                  its lock is held, then selects the new stack. Reboot.
#   after-new      the legacy modules are excluded; the packaged drivers
#                  attach and libdrm lists only them.
#   select-legacy  drmadm selects the legacy stack. Reboot.
#   after-legacy   the new modules are excluded, the legacy core loads,
#                  devfsadm -C removes the new stack's stale /dev/dri links
#                  and libdrm lists no device.
#   select-root    drmadm -R on a mounted clone of the boot environment.
#
# Then "drmadm select new", a reboot, "pkg uninstall virtio-gpu cdrmnull"
# (their drivers would collide with the suites' own add_drv) and
# run-r5a-test.sh with LEGACY_GUARD=skip run the R0 to R5a suites on the
# selected new stack.
#
# Run as root in a directory that holds drm-test-lib.sh, drm_module.a,
# drm_core.mapfile, drm_facade_ctf.o (optional), virtio_gpu.a, the cdrmnull
# module (drivers/cdrmnull/build.sh) and cdrmnull.conf, drmadm and drmadm.8
# (tools/drmadm), packaging/*.p5m as *.p5m, packaging/xorg.conf.d and
# LICENSE. The machine needs a virtio-gpu device (pci1af4,1050), gcc and,
# from the install phase on, libdrm 2.4.134 with its tests
# (/opt/drm-tests/amd64). Use a disposable VM.
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

FRAG=/etc/system.d/driver:graphics:drmadm
T=/opt/drm-tests/amd64
LEGACY_MSG='drm: the DRM core misc/drm_core is loaded'
NEW_MSG='drm_core: .*misc/drm.* is loaded'
DRMADM=${DRMADM:-/usr/sbin/drmadm}

modid() {
	modinfo | awk -v m="$1" '$6 == m {print $1}'
}

loaded() {
	[ -n "$(modid "$1")" ]
}

# unload_all: every DRM driver, then the cores.
unload_all() {
	for m in cdrmnull rdrmnull virtio_gpu i915n i915 drm_core drm; do
		id=$(modid $m)
		[ -n "$id" ] && step modunload -i "$id"
	done
	for m in cdrmnull rdrmnull virtio_gpu i915n i915 drm_core drm; do
		loaded $m && { bad "$m stays loaded"; return 1; }
	done
	return 0
}

# msgs RE: how many console messages match RE. The kernel's message
# buffer, not /var/adm/messages, whose syslogd folds repeated messages
# into "last message repeated N times".
msgs() {
	echo ::msgbuf | mdb -k | grep -c "$1"
}

# wait_msg RE N: waits up to 5 s until more than N messages match RE.
wait_msg() {
	i=0
	while [ "$(msgs "$1")" -le "$2" ] && [ $i -lt 10 ]; do
		sleep 0.5
		i=$((i + 1))
	done
}

# bits FILE: the BITS column of mdb's ::modctl for the module record whose
# file name ends in /FILE: "l" in memory, "i" installed, "p" primary.
bits() {
	echo ::modctl | mdb -k | awk -v f="$1" '$NF == f || $NF ~ ("/" f "$") {
	    b = $3; if (b ~ /^0x/) b = "-"; print b; exit }'
}

# refused WHAT MSG -- COMMAND: COMMAND fails and logs MSG once more.
refused() {
	what=$1 re=$2
	shift 3
	n=$(msgs "$re")
	if step "$@"; then
		bad "$what: loaded"
		return 1
	fi
	wait_msg "$re" "$n"
	if [ "$(msgs "$re")" -gt "$n" ]; then
		echo ::msgbuf | mdb -k | grep "$re" | tail -1
		pass "$what: refused with the guard's warning"
	else
		echo ::msgbuf | mdb -k | tail -3
		bad "$what: failed without the guard's warning"
	fi
}

# excluded WHAT -- COMMAND: COMMAND fails and neither guard logs anything
# (the module framework refuses before _init).
excluded() {
	what=$1
	shift 2
	n1=$(msgs "$LEGACY_MSG") n2=$(msgs "$NEW_MSG")
	if step "$@"; then
		bad "$what: loaded although excluded"
		return 1
	fi
	sleep 1
	if [ "$(msgs "$LEGACY_MSG")" -eq "$n1" ] && [ "$(msgs "$NEW_MSG")" -eq "$n2" ]; then
		pass "$what: not loaded (excluded; no guard ran)"
	else
		bad "$what: a guard ran, so the module was not excluded"
	fi
}

status() {
	step "$DRMADM" status | tee drmadm-status.out
}

i915_aliases() {
	awk '$1 == "i915"' /etc/driver_aliases | wc -l | tr -d ' '
}

# devices N: drmdevice (libdrm 2.4.134) lists N devices.
devices() {
	step ls -l /dev/dri
	"$T/drmdevice" > drmdevice.out 2>&1
	rc=$?
	grep -E 'Devices reported|available_nodes|faux|pci [0-9]' drmdevice.out | head -20
	echo "[drmdevice exit $rc]"
	got=$(sed -n 's/.*Devices reported \([0-9]*\).*/\1/p' drmdevice.out | head -1)
	[ -z "$got" ] && got=0
	if [ "$got" -eq "$1" ]; then
		pass "libdrm lists $1 device(s) $2"
	else
		cat drmdevice.out
		bad "libdrm lists $got device(s), not $1, $2"
	fi
}

# fragment_excludes MODULES: the fragment excludes exactly MODULES, each
# as class/name and as name.
fragment_excludes() {
	want=$(for m in $1; do echo "exclude: $m"; echo "exclude: ${m#*/}"; done | sort)
	have=$(grep '^exclude:' "$FRAG" | sort)
	if [ "$want" = "$have" ]; then
		pass "$FRAG excludes $1"
	else
		echo "have: $have"
		echo "want: $want"
		bad "$FRAG excludes something else"
	fi
}

selected() {
	if grep -q "^Selected stack: $1, $2" drmadm-status.out; then
		pass "drmadm status: $1, $2"
	else
		bad "drmadm status does not say \"$1, $2\""
	fi
}

LEGACY="misc/drm drv/i915 drv/radeon"
NEW="misc/drm_core drv/i915n drv/virtio_gpu drv/cdrmnull drv/rdrmnull"

phase_baseline() {
	echo "=== baseline: OpenIndiana's driver/graphics/drm (no reverse guard)"
	step pkg list -v driver/graphics/drm
	unload_all
	drm_core_install || exit 1
	unload_all || exit 1
	step modload /kernel/misc/amd64/drm || bad "the legacy misc/drm does not load"
	refused "misc/drm_core beside the legacy misc/drm" "$NEW_MSG" -- modload -p misc/drm_core
	unload_all
	step modload -p misc/drm_core
	if step modload /kernel/misc/amd64/drm; then
		info "the legacy misc/drm without the reverse guard loads beside misc/drm_core"
		step sh -c 'modinfo | grep -E " drm | drm_core "'
	else
		info "the legacy misc/drm did not load beside misc/drm_core"
	fi
	unload_all
	echo
	echo "=== baseline: OpenIndiana's SUNW_drm_link removal rule"
	step cp cdrmnull /usr/kernel/drv/amd64/cdrmnull || exit 1
	step cp cdrmnull.conf /usr/kernel/drv/cdrmnull.conf || exit 1
	step add_drv -m '* 0666 root sys' cdrmnull || exit 1
	l=$(dri_link cdrmnull@ card) || bad "no /dev/dri link for cdrmnull"
	step rem_drv cdrmnull
	step rm -f /usr/kernel/drv/amd64/cdrmnull /usr/kernel/drv/cdrmnull.conf
	step devfsadm -C
	if [ -n "${l:-}" ] && [ -h "$l" ]; then
		step ls -l "$l"
		info "devfsadm -C leaves the dangling $l with the old removal rule"
		remove_dri_links cdrmnull@
	else
		info "devfsadm -C removed ${l:-the link}"
	fi
	drm_core_remove
}

phase_install() {
	echo "=== install: both stacks into a new boot environment g7"
	: "${GFXDRM:?set GFXDRM to the gfx-drm p5p or repository with the reverse guard}"
	P=$PWD/proto
	R=$PWD/repo
	rm -rf "$P" "$R"
	mkdir -p "$P/kernel/misc/amd64" "$P/kernel/drv/amd64" "$P/usr/kernel/drv/amd64" \
	    "$P/usr/sbin" "$P/usr/share/man/man8" "$P/etc/X11/xorg.conf.d"
	ctf=
	[ -f drm_facade_ctf.o ] && ctf="-z defaultextract drm_facade_ctf.o"
	# shellcheck disable=SC2086 # $ctf is empty or two words
	step ld -ztype=kmod -N fs/specfs -B reduce -M drm_core.mapfile -z allextract drm_module.a $ctf \
	    -o "$P/kernel/misc/amd64/drm_core" || exit 1
	step ld -ztype=kmod -N misc/virtio -N fs/specfs -N misc/drm_core -z allextract virtio_gpu.a \
	    -o "$P/kernel/drv/amd64/virtio_gpu" || exit 1
	step cp cdrmnull "$P/usr/kernel/drv/amd64/cdrmnull" || exit 1
	step cp cdrmnull.conf "$P/usr/kernel/drv/cdrmnull.conf" || exit 1
	step cp drmadm "$P/usr/sbin/drmadm" || exit 1
	step cp drmadm.8 "$P/usr/share/man/man8/drmadm.8" || exit 1
	step cp xorg.conf.d/20-virtio-gpu.conf "$P/etc/X11/xorg.conf.d/" || exit 1
	step pkgrepo create "$R" || exit 1
	step pkgrepo -s "$R" set publisher/prefix=illumos-drm || exit 1
	for m in drmadm drm-core virtio-gpu cdrmnull; do
		step pkgsend publish -s "$R" -d "$P" -d . "$m.p5m" || exit 1
	done
	step pkgrepo -s "$R" list
	step pkg set-publisher -g "file://$R" illumos-drm || exit 1
	step pkg set-publisher -g "$GFXDRM" gfx-drm || exit 1
	step pkg set-publisher --non-sticky openindiana.org || exit 1
	step pkg install --be-name g7 --reject entire --reject userland-incorporation \
	    pkg://gfx-drm/driver/graphics/drm pkg://gfx-drm/driver/graphics/drm-devlink \
	    pkg://gfx-drm/driver/graphics/agpgart pkg://gfx-drm/x11/library/libdrm \
	    pkg://gfx-drm/system/test/libdrm pkg://gfx-drm/system/header/header-drm \
	    driver/graphics/drm-core driver/graphics/drmadm driver/graphics/virtio-gpu \
	    driver/graphics/cdrmnull || exit 1
	step beadm list
	pass "both stacks installed into the boot environment g7 (activated)"
}

phase_guards() {
	echo "=== guards: each core refuses while the other is loaded"
	step pkg list -v driver/graphics/drm driver/graphics/drm-devlink driver/graphics/drm-core \
	    driver/graphics/drmadm driver/graphics/virtio-gpu driver/graphics/cdrmnull x11/library/libdrm
	status
	grep -q '^Selected stack: none' drmadm-status.out && pass "drmadm status: nothing selected" ||
	    bad "drmadm status: a stack is selected after the installation"
	unload_all || exit 1
	echo
	echo "--- legacy first"
	step modload -p misc/drm && pass "the legacy misc/drm loads" || bad "the legacy misc/drm does not load"
	refused "misc/drm_core while misc/drm is loaded" "$NEW_MSG" -- modload -p misc/drm_core
	refused "drv/virtio_gpu (needs misc/drm_core) while misc/drm is loaded" "$NEW_MSG" -- \
	    modload -p drv/virtio_gpu
	refused "drv/cdrmnull (needs misc/drm_core) while misc/drm is loaded" "$NEW_MSG" -- \
	    modload -p drv/cdrmnull
	loaded drm_core && bad "misc/drm_core is loaded" || pass "misc/drm_core is not loaded"
	unload_all || exit 1
	echo
	echo "--- new first"
	step modload -p misc/drm_core && pass "misc/drm_core loads" || bad "misc/drm_core does not load"
	refused "misc/drm while misc/drm_core is loaded" "$LEGACY_MSG" -- modload -p misc/drm
	refused "/kernel/misc/amd64/drm (an explicit path) while misc/drm_core is loaded" "$LEGACY_MSG" -- \
	    modload /kernel/misc/amd64/drm
	refused "drv/i915 (needs misc/drm) while misc/drm_core is loaded" "$LEGACY_MSG" -- \
	    modload -p drv/i915
	loaded drm && bad "the legacy misc/drm is loaded" || pass "the legacy misc/drm is not loaded"
	loaded i915 && bad "i915 is loaded" || pass "i915 is not loaded"
	unload_all || exit 1
	echo
	echo "--- a core left in memory by a refused dependency does not count"
	# A dependency whose _init fails stays in memory, not installed and not
	# busy (mod_install_requisites does not unload it, modunload cannot),
	# until it is loaded again. The refused drv/i915 above left misc/drm so.
	step sh -c 'echo ::modctl | mdb -k | grep -E "/drm(_core)?$"'
	b=$(bits drm)
	info "misc/drm after the refused drv/i915: bits \"$b\" (l: in memory, i: installed)"
	[ "$b" = l ] && pass "the refused drv/i915 left misc/drm in memory, not installed" ||
	    info "misc/drm is not left in memory (bits \"$b\"): the next check is not exercised"
	step modload -p misc/drm_core && pass "misc/drm_core loads beside a refused, uninstalled misc/drm" ||
	    bad "misc/drm_core does not load beside a refused, uninstalled misc/drm"
	unload_all || exit 1
	step modload -p misc/drm || bad "the legacy misc/drm does not load"
	refused "drv/virtio_gpu while misc/drm is loaded" "$NEW_MSG" -- modload -p drv/virtio_gpu
	unload_all || exit 1
	step sh -c 'echo ::modctl | mdb -k | grep -E "/drm(_core)?$"'
	b=$(bits drm_core)
	info "misc/drm_core after the refused drv/virtio_gpu: bits \"$b\""
	[ "$b" = l ] && pass "the refused drv/virtio_gpu left misc/drm_core in memory, not installed" ||
	    info "misc/drm_core is not left in memory (bits \"$b\"): the next check is not exercised"
	step modload -p misc/drm && pass "misc/drm loads beside a refused, uninstalled misc/drm_core" ||
	    bad "misc/drm does not load beside a refused, uninstalled misc/drm_core"
	unload_all || exit 1
	echo
	echo "--- the drivers bring their core, and the other core refuses"
	step devfsadm -i virtio_gpu
	step devfsadm -i cdrmnull
	step sh -c 'modinfo | grep -E " (drm|drm_core|virtio_gpu|cdrmnull|i915) "'
	loaded virtio_gpu && loaded cdrmnull && pass "virtio_gpu and cdrmnull attach (misc/drm_core loaded)" ||
	    bad "the packaged drivers do not attach"
	refused "misc/drm while the new stack's drivers are attached" "$LEGACY_MSG" -- modload -p misc/drm
	devices 2 "while the new stack is attached"
	unload_all || exit 1
	echo
	echo "--- $RACES concurrent loads of both cores"
	# misc/drm_core (16 MB with its debug information) takes longer to
	# load than misc/drm, so the second load starts after a delay that
	# sweeps 0 to 98 ms, alternating which core starts first, to make the
	# two _init calls meet.
	n1=$(msgs "$LEGACY_MSG") n2=$(msgs "$NEW_MSG")
	both=0 legacy=0 new=0 neither=0 i=0
	while [ $i -lt "$RACES" ]; do
		d=$(awk -v i=$i 'BEGIN { printf "%.3f", (int(i / 2) % 50) * 0.002 }')
		if [ $((i % 2)) -eq 0 ]; then
			modload -p misc/drm_core 2>/dev/null & sleep "$d"; modload -p misc/drm 2>/dev/null &
		else
			modload -p misc/drm 2>/dev/null & sleep "$d"; modload -p misc/drm_core 2>/dev/null &
		fi
		wait
		l=no n=no
		loaded drm && l=yes
		loaded drm_core && n=yes
		case $l$n in
		yesyes) both=$((both + 1)); modinfo | grep -E ' (drm|drm_core) ' ;;
		yesno) legacy=$((legacy + 1)) ;;
		noyes) new=$((new + 1)) ;;
		nono) neither=$((neither + 1)) ;;
		esac
		for m in drm drm_core; do
			id=$(modid $m)
			[ -n "$id" ] && modunload -i "$id"
		done
		i=$((i + 1))
	done
	echo "$RACES races: legacy won $legacy, new won $new, neither $neither, both $both"
	echo "warnings: legacy guard $(($(msgs "$LEGACY_MSG") - n1)), new guard $(($(msgs "$NEW_MSG") - n2))"
	[ $both -eq 0 ] && pass "no race left both cores loaded ($RACES races)" ||
	    bad "$both of $RACES races left both cores loaded"
	[ $legacy -gt 0 ] && [ $new -gt 0 ] && pass "races were won by both cores ($legacy legacy, $new new)" ||
	    bad "the races were not exercised: $legacy won by the legacy core, $new by the new one"
	unload_all
}

phase_select_new() {
	echo "=== select-new"
	status
	rm -f frag.before
	[ -f "$FRAG" ] && cp "$FRAG" frag.before
	step cp /etc/driver_aliases driver_aliases.before-new
	step modload -p misc/drm_core
	if step "$DRMADM" select new; then
		bad "drmadm select new ran while misc/drm_core was loaded"
	else
		pass "drmadm select refuses while a DRM module is loaded"
	fi
	if { [ -f frag.before ] && cmp -s frag.before "$FRAG"; } || { [ ! -f frag.before ] && [ ! -f "$FRAG" ]; }; then
		cmp -s driver_aliases.before-new /etc/driver_aliases && pass "a refused select changes nothing" ||
		    bad "a refused select changed /etc/driver_aliases"
	else
		bad "a refused select changed $FRAG"
	fi
	unload_all || exit 1
	step mkdir /etc/system.d/.drmadm.lock
	if step "$DRMADM" select new; then
		bad "drmadm select new ran while its lock was held"
	else
		pass "drmadm select refuses while another drmadm holds the lock"
	fi
	step rmdir /etc/system.d/.drmadm.lock
	before=$(i915_aliases)
	echo "i915 has $before aliases"
	[ "$before" -gt 0 ] && pass "i915 has its $before aliases before the selection" ||
	    bad "i915 has no aliases before the selection"
	step "$DRMADM" select new | tee drmadm-select-new.out
	[ "$(i915_aliases)" -eq 0 ] && pass "i915 has no alias after select new" ||
	    bad "i915 keeps $(i915_aliases) aliases"
	grep -q '^i915n ' /etc/name_to_major && bad "i915n is installed" ||
	    pass "i915n is not installed: the aliases stay unbound"
	grep -q 'i915n is not installed' drmadm-select-new.out && pass "drmadm says the GPU stays unbound" ||
	    bad "drmadm does not say that the GPU stays unbound"
	gone=$(diff driver_aliases.before-new /etc/driver_aliases | grep -c '^<')
	added=$(diff driver_aliases.before-new /etc/driver_aliases | grep -c '^>')
	[ "$gone" -eq "$before" ] && [ "$added" -eq 0 ] &&
	    pass "exactly the $before i915 aliases left driver_aliases" ||
	    bad "$gone lines left driver_aliases and $added came, not $before and 0"
	step cat "$FRAG"
	fragment_excludes "$LEGACY"
	[ "$(grep -c '^\* drmadm-alias: ' "$FRAG")" -eq "$before" ] &&
	    pass "the fragment records the $before aliases" || bad "the fragment records another alias set"
	if grep -q 'exclude: misc/drm$' /etc/system.d/.self-assembly 2>/dev/null; then
		pass "bootadm assembled the fragment into /etc/system.d/.self-assembly"
	else
		bad "the fragment is not in /etc/system.d/.self-assembly"
	fi
	status
	selected new "from the next boot"
	step cp "$FRAG" frag.first
	step "$DRMADM" select new | tee drmadm-select-new2.out
	grep -q 'Nothing changed' drmadm-select-new2.out && cmp -s frag.first "$FRAG" &&
	    pass "a second select new changes nothing" || bad "a second select new changed something"
	step pkg verify driver/graphics/drm
	info "pkg verify driver/graphics/drm reports the moved i915 aliases (above)"
}

phase_after_new() {
	echo "=== after-new: the boot with the new stack selected"
	status
	selected new "since the last boot"
	loaded drm || loaded i915 && bad "a legacy module is loaded" || pass "no legacy module is loaded"
	excluded "misc/drm" -- modload -p misc/drm
	excluded "/kernel/misc/amd64/drm" -- modload /kernel/misc/amd64/drm
	excluded "drv/i915" -- modload -p drv/i915
	step devfsadm -i virtio_gpu
	step devfsadm -i cdrmnull
	loaded virtio_gpu && loaded cdrmnull && pass "the new stack's packaged drivers attach" ||
	    bad "the new stack's drivers do not attach"
	devices 2 "with the new stack selected"
	step ls -l /dev/dri
	ls /dev/dri > dri.after-new
}

phase_select_legacy() {
	echo "=== select-legacy"
	unload_all || exit 1
	ls -l /dev/dri
	step "$DRMADM" select legacy | tee drmadm-select-legacy.out
	n=$(i915_aliases)
	m=$(pkg contents -m driver/graphics/drm | awk '$1 == "driver"' | tr ' ' '\n' | grep -c '^alias=')
	[ "$n" -eq "$m" ] && pass "i915 has the $m aliases of driver/graphics/drm again" ||
	    bad "i915 has $n aliases, the package declares $m"
	step pkg verify driver/graphics/drm && pass "pkg verify driver/graphics/drm passes again" ||
	    bad "pkg verify driver/graphics/drm fails after select legacy"
	step cat "$FRAG"
	fragment_excludes "$NEW"
	status
	selected legacy "from the next boot"
}

phase_after_legacy() {
	echo "=== after-legacy: the boot with the legacy stack selected"
	status
	selected legacy "since the last boot"
	for m in drm_core virtio_gpu cdrmnull; do
		loaded $m && bad "$m is loaded" || pass "$m is not loaded"
	done
	excluded "misc/drm_core" -- modload -p misc/drm_core
	excluded "drv/virtio_gpu" -- modload -p drv/virtio_gpu
	step devfsadm -i virtio_gpu
	loaded virtio_gpu && bad "virtio_gpu attached" || pass "devfsadm -i virtio_gpu attaches nothing"
	step modload -p misc/drm && pass "the legacy misc/drm loads" || bad "the legacy misc/drm does not load"
	step modload -p drv/i915 && pass "the legacy i915 module loads" || bad "the legacy i915 module does not load"
	step devfsadm -i i915
	step sh -c 'prtconf -D | grep -i i915'
	info "the guest has no Intel GPU: i915 has no device to attach to"
	unload_all
	echo
	echo "--- stale /dev/dri links of the new stack"
	step ls -l /dev/dri
	devices 0 "before devfsadm -C, with the new stack excluded"
	step devfsadm -C -v
	left=$(ls /dev/dri 2>/dev/null | wc -l | tr -d ' ')
	[ "$left" -eq 0 ] && pass "devfsadm -C removed every stale /dev/dri link" ||
	    { ls -l /dev/dri; bad "$left /dev/dri links survive devfsadm -C"; }
	devices 0 "with the legacy stack selected"
}

phase_select_root() {
	echo "=== select-root: drmadm -R on a mounted boot environment"
	cur=$(sed -n 's/^\* drmadm-selected: //p' "$FRAG")
	case $cur in
	legacy) other=new ;;
	*) other=legacy ;;
	esac
	step beadm create g7-root || exit 1
	step beadm mount g7-root /mnt || exit 1
	step "$DRMADM" -R /mnt status
	step "$DRMADM" -R /mnt select "$other" | tee drmadm-select-root.out
	grep -q "^\* drmadm-selected: $other" "/mnt$FRAG" && pass "drmadm -R /mnt selected $other there" ||
	    bad "drmadm -R /mnt did not select $other"
	grep -q "^\* drmadm-selected: $cur" "$FRAG" && pass "the running root still has $cur" ||
	    bad "drmadm -R changed the running root"
	cmp -s /etc/driver_aliases /mnt/etc/driver_aliases && bad "the aliases of /mnt did not change" ||
	    pass "drmadm -R moved the aliases in /mnt only"
	step "$DRMADM" -R /mnt status
	step beadm umount g7-root
	step beadm destroy -F g7-root
}

RACES=${RACES:-100}
case "${1:-}" in
baseline) phase_baseline ;;
install) phase_install ;;
guards) phase_guards ;;
select-new) phase_select_new ;;
after-new) phase_after_new ;;
select-legacy) phase_select_legacy ;;
after-legacy) phase_after_legacy ;;
select-root) phase_select_root ;;
*) echo "usage: run-g7-test.sh baseline|install|guards|select-new|after-new|select-legacy|after-legacy|select-root" >&2
   exit 2 ;;
esac
echo
echo "checks passed: $npass"
[ $fail -eq 0 ] && echo "run-g7-test $1: PASS" || echo "run-g7-test $1: FAIL"
exit $fail
