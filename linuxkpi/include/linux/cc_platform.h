/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's confidential-computing platform attributes (<linux/cc_platform.h>).
 * illumos runs no guest with encrypted memory (SEV/TDX) that this layer
 * would need to know about: every attribute is absent.
 */
#ifndef _LINUX_CC_PLATFORM_H
#define	_LINUX_CC_PLATFORM_H

#include <linux/types.h>

enum cc_attr {
	CC_ATTR_MEM_ENCRYPT,
	CC_ATTR_HOST_MEM_ENCRYPT,
	CC_ATTR_GUEST_MEM_ENCRYPT,
	CC_ATTR_GUEST_STATE_ENCRYPT,
	CC_ATTR_GUEST_UNROLL_STRING_IO,
	CC_ATTR_GUEST_SEV_SNP,
	CC_ATTR_HOST_SEV_SNP,
};

static inline bool
cc_platform_has(enum cc_attr attr)
{
	return (false);
}

#endif
