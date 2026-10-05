/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's static keys (include/linux/jump_label.h, GPL-2.0): without code
 * patching on illumos, a key is a boolean read at each test.
 */
#ifndef _LINUX_JUMP_LABEL_H
#define	_LINUX_JUMP_LABEL_H
#include <linux/types.h>
struct static_key {
	atomic_t enabled;
};
struct static_key_true {
	struct static_key key;
};
struct static_key_false {
	struct static_key key;
};
#define	STATIC_KEY_INIT_TRUE		{ .enabled = { 1 } }
#define	STATIC_KEY_INIT_FALSE		{ .enabled = { 0 } }
#define	DEFINE_STATIC_KEY_TRUE(name)	struct static_key_true name = { .key = STATIC_KEY_INIT_TRUE }
#define	DEFINE_STATIC_KEY_FALSE(name)	struct static_key_false name = { .key = STATIC_KEY_INIT_FALSE }
#define	DECLARE_STATIC_KEY_TRUE(name)	extern struct static_key_true name
#define	DECLARE_STATIC_KEY_FALSE(name)	extern struct static_key_false name
#define	static_key_enabled(k)		(__atomic_load_n(&(k)->key.enabled.counter, __ATOMIC_RELAXED) > 0)
#define	static_branch_likely(k)		__builtin_expect(static_key_enabled(k), 1)
#define	static_branch_unlikely(k)	__builtin_expect(static_key_enabled(k), 0)
#define	static_branch_enable(k)		__atomic_store_n(&(k)->key.enabled.counter, 1, __ATOMIC_SEQ_CST)
#define	static_branch_disable(k)	__atomic_store_n(&(k)->key.enabled.counter, 0, __ATOMIC_SEQ_CST)
#define	static_branch_inc(k)		__atomic_fetch_add(&(k)->key.enabled.counter, 1, __ATOMIC_SEQ_CST)
#define	static_branch_dec(k)		__atomic_fetch_sub(&(k)->key.enabled.counter, 1, __ATOMIC_SEQ_CST)
#endif
