/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's pseudo-random state (include/linux/prandom.h, GPL-2.0;
 * reimplemented with a xorshift128+ generator, not Linux's Tausworthe:
 * sequences differ from Linux, which only selftests compare).
 */
#ifndef _LINUX_PRANDOM_H
#define	_LINUX_PRANDOM_H
#include <linux/types.h>
struct rnd_state {
	u64 s0, s1;
};
static inline void
prandom_seed_state(struct rnd_state *state, u64 seed)
{
	state->s0 = seed ^ 0x9e3779b97f4a7c15ULL;
	state->s1 = (seed << 1) ^ 0xbf58476d1ce4e5b9ULL;
	if (state->s0 == 0 && state->s1 == 0)
		state->s1 = 1;
}
static inline u32
prandom_u32_state(struct rnd_state *state)
{
	u64 x = state->s0, y = state->s1;

	state->s0 = y;
	x ^= x << 23;
	state->s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
	return ((u32)((state->s1 + y) >> 32));
}
static inline void
prandom_bytes_state(struct rnd_state *state, void *buf, size_t nbytes)
{
	u8 *p = buf;

	while (nbytes-- > 0)
		*p++ = (u8)prandom_u32_state(state);
}
#endif
