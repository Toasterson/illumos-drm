# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Shell functions shared by the run-*-test.sh scripts; sourced after they
# define step(). Every DRM driver depends on misc/drm, the DRM core (since
# R5a; it holds the table of DRM device numbers of the whole system, which
# was misc/drm_minor before), and a device's nodes are card<N> and
# renderD<128+N> for the N it was given when it registered, so the scripts
# find their nodes through the /dev/dri links instead of assuming card0.
#
# The directory must hold drm.a (`cargo xtask build drm`) and drm.mapfile
# (modules/drm/drm.mapfile) unless the module is installed already.
#
# OpenIndiana's driver/graphics/drm package (gfx-drm) delivers another
# module of the same name, /kernel/misc/amd64/drm, the C core of its
# legacy drivers. On a test machine with it, drm_core_install moves it
# aside while the test runs and drm_core_remove puts it back.

DRM_CORE_DIR=/kernel/misc/amd64
DRM_CORE_OURS=no
DRM_CORE_ASIDE=no

# drm_core_install: links misc/drm, checks that it exports exactly the
# symbols of its mapfile (_init, _fini, _info and the facade), installs it
# and loads it, unless it is installed already.
drm_core_install() {
	if [ -f /usr/kernel/misc/amd64/drm ] || { [ -f "$DRM_CORE_DIR/drm" ] && \
	    strings "$DRM_CORE_DIR/drm" | grep -q 'DRM core and facade (illumos-drm)'; }; then
		echo "misc/drm (illumos-drm) is installed already"
		step modload -p misc/drm
		return 0
	fi
	step digest -a sha256 drm.a
	# The module is linked as misc-drm (drm/ holds the uAPI headers) and
	# installed as drm.
	step ld -ztype=kmod -N fs/specfs -B reduce -M drm.mapfile -z allextract drm.a -o misc-drm || return 1
	step digest -a sha256 misc-drm
	echo
	echo "\$ nm misc-drm: the global symbols it defines, against drm.mapfile"
	globals=$(nm misc-drm | awk -F'|' '$5 ~ /GLOB/ && $7 !~ /UNDEF/ {gsub(/ /, "", $8); print $8}' | sort | tr '\n' ' ')
	want=$(awk '/global:/ {g=1; next} /local:/ {g=0} g && /;/ {gsub(/[;\t ]/, ""); print}' drm.mapfile | sort | tr '\n' ' ')
	echo "$(echo "$globals" | wc -w | tr -d ' ') global symbols"
	if [ "$globals" = "$want" ]; then
		echo "PASS misc/drm exports exactly the symbols of drm.mapfile"
	else
		echo "FAIL misc/drm exports other symbols than drm.mapfile names"
		echo "have: $globals"
		echo "want: $want"
		return 1
	fi
	if [ -f "$DRM_CORE_DIR/drm" ]; then
		step mv "$DRM_CORE_DIR/drm" "$DRM_CORE_DIR/drm.gfx-drm" || return 1
		DRM_CORE_ASIDE=yes
	fi
	step cp misc-drm "$DRM_CORE_DIR/drm" || return 1
	DRM_CORE_OURS=yes
	step modload -p misc/drm
}

# drm_core_remove: unloads and removes misc/drm if drm_core_install
# installed it, and puts gfx-drm's module back. It stays when a driver
# still depends on it.
drm_core_remove() {
	[ "$DRM_CORE_OURS" = yes ] || return 0
	id=$(modinfo | awk '$6 == "drm" {print $1}')
	if [ -n "$id" ] && ! step modunload -i "$id"; then
		echo "misc/drm stays: a driver still depends on it"
		return 1
	fi
	step rm -f "$DRM_CORE_DIR/drm"
	if [ "$DRM_CORE_ASIDE" = yes ]; then
		step mv "$DRM_CORE_DIR/drm.gfx-drm" "$DRM_CORE_DIR/drm"
		DRM_CORE_ASIDE=no
	fi
	DRM_CORE_OURS=no
}

# dri_link DEVICE KIND: prints the /dev/dri link of kind KIND (card or
# renderD) whose target is a minor node of the device node DEVICE, the
# last component of its /devices path up to the ':' (rdrmnull@0) or a
# prefix of it (pci1af4,1100@ for the virtio-gpu function in any slot),
# waiting up to 10 seconds for devfsadm.
dri_link() {
	i=0
	while [ $i -lt 10 ]; do
		for l in /dev/dri/"$2"*; do
			case $(ls -l "$l" 2>/dev/null) in
			*"/$1"*":$2"[0-9]*) echo "$l"; return 0 ;;
			esac
		done
		sleep 1
		i=$((i + 1))
	done
	return 1
}

# rdrmnull_nodes: exports DRM_CARD, DRM_RENDER and DRM_INSTANCE for
# the driver DRM_DRIVER (default rdrmnull) instance 0 and, if it exists,
# DRM_CARD1 for its instance 1. Fails unless the
# card and render numbers agree (card<N>, renderD<128+N>).
rdrmnull_nodes() {
	DRM_CARD=$(dri_link "${DRM_DRIVER:-rdrmnull}@0" card) || return 1
	DRM_RENDER=$(dri_link "${DRM_DRIVER:-rdrmnull}@0" renderD) || return 1
	DRM_INSTANCE=0
	n=${DRM_CARD##*card}
	if [ "${DRM_RENDER##*renderD}" != $((128 + n)) ]; then
		echo "FAIL $DRM_CARD and $DRM_RENDER do not belong together"
		return 1
	fi
	export DRM_CARD DRM_RENDER DRM_INSTANCE
	if [ -d /devices/pseudo ] && ls /devices/pseudo | grep -q "^${DRM_DRIVER:-rdrmnull}@1:card"; then
		DRM_CARD1=$(dri_link "${DRM_DRIVER:-rdrmnull}@1" card) || return 1
		export DRM_CARD1
	fi
	echo "rdrmnull nodes: DRM_CARD=$DRM_CARD DRM_RENDER=$DRM_RENDER DRM_CARD1=${DRM_CARD1:-none}"
}

# remove_dri_links DEVICE: removes the /dev/dri links to minor nodes of
# DEVICE (a /devices path component, such as rdrmnull@ or pci1af4,1100@),
# which devfsadm -C leaves where no link module claims them.
remove_dri_links() {
	for l in /dev/dri/*; do
		case $(ls -l "$l" 2>/dev/null) in
		*"-> "*"/$1"*) step rm -f "$l" ;;
		esac
	done
}

# wait_log PATTERN COUNT: waits up to 15 seconds until /var/adm/messages
# has more than COUNT lines matching PATTERN. Kernel messages reach the
# file through syslogd, some time after the call that logged them.
wait_log() {
	i=0
	while [ "$(grep -c "$1" /var/adm/messages)" -le "$2" ] && [ $i -lt 15 ]; do
		sleep 1
		i=$((i + 1))
	done
}
