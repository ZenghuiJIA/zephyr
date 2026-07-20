/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/sys_io.h>

enum ing_pwm_layout { ING_PWM_916, ING_PWM_918 };
struct ing_pwm_config { uintptr_t base; uint32_t clock; enum ing_pwm_layout layout; };
struct ing_pwm_data { struct k_spinlock lock; };

static uint32_t field_set(uint32_t old, uint32_t shift, uint32_t width, uint32_t value)
{
	uint32_t mask = GENMASK(shift + width - 1U, shift);
	return (old & ~mask) | ((value << shift) & mask);
}

static int ing_pwm_set_cycles(const struct device *dev, uint32_t channel,
			      uint32_t period, uint32_t pulse, pwm_flags_t flags)
{
	const struct ing_pwm_config *c = dev->config;
	struct ing_pwm_data *d = dev->data;
	k_spinlock_key_t key;
	uint32_t threshold;

	if (channel >= 3U || pulse > period || period == 0U) { return -EINVAL; }
	if (flags & ~PWM_POLARITY_INVERTED) { return -ENOTSUP; }
	threshold = (flags & PWM_POLARITY_INVERTED) ? pulse : period - pulse;
	key = k_spin_lock(&d->lock);
	if (c->layout == ING_PWM_916) {
		uintptr_t ch = c->base + channel * 0x20U;
		uint32_t ctrl = sys_read32(ch);
		ctrl = field_set(ctrl, 2, 2, 3); /* halt both outputs while updating */
		ctrl &= ~BIT(6);
		sys_write32(ctrl, ch);
		sys_write32(period, ch + 0x10);
		sys_write32(threshold, ch + 0x14);
		ctrl = field_set(ctrl, 7, 3, 0); /* up counter, no dead zone */
		ctrl = field_set(ctrl, 0, 2, pulse == 0U ? 3U : 0U);
		ctrl |= BIT(6);
		ctrl = field_set(ctrl, 2, 2, 0);
		sys_write32(ctrl, ch);
	} else {
		uintptr_t group = c->base + 0x20U + channel * 0x40U;
		uint32_t v;
		v = field_set(sys_read32(c->base + 0x04), channel, 1, 1);
		sys_write32(v, c->base + 0x04);
		v = field_set(sys_read32(c->base + 0x0c), channel, 1, 0);
		sys_write32(v, c->base + 0x0c);
		sys_write32(period, group);
		sys_write32(0, group + 0x04);
		sys_write32(threshold, group + 0x08);
		v = field_set(sys_read32(c->base + 0x10), channel * 2U, 2, 0);
		sys_write32(v, c->base + 0x10);
		v = field_set(sys_read32(c->base + 0x14), channel * 3U, 3, 0);
		sys_write32(v, c->base + 0x14);
		v = field_set(sys_read32(c->base), channel * 2U, 2, pulse == 0U ? 3U : 0U);
		sys_write32(v, c->base);
		v = field_set(sys_read32(c->base + 0x0c), channel, 1, 1);
		sys_write32(v, c->base + 0x0c);
		v = field_set(sys_read32(c->base + 0x04), channel, 1, 0);
		sys_write32(v, c->base + 0x04);
	}
	k_spin_unlock(&d->lock, key);
	return 0;
}

static int ing_pwm_get_cycles(const struct device *dev, uint32_t channel, uint64_t *cycles)
{
	const struct ing_pwm_config *c = dev->config;
	if (channel >= 3U) { return -EINVAL; }
	*cycles = c->clock;
	return 0;
}

static DEVICE_API(pwm, ing_pwm_api) = {
	.set_cycles = ing_pwm_set_cycles,
	.get_cycles_per_sec = ing_pwm_get_cycles,
};

#define ING_PWM_DEFINE(inst, kind) \
	static struct ing_pwm_data ing_pwm_data_##kind##_##inst; \
	static const struct ing_pwm_config ing_pwm_cfg_##kind##_##inst = { \
		.base = DT_INST_REG_ADDR(inst), .clock = DT_INST_PROP(inst, input_clock_frequency), \
		.layout = kind }; \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &ing_pwm_data_##kind##_##inst, \
		&ing_pwm_cfg_##kind##_##inst, POST_KERNEL, CONFIG_PWM_INIT_PRIORITY, &ing_pwm_api)

#define DT_DRV_COMPAT ingchips_ing916_pwm
#define ING916_PWM(inst) ING_PWM_DEFINE(inst, ING_PWM_916)
DT_INST_FOREACH_STATUS_OKAY(ING916_PWM)
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT ingchips_ing918_pwm
#define ING918_PWM(inst) ING_PWM_DEFINE(inst, ING_PWM_918)
DT_INST_FOREACH_STATUS_OKAY(ING918_PWM)
