/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The C half of the layout check of crates/drm-uapi/src/virtgpu_compat.rs:
 * the older layouts of the virtio-gpu EXECBUFFER and transfer structures,
 * printed from the original headers in the format of drm_layout.c. Built
 * twice on illumos (see the README, "Checks"):
 *
 *   -DV1 -I<libdrm 2.4.109 include/drm, with the illumos patch>
 *        32-byte EXECBUFFER, 36-byte transfers
 *   -DV2 -I<Mesa 21.3 include/drm-uapi> -I<libdrm include/drm>
 *        40-byte EXECBUFFER
 */
#include <stdio.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/ioccom.h>
#include "drm.h"
#include "virtgpu_drm.h"

#define	SIZE(t, n)	printf("size %s %zu\n", n, sizeof (struct t))
#define	OFF(t, n, m)	printf("off %s %s %zu\n", n, #m, offsetof(struct t, m))
/* A member whose name is a Rust keyword, labelled as drm_layout.c does. */
#define	OFFK(t, n, m)	printf("off %s %s_ %zu\n", n, #m, offsetof(struct t, m))
#define	IOCTL(v, n)	printf("ioctl %s 0x%x\n", n, (unsigned int)(v))

int
main(void)
{
#if defined(V1)
	SIZE(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v1");
	OFF(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v1", flags);
	OFF(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v1", size);
	OFF(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v1", command);
	OFF(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v1", bo_handles);
	OFF(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v1", num_bo_handles);
	OFF(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v1", fence_fd);
	IOCTL(DRM_IOCTL_VIRTGPU_EXECBUFFER, "DRM_IOCTL_VIRTGPU_EXECBUFFER_V1");
	SIZE(drm_virtgpu_3d_transfer_to_host, "drm_virtgpu_3d_transfer_to_host_v1");
	OFF(drm_virtgpu_3d_transfer_to_host, "drm_virtgpu_3d_transfer_to_host_v1", bo_handle);
	OFFK(drm_virtgpu_3d_transfer_to_host, "drm_virtgpu_3d_transfer_to_host_v1", box);
	OFF(drm_virtgpu_3d_transfer_to_host, "drm_virtgpu_3d_transfer_to_host_v1", level);
	OFF(drm_virtgpu_3d_transfer_to_host, "drm_virtgpu_3d_transfer_to_host_v1", offset);
	IOCTL(DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, "DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST_V1");
	SIZE(drm_virtgpu_3d_transfer_from_host, "drm_virtgpu_3d_transfer_from_host_v1");
	OFF(drm_virtgpu_3d_transfer_from_host, "drm_virtgpu_3d_transfer_from_host_v1", bo_handle);
	OFFK(drm_virtgpu_3d_transfer_from_host, "drm_virtgpu_3d_transfer_from_host_v1", box);
	OFF(drm_virtgpu_3d_transfer_from_host, "drm_virtgpu_3d_transfer_from_host_v1", level);
	OFF(drm_virtgpu_3d_transfer_from_host, "drm_virtgpu_3d_transfer_from_host_v1", offset);
	IOCTL(DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, "DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST_V1");
#elif defined(V2)
	SIZE(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v2");
	OFF(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v2", flags);
	OFF(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v2", size);
	OFF(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v2", command);
	OFF(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v2", bo_handles);
	OFF(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v2", num_bo_handles);
	OFF(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v2", fence_fd);
	OFF(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v2", ring_idx);
	OFF(drm_virtgpu_execbuffer, "drm_virtgpu_execbuffer_v2", pad);
	IOCTL(DRM_IOCTL_VIRTGPU_EXECBUFFER, "DRM_IOCTL_VIRTGPU_EXECBUFFER_V2");
#else
#error "build with -DV1 or -DV2"
#endif
	return (0);
}
