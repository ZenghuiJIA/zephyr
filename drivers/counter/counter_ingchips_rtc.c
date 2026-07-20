/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/sys_io.h>
#include <platform_api.h>

enum ing_rtc_layout { ING_RTC_916, ING_RTC_918 };
struct ing_rtc_config {
	struct counter_config_info info;
	uintptr_t counter_base;
	uintptr_t control_base;
	enum ing_rtc_layout layout;
};
struct ing_rtc_data {
	struct k_spinlock lock;
	counter_alarm_callback_t callback;
	void *user_data;
};

static int stable_read(uintptr_t addr, uint32_t *value)
{
	uint32_t a = sys_read32(addr);

	for (uint32_t attempt = 0U; attempt < 16U; attempt++) {
		uint32_t b = sys_read32(addr);

		if (a == b) {
			*value = b;
			return 0;
		}
		a = b;
	}
	return -EIO;
}

static int ing_rtc_start(const struct device *dev)
{
	const struct ing_rtc_config *c = dev->config;
	uintptr_t a = c->layout == ING_RTC_916 ? c->control_base : c->counter_base + 0x130;
	uint32_t bit = c->layout == ING_RTC_916 ? BIT(6) : BIT(3);
	sys_write32(sys_read32(a) | bit, a);
	return 0;
}

static int ing_rtc_stop(const struct device *dev)
{
	const struct ing_rtc_config *c = dev->config;
	uintptr_t a = c->layout == ING_RTC_916 ? c->control_base : c->counter_base + 0x130;
	uint32_t bit = c->layout == ING_RTC_916 ? BIT(6) : BIT(3);
	sys_write32(sys_read32(a) & ~bit, a);
	return 0;
}

static int ing_rtc_get_value(const struct device *dev, uint32_t *ticks)
{
	const struct ing_rtc_config *c = dev->config;
	return stable_read(c->counter_base + (c->layout == ING_RTC_916 ? 0xb8 : 0x168), ticks);
}

static int ing_rtc_set_alarm(const struct device *dev, uint8_t chan,
			     const struct counter_alarm_cfg *alarm)
{
	const struct ing_rtc_config *c = dev->config;
	struct ing_rtc_data *d = dev->data;
	k_spinlock_key_t key;
	uint32_t now, target;

	if (chan != 0U || alarm == NULL || alarm->callback == NULL) { return -EINVAL; }
	if (ing_rtc_get_value(dev, &now) != 0) {
		return -EIO;
	}
	target = (alarm->flags & COUNTER_ALARM_CFG_ABSOLUTE) ? alarm->ticks : now + alarm->ticks;
	key = k_spin_lock(&d->lock);
	if (d->callback != NULL) { k_spin_unlock(&d->lock, key); return -EBUSY; }
	d->callback = alarm->callback;
	d->user_data = alarm->user_data;
	if (c->layout == ING_RTC_916) {
		sys_write32(target, c->control_base + 0x04);
		sys_write32(sys_read32(c->control_base + 0x1c) | 0x5U, c->control_base + 0x1c);
	} else {
		sys_write32(0, c->counter_base + 0x124);
		sys_write32(target - now, c->counter_base + 0x10c);
	}
	k_spin_unlock(&d->lock, key);
	return 0;
}

static int ing_rtc_cancel_alarm(const struct device *dev, uint8_t chan)
{
	const struct ing_rtc_config *c = dev->config;
	struct ing_rtc_data *d = dev->data;
	k_spinlock_key_t key;
	if (chan != 0U) { return -EINVAL; }
	key = k_spin_lock(&d->lock);
	d->callback = NULL;
	if (c->layout == ING_RTC_916) {
		sys_write32(sys_read32(c->control_base + 0x1c) & ~0x5U, c->control_base + 0x1c);
	} else {
		sys_write32(0, c->counter_base + 0x10c);
	}
	k_spin_unlock(&d->lock, key);
	return 0;
}

static uint32_t ing_rtc_top(const struct device *dev) { ARG_UNUSED(dev); return UINT32_MAX; }
static uint32_t ing_rtc_freq(const struct device *dev)
{ return ((const struct ing_rtc_config *)dev->config)->info.freq; }
static uint32_t ing_rtc_pending(const struct device *dev)
{
	const struct ing_rtc_config *c = dev->config;
	return c->layout == ING_RTC_916 ? !!(sys_read32(c->control_base + 0x1c) & BIT(3)) :
		!!(sys_read32(c->counter_base + 0x130) & BIT(5));
}

static void ing_rtc_isr(const struct device *dev)
{
	const struct ing_rtc_config *c = dev->config;
	struct ing_rtc_data *d = dev->data;
	counter_alarm_callback_t cb = d->callback;
	void *ud = d->user_data;
	uint32_t now;
	d->callback = NULL;
	if (c->layout == ING_RTC_916) {
		sys_write32(sys_read32(c->control_base + 0x1c) | BIT(1), c->control_base + 0x1c);
	} else {
		uint32_t v = sys_read32(c->counter_base + 0x130);
		sys_write32(v | BIT(0), c->counter_base + 0x130);
		sys_write32(v & ~BIT(0), c->counter_base + 0x130);
	}
	if (cb && ing_rtc_get_value(dev, &now) == 0) { cb(dev, 0, now, ud); }
}

static uint32_t ing_rtc_platform_isr(void *arg)
{
	ing_rtc_isr(arg);
	return 0;
}

static void ing_rtc_register_callback(const struct device *dev)
{
	void (*volatile set_callback)(platform_irq_callback_type_t, f_platform_irq_cb, void *) =
		platform_set_irq_callback;

	set_callback(PLATFORM_CB_IRQ_RTC, ing_rtc_platform_isr, (void *)dev);
}

static int ing_rtc_init(const struct device *dev)
{
	const struct ing_rtc_config *c = dev->config;
	ARG_UNUSED(c);
	ing_rtc_register_callback(dev);
	return ing_rtc_start(dev);
}

static DEVICE_API(counter, ing_rtc_api) = {
	.start = ing_rtc_start, .stop = ing_rtc_stop, .get_value = ing_rtc_get_value,
	.set_alarm = ing_rtc_set_alarm, .cancel_alarm = ing_rtc_cancel_alarm,
	.get_top_value = ing_rtc_top, .get_pending_int = ing_rtc_pending, .get_freq = ing_rtc_freq,
};

#define ING_RTC_COMMON(inst, kind, ctr, ctl, hz) \
	static struct ing_rtc_data ing_rtc_data_##kind##_##inst; \
	static const struct ing_rtc_config ing_rtc_cfg_##kind##_##inst = { \
		.info = { .max_top_value = UINT32_MAX, .freq = hz, .flags = COUNTER_CONFIG_INFO_COUNT_UP, \
			.channels = 1 }, .counter_base = ctr, .control_base = ctl, .layout = kind }; \
	DEVICE_DT_INST_DEFINE(inst, ing_rtc_init, NULL, &ing_rtc_data_##kind##_##inst, \
		&ing_rtc_cfg_##kind##_##inst, POST_KERNEL, CONFIG_COUNTER_INIT_PRIORITY, &ing_rtc_api)

#define DT_DRV_COMPAT ingchips_ing916_rtc
#define ING916_RTC(inst) ING_RTC_COMMON(inst, ING_RTC_916, DT_INST_REG_ADDR_BY_NAME(inst, counter), \
	DT_INST_REG_ADDR_BY_NAME(inst, control), 32768U)
DT_INST_FOREACH_STATUS_OKAY(ING916_RTC)
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT ingchips_ing918_rtc
#define ING918_RTC(inst) ING_RTC_COMMON(inst, ING_RTC_918, DT_INST_REG_ADDR(inst), DT_INST_REG_ADDR(inst), 50000U)
DT_INST_FOREACH_STATUS_OKAY(ING918_RTC)
