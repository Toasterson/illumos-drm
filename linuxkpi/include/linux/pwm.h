/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's PWM consumer interface (include/linux/pwm.h, GPL-2.0;
 * independent declarations). illumos has no PWM controllers: pwm_get()
 * fails with -ENODEV, so drivers fall back to their own backlight
 * registers (i915 uses PWM devices only for the SoC/PMIC backlight of
 * Valleyview and Broxton). The state helpers work on the structure as
 * Linux's do:
 *
 * pwm_get_state(pwm, state)	copies pwm->state.
 * pwm_get_relative_duty_cycle(state, scale)
 *	the duty cycle in units of period/scale, rounded to nearest; 0 for
 *	a zero period.
 * pwm_set_relative_duty_cycle(state, duty, scale)
 *	sets duty_cycle = period * duty / scale (rounded to nearest);
 *	-EINVAL for scale 0 or duty > scale.
 * pwm_apply_might_sleep(pwm, state)	-ENODEV (no controller).
 */
#ifndef _LINUX_PWM_H
#define	_LINUX_PWM_H

#include <linux/types.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/math64.h>

struct device;
struct pwm_chip;

enum pwm_polarity {
	PWM_POLARITY_NORMAL,
	PWM_POLARITY_INVERSED
};

struct pwm_args {
	u64 period;
	enum pwm_polarity polarity;
};

struct pwm_state {
	u64 period;
	u64 duty_cycle;
	enum pwm_polarity polarity;
	bool enabled;
	bool usage_power;
};

struct pwm_device {
	const char *label;
	unsigned long flags;
	unsigned int hwpwm;
	struct pwm_chip *chip;
	struct pwm_args args;
	struct pwm_state state;
	struct pwm_state last;
};

static inline struct pwm_device *
pwm_get(struct device *dev, const char *con_id)
{
	return (ERR_PTR(-ENODEV));
}

static inline struct pwm_device *
devm_pwm_get(struct device *dev, const char *con_id)
{
	return (ERR_PTR(-ENODEV));
}

static inline void
pwm_put(struct pwm_device *pwm)
{
}

static inline void
pwm_get_state(const struct pwm_device *pwm, struct pwm_state *state)
{
	*state = pwm->state;
}

static inline bool
pwm_is_enabled(const struct pwm_device *pwm)
{
	return (pwm->state.enabled);
}

static inline int
pwm_apply_might_sleep(struct pwm_device *pwm, const struct pwm_state *state)
{
	return (-ENODEV);
}

static inline unsigned int
pwm_get_relative_duty_cycle(const struct pwm_state *state, unsigned int scale)
{
	if (state->period == 0)
		return (0);
	return ((unsigned int)(((u64)state->duty_cycle * scale +
	    state->period / 2) / state->period));
}

static inline int
pwm_set_relative_duty_cycle(struct pwm_state *state, unsigned int duty_cycle,
    unsigned int scale)
{
	if (scale == 0 || duty_cycle > scale)
		return (-EINVAL);
	state->duty_cycle = ((u64)duty_cycle * state->period + scale / 2) /
	    scale;
	return (0);
}

#endif
