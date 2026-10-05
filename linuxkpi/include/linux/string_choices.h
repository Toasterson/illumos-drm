/* SPDX-License-Identifier: MIT */
/* Copyright 2026 The illumos-drm authors. Linux's str_*() choice helpers. */
#ifndef _LINUX_STRING_CHOICES_H
#define	_LINUX_STRING_CHOICES_H
#include <linux/types.h>
static inline const char *str_yes_no(bool v) { return (v ? "yes" : "no"); }
static inline const char *str_no_yes(bool v) { return (v ? "no" : "yes"); }
static inline const char *str_on_off(bool v) { return (v ? "on" : "off"); }
static inline const char *str_off_on(bool v) { return (v ? "off" : "on"); }
static inline const char *str_true_false(bool v) { return (v ? "true" : "false"); }
static inline const char *str_false_true(bool v) { return (v ? "false" : "true"); }
static inline const char *str_enable_disable(bool v) { return (v ? "enable" : "disable"); }
static inline const char *str_disable_enable(bool v) { return (v ? "disable" : "enable"); }
static inline const char *str_enabled_disabled(bool v) { return (v ? "enabled" : "disabled"); }
static inline const char *str_disabled_enabled(bool v) { return (v ? "disabled" : "enabled"); }
static inline const char *str_read_write(bool v) { return (v ? "read" : "write"); }
static inline const char *str_write_read(bool v) { return (v ? "write" : "read"); }
static inline const char *str_high_low(bool v) { return (v ? "high" : "low"); }
static inline const char *str_low_high(bool v) { return (v ? "low" : "high"); }
static inline const char *str_up_down(bool v) { return (v ? "up" : "down"); }
static inline const char *str_down_up(bool v) { return (v ? "down" : "up"); }
static inline const char *str_plural(size_t n) { return (n == 1 ? "" : "s"); }
static inline const char *str_lo_hi(bool v) { return (v ? "lo" : "hi"); }
static inline const char *str_hi_lo(bool v) { return (v ? "hi" : "lo"); }
static inline const char *str_active_inactive(bool v) { return (v ? "active" : "inactive"); }
static inline const char *str_get_put(bool v) { return (v ? "get" : "put"); }
static inline const char *str_locked_unlocked(bool v) { return (v ? "locked" : "unlocked"); }
static inline const char *str_on_off_upper(bool v) { return (v ? "ON" : "OFF"); }
#define	str_enabled_disabled_upper(v)	((v) ? "ENABLED" : "DISABLED")
#endif
