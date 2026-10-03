/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's string helpers (include/linux/string_helpers.h and
 * string_choices.h, GPL-2.0; reimplemented): str_yes_no() and friends.
 */
#ifndef _LINUX_STRING_HELPERS_H
#define	_LINUX_STRING_HELPERS_H

#include <linux/types.h>
#include <linux/string.h>
#include <linux/string_choices.h>

enum string_size_units {
	STRING_UNITS_10,
	STRING_UNITS_2
};

extern int string_get_size(u64 size, u64 blk_size, enum string_size_units units,
    char *buf, int len);

#endif
