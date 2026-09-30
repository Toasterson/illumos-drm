# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Shell functions shared by the run-*-test.sh scripts; sourced after they
# define step(). Every driver built on the core depends on misc/drm_minor,
# the table of DRM device numbers of the whole system, and a device's
# nodes are card<N> and renderD<128+N> for the N it was given when it
# attached, so the scripts find their nodes through the /dev/dri links
# instead of assuming card0.
#
# The directory must hold drm_minor.a (`cargo xtask build drm_minor`) and
# drm_minor.mapfile (modules/drm-minor/exports.mapfile) unless the module
# is installed already.

DRM_MINOR_DIR=/kernel/misc/amd64
DRM_MINOR_OURS=no

# drm_minor_install: links misc/drm_minor, checks that it exports only its
# entry points and its two functions, installs it and loads it, unless it
# is installed already (by the package or an enclosing test script).
drm_minor_install() {
	if [ -f "$DRM_MINOR_DIR/drm_minor" ] || [ -f /usr/kernel/misc/amd64/drm_minor ]; then
		echo "misc/drm_minor is installed already"
		step modload -p misc/drm_minor
		return 0
	fi
	step digest -a sha256 drm_minor.a
	step ld -ztype=kmod -B reduce -M drm_minor.mapfile -z allextract drm_minor.a -o drm_minor || return 1
	step digest -a sha256 drm_minor
	echo
	echo "\$ nm drm_minor: the global symbols it defines"
	globals=$(nm drm_minor | awk -F'|' '$5 ~ /GLOB/ && $7 !~ /UNDEF/ {gsub(/ /, "", $8); print $8}' | sort | tr '\n' ' ')
	echo "$globals"
	if [ "$globals" = "_fini _info _init drm_minor_alloc drm_minor_free " ]; then
		echo "PASS drm_minor exports only _init, _fini, _info, drm_minor_alloc and drm_minor_free"
	else
		echo "FAIL drm_minor exports other symbols"
		return 1
	fi
	step cp drm_minor "$DRM_MINOR_DIR/drm_minor" || return 1
	DRM_MINOR_OURS=yes
	step modload -p misc/drm_minor
}

# drm_minor_remove: unloads and removes misc/drm_minor if drm_minor_install
# installed it. It stays when a driver still depends on it.
drm_minor_remove() {
	[ "$DRM_MINOR_OURS" = yes ] || return 0
	id=$(modinfo | awk '$6 == "drm_minor" {print $1}')
	if [ -n "$id" ] && ! step modunload -i "$id"; then
		echo "misc/drm_minor stays: a driver still depends on it"
		return 1
	fi
	step rm -f "$DRM_MINOR_DIR/drm_minor"
	DRM_MINOR_OURS=no
}

# dri_link DEVICE KIND: prints the /dev/dri link of kind KIND (card or
# renderD) whose target is a minor node of the device node DEVICE (the
# last component of its /devices path, such as rdrmnull@0 or
# pci1af4,1100@10), waiting up to 10 seconds for devfsadm.
dri_link() {
	i=0
	while [ $i -lt 10 ]; do
		for l in /dev/dri/"$2"*; do
			case $(ls -l "$l" 2>/dev/null) in
			*"/$1:$2"[0-9]*) echo "$l"; return 0 ;;
			esac
		done
		sleep 1
		i=$((i + 1))
	done
	return 1
}

# rdrmnull_nodes: exports DRM_CARD, DRM_RENDER and DRM_INSTANCE for
# rdrmnull@0 and, if it exists, DRM_CARD1 for rdrmnull@1. Fails unless the
# card and render numbers agree (card<N>, renderD<128+N>).
rdrmnull_nodes() {
	DRM_CARD=$(dri_link rdrmnull@0 card) || return 1
	DRM_RENDER=$(dri_link rdrmnull@0 renderD) || return 1
	DRM_INSTANCE=0
	n=${DRM_CARD##*card}
	if [ "${DRM_RENDER##*renderD}" != $((128 + n)) ]; then
		echo "FAIL $DRM_CARD and $DRM_RENDER do not belong together"
		return 1
	fi
	export DRM_CARD DRM_RENDER DRM_INSTANCE
	if [ -d /devices/pseudo ] && ls /devices/pseudo | grep -q '^rdrmnull@1:card'; then
		DRM_CARD1=$(dri_link rdrmnull@1 card) || return 1
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
