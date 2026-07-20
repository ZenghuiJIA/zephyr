/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <platform_api.h>

enum ing_wdt_layout { ING_WDT_916, ING_WDT_918 };
#if defined(CONFIG_SOC_SERIES_ING916)
#define ING_WDT_PLATFORM_IRQ PLATFORM_CB_IRQ_WDT
#else
#define ING_WDT_PLATFORM_IRQ PLATFORM_CB_IRQ_TIMER0
#endif
struct ing_wdt_config {
	uintptr_t base;
	uintptr_t aon;
	uint32_t clock;
	enum ing_wdt_layout layout;
};
struct ing_wdt_data { wdt_callback_t callback; uint32_t timeout; int mode; bool installed; };

static const uint32_t ing916_intervals_ms[] = {
	2, 8, 32, 63, 125, 250, 500, 1000, 4000, 16000, 64000, 256000,
	1024000, 4096000, 16384000, 65536000,
};

static int ing_wdt_install(const struct device *dev, const struct wdt_timeout_cfg *cfg)
{
	const struct ing_wdt_config *c = dev->config;
	struct ing_wdt_data *d = dev->data;
	if (d->installed) { return -ENOMEM; }
	if (cfg->window.min || !cfg->window.max || cfg->flags != WDT_FLAG_RESET_SOC) {
		return cfg->flags == WDT_FLAG_RESET_CPU_CORE ? -ENOTSUP : -EINVAL;
	}
	if (c->layout == ING_WDT_916) {
		for (d->mode = 0; d->mode < ARRAY_SIZE(ing916_intervals_ms); d->mode++) {
			if (cfg->window.max <= ing916_intervals_ms[d->mode]) { break; }
		}
		if (d->mode == ARRAY_SIZE(ing916_intervals_ms)) { return -EINVAL; }
		d->timeout = ing916_intervals_ms[d->mode];
	} else {
		uint64_t ticks = (uint64_t)c->clock * cfg->window.max / 1000U;
		if (!ticks || ticks > UINT32_MAX) { return -EINVAL; }
		d->timeout = ticks;
	}
	d->callback = cfg->callback;
	d->installed = true;
	return 0;
}

static int ing_wdt_setup(const struct device *dev, uint8_t options)
{
	const struct ing_wdt_config *c = dev->config;
	struct ing_wdt_data *d = dev->data;
	if (!d->installed) { return -EINVAL; }
	if (options & (WDT_OPT_PAUSE_IN_SLEEP | WDT_OPT_PAUSE_HALTED_BY_DBG)) { return -ENOTSUP; }
	if (c->layout == ING_WDT_916) {
		sys_write32(0x5aa5, c->base + 0x18);
		sys_write32((7U << 8) | ((uint32_t)d->mode << 4) | (d->callback ? 0xdU : 0x9U),
			    c->base + 0x10);
		sys_write32(sys_read32(c->aon + 0x14) | BIT(28), c->aon + 0x14);
	} else {
		sys_write32(0xdeadface, c->base + 0x0c);
		sys_write32(BIT(1), c->base + 0x08);
		sys_write32(d->timeout, c->base + 0x04);
		sys_write32(BIT(0) | BIT(5) | (d->callback ? BIT(4) : 0), c->base + 0x08);
		sys_write32(1, c->base + 0x0c);
	}
	return 0;
}

static int ing_wdt_disable(const struct device *dev)
{
	const struct ing_wdt_config *c = dev->config;
	struct ing_wdt_data *d = dev->data;
	if (c->layout == ING_WDT_916) {
		sys_write32(0x5aa5, c->base + 0x18);
		sys_write32(0, c->base + 0x10);
		sys_write32(sys_read32(c->aon + 0x14) & ~BIT(28), c->aon + 0x14);
	} else {
		sys_write32(0xdeadface, c->base + 0x0c);
		sys_write32(sys_read32(c->base + 0x08) & ~BIT(5), c->base + 0x08);
	}
	d->installed = false;
	return 0;
}

static int ing_wdt_feed(const struct device *dev, int channel)
{
	const struct ing_wdt_config *c = dev->config;
	if (channel != 0) { return -EINVAL; }
	if (c->layout == ING_WDT_916) {
		sys_write32(0x5aa5, c->base + 0x18);
		sys_write32(0xcafe, c->base + 0x14);
	} else {
		sys_write32(0xdeadface, c->base + 0x0c);
		sys_write32(sys_read32(c->base + 0x08) | BIT(1), c->base + 0x08);
		sys_write32(1, c->base + 0x0c);
	}
	return 0;
}

static void ing_wdt_isr(const struct device *dev)
{
	const struct ing_wdt_config *c = dev->config;
	struct ing_wdt_data *d = dev->data;
	if (c->layout == ING_WDT_916) { sys_write32(1, c->base + 0x1c); }
	else { sys_write32(sys_read32(c->base + 0x08) | BIT(6), c->base + 0x08); }
	if (d->callback) { d->callback(dev, 0); }
}

static uint32_t ing_wdt_platform_isr(void *arg)
{
	ing_wdt_isr(arg);
	return 0;
}

static void ing_wdt_register_callback(const struct device *dev)
{
	void (*volatile set_callback)(platform_irq_callback_type_t, f_platform_irq_cb, void *) =
		platform_set_irq_callback;

	set_callback(ING_WDT_PLATFORM_IRQ, ing_wdt_platform_isr, (void *)dev);
}

static int ing_wdt_init(const struct device *dev)
{
	ing_wdt_register_callback(dev);
#ifdef CONFIG_WDT_DISABLE_AT_BOOT
	return ing_wdt_disable(dev);
#else
	return 0;
#endif
}

static DEVICE_API(wdt, ing_wdt_api) = {
	.setup = ing_wdt_setup, .disable = ing_wdt_disable,
	.install_timeout = ing_wdt_install, .feed = ing_wdt_feed,
};

#define ING_WDT_COMMON(inst, kind, regbase, aonbase, hz) \
	static struct ing_wdt_data ing_wdt_data_##kind##_##inst; \
	static const struct ing_wdt_config ing_wdt_cfg_##kind##_##inst = { \
		.base = regbase, .aon = aonbase, .clock = hz, .layout = kind }; \
	DEVICE_DT_INST_DEFINE(inst, ing_wdt_init, NULL, &ing_wdt_data_##kind##_##inst, \
		&ing_wdt_cfg_##kind##_##inst, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &ing_wdt_api)

#define DT_DRV_COMPAT ingchips_ing916_wdt
#define ING916_WDT(inst) ING_WDT_COMMON(inst, ING_WDT_916, DT_INST_REG_ADDR_BY_NAME(inst, wdt), \
	DT_INST_REG_ADDR_BY_NAME(inst, aon), 32768U)
DT_INST_FOREACH_STATUS_OKAY(ING916_WDT)
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT ingchips_ing918_wdt
#define ING918_WDT(inst) ING_WDT_COMMON(inst, ING_WDT_918, DT_INST_REG_ADDR(inst), 0, \
	DT_INST_PROP(inst, input_clock_frequency))
DT_INST_FOREACH_STATUS_OKAY(ING918_WDT)
