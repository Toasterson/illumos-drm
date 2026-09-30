/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * A test aid, loaded with LD_PRELOAD_64, that answers libdrm's device
 * enumeration (drmGetDevice2, drmGetDevices2 and their older forms) for
 * the virtio-gpu device of the R4 test guest.
 *
 * OpenIndiana's libdrm 2.4.109 (gfx-drm, usr/src/lib/libdrm/common/
 * sun_devinfo.c) finds DRM devices only among device nodes named
 * "display", the name the PCI enumerator gives VGA-class devices such as
 * i915's. The virtio-gpu function (PCI class 0x0380) is named after its
 * subsystem ID, pci1af4,1100, so libdrm finds no device, Mesa's loader
 * prints "failed to retrieve device information", and EGL's DRM platform
 * fails with "DRI2: failed to find EGLDevice". The fix belongs in libdrm
 * (match the ddi_display:drm minor node type rather than the node name);
 * until then this shim describes the one device: PCI 1af4:1050 at
 * 0000:00:10.0 (the slot run-r4 guests put it in), card0 and renderD128.
 *
 *   gcc -m64 -shared -fPIC -o drm_devshim.so drm_devshim.c
 *   LD_PRELOAD_64=./drm_devshim.so program
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <xf86drm.h>

struct block {
	drmDevice dev;
	char *nodes[DRM_NODE_MAX];
	char card[32];
	char render[32];
	drmPciBusInfo bus;
	drmPciDeviceInfo info;
};

static drmDevicePtr
make(void)
{
	struct block *b = calloc(1, sizeof (*b));
	struct stat st;

	if (b == NULL)
		return (NULL);
	(void) strcpy(b->card, "/dev/dri/card0");
	(void) strcpy(b->render, "/dev/dri/renderD128");
	b->dev.nodes = b->nodes;
	if (stat(b->card, &st) == 0) {
		b->nodes[DRM_NODE_PRIMARY] = b->card;
		b->dev.available_nodes |= 1 << DRM_NODE_PRIMARY;
	}
	if (stat(b->render, &st) == 0) {
		b->nodes[DRM_NODE_RENDER] = b->render;
		b->dev.available_nodes |= 1 << DRM_NODE_RENDER;
	}
	b->dev.bustype = DRM_BUS_PCI;
	b->bus = (drmPciBusInfo){ .domain = 0, .bus = 0, .dev = 0x10, .func = 0 };
	b->info = (drmPciDeviceInfo){ .vendor_id = 0x1af4, .device_id = 0x1050,
	    .subvendor_id = 0x1af4, .subdevice_id = 0x1100, .revision_id = 1 };
	b->dev.businfo.pci = &b->bus;
	b->dev.deviceinfo.pci = &b->info;
	return (&b->dev);
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void) fd;
	(void) flags;
	*device = make();
	return (*device == NULL ? -ENOMEM : 0);
}

int
drmGetDevice(int fd, drmDevicePtr *device)
{
	return (drmGetDevice2(fd, 0, device));
}

int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void) flags;
	if (devices == NULL || max_devices < 1)
		return (1);
	devices[0] = make();
	return (devices[0] == NULL ? -ENOMEM : 1);
}

int
drmGetDevices(drmDevicePtr devices[], int max_devices)
{
	return (drmGetDevices2(0, devices, max_devices));
}
