/*
 * Input to bindgen for the drm-uapi crate. The headers are those of libdrm
 * 2.4.109 with the illumos patches that gfx-drm applies when it builds
 * libdrm (usr/src/common/libdrm/patches/incl-drm-*.patch); see
 * tools/uapi/generate.sh.
 */
#include "drm.h"
#include "drm_mode.h"
#include "virtgpu_drm.h"
