/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <kunit/visibility.h>: no KUnit on illumos; VISIBLE_IF_KUNIT and
 * EXPORT_SYMBOL_IF_KUNIT come from <linux/export.h> (static, nothing).
 */
#ifndef _KUNIT_VISIBILITY_H
#define	_KUNIT_VISIBILITY_H
#include <linux/export.h>
#endif
