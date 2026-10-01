/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * The CTF shadow of misc/drm: a C translation unit that includes every
 * facade header and names every facade type, compiled with DWARF, turned
 * into CTF with ctfconvert and linked into the module, so that mdb -k can
 * ::print the facade's structures (struct drm_device, struct dma_fence,
 * ...) inside misc/drm, which is otherwise Rust and has no CTF (ctfconvert
 * does not read Rust's DWARF; illumos-ddi docs/DESIGN.md section 10).
 * It defines no code and no data the module uses.
 *
 *	gcc -m64 -D_KERNEL -std=gnu99 -gdwarf-4 -fno-eliminate-unused-debug-types \
 *	    -I<illumos-drm>/include -c drm_facade_ctf.c
 *	/opt/onbld/bin/i386/ctfconvert -l drm drm_facade_ctf.o
 */

#include <drm/drm_facade.h>

/*
 * One pointer variable per facade structure (eight bytes of BSS each),
 * which carries the structure's definition into the debug information.
 * They are not in drm.mapfile, so the link makes them local.
 * struct drm_illumos_mapping, struct drm_rkms_fb and the other incomplete
 * types have no layout of their own and are left out.
 */
#define	DRM_FACADE_CTF_(s)	struct s *drm_facade_ctf_##s;
DRM_FACADE_STRUCTS(DRM_FACADE_CTF_)
#undef	DRM_FACADE_CTF_

struct drm_facade_version *drm_facade_ctf_version;
spinlock_t *drm_facade_ctf_spinlock;
