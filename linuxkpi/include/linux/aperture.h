/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's firmware-framebuffer handover (aperture helpers). Not supported
 * on illumos: the console framebuffer is not removed; the calls return 0.
 * A headless driver does not touch the display.
 */
#ifndef _LINUX_APERTURE_H
#define	_LINUX_APERTURE_H
#include <linux/types.h>
struct pci_dev;
#define	aperture_remove_conflicting_pci_devices(p, n)	0
#define	aperture_remove_conflicting_devices(b, s, n)	0
#define	devm_aperture_acquire_for_platform_device(p, b, s)	0
#endif
