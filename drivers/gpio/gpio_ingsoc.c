/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/sys/sys_io.h>

enum gpio_ingsoc_layout {
	GPIO_INGSOC_916,
	GPIO_INGSOC_918,
};

struct gpio_ingsoc_config {
	struct gpio_driver_config common;
	mem_addr_t base;
	enum gpio_ingsoc_layout layout;
};
struct gpio_ingsoc_data { struct gpio_driver_data common; };

static inline uint32_t gpio_input_offset(const struct gpio_ingsoc_config *cfg)
{
	return cfg->layout == GPIO_INGSOC_916 ? 0x20U : 0x00U;
}

static inline uint32_t gpio_output_offset(const struct gpio_ingsoc_config *cfg)
{
	return cfg->layout == GPIO_INGSOC_916 ? 0x24U : 0x10U;
}

static int gpio_ingsoc_pin_configure(const struct device *dev, gpio_pin_t pin,
				     gpio_flags_t flags)
{
	const struct gpio_ingsoc_config *cfg = dev->config;
	uint32_t mask;
	uint32_t direction_offset = cfg->layout == GPIO_INGSOC_916 ? 0x28U : 0x20U;
	uint32_t pull_enable_offset = cfg->layout == GPIO_INGSOC_916 ? 0x40U : 0x60U;
	uint32_t pull_type_offset = cfg->layout == GPIO_INGSOC_916 ? 0x44U : 0x70U;
	uint32_t direction;
	uint32_t pull_enable;
	uint32_t pull_type;

	if (pin >= 32U) {
		return -EINVAL;
	}
	mask = BIT(pin);
	if ((cfg->common.port_pin_mask & mask) == 0U) {
		return -EINVAL;
	}
	if ((flags & (GPIO_SINGLE_ENDED | GPIO_DISCONNECTED)) != 0U) {
		return -ENOTSUP;
	}
	direction = sys_read32(cfg->base + direction_offset);
	pull_enable = sys_read32(cfg->base + pull_enable_offset);
	pull_type = sys_read32(cfg->base + pull_type_offset);
	if ((flags & GPIO_OUTPUT) != 0U) {
		if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
			sys_write32(mask, cfg->base +
				    (cfg->layout == GPIO_INGSOC_916 ? 0x30U : 0x14U));
		} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
			sys_write32(mask, cfg->base +
				    (cfg->layout == GPIO_INGSOC_916 ? 0x2cU : 0x18U));
		}
		direction = cfg->layout == GPIO_INGSOC_916 ? direction | mask : direction & ~mask;
	} else if ((flags & GPIO_INPUT) != 0U) {
		direction = cfg->layout == GPIO_INGSOC_916 ? direction & ~mask : direction | mask;
	} else {
		return -ENOTSUP;
	}
	sys_write32(direction, cfg->base + direction_offset);
	if ((flags & GPIO_PULL_UP) != 0U) {
		pull_enable |= mask;
		pull_type |= mask;
	} else if ((flags & GPIO_PULL_DOWN) != 0U) {
		pull_enable |= mask;
		pull_type &= ~mask;
	} else {
		pull_enable &= ~mask;
	}
	sys_write32(pull_type, cfg->base + pull_type_offset);
	sys_write32(pull_enable, cfg->base + pull_enable_offset);
	return 0;
}

static int gpio_ingsoc_port_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	const struct gpio_ingsoc_config *cfg = dev->config;
	*value = sys_read32(cfg->base + gpio_input_offset(cfg));
	return 0;
}

static int gpio_ingsoc_port_set_masked_raw(const struct device *dev,
					   gpio_port_pins_t mask,
					   gpio_port_value_t value)
{
	const struct gpio_ingsoc_config *cfg = dev->config;
	uint32_t output = sys_read32(cfg->base + gpio_output_offset(cfg));

	sys_write32((output & ~mask) | (value & mask), cfg->base + gpio_output_offset(cfg));
	return 0;
}

static int gpio_ingsoc_port_set_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_ingsoc_config *cfg = dev->config;
	sys_write32(pins, cfg->base + (cfg->layout == GPIO_INGSOC_916 ? 0x30U : 0x14U));
	return 0;
}

static int gpio_ingsoc_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_ingsoc_config *cfg = dev->config;
	sys_write32(pins, cfg->base + (cfg->layout == GPIO_INGSOC_916 ? 0x2cU : 0x18U));
	return 0;
}

static int gpio_ingsoc_port_toggle_bits(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_ingsoc_config *cfg = dev->config;
	if (cfg->layout == GPIO_INGSOC_918) {
		sys_write32(pins, cfg->base + 0x1cU);
	} else {
		uint32_t output = sys_read32(cfg->base + gpio_output_offset(cfg));

		sys_write32(output ^ pins, cfg->base + gpio_output_offset(cfg));
	}
	return 0;
}

static int gpio_ingsoc_pin_interrupt_configure(const struct device *dev, gpio_pin_t pin,
					       enum gpio_int_mode mode,
					       enum gpio_int_trig trig)
{
	ARG_UNUSED(dev); ARG_UNUSED(pin); ARG_UNUSED(mode); ARG_UNUSED(trig);
	return -ENOTSUP;
}

static DEVICE_API(gpio, gpio_ingsoc_api) = {
	.pin_configure = gpio_ingsoc_pin_configure,
	.port_get_raw = gpio_ingsoc_port_get_raw,
	.port_set_masked_raw = gpio_ingsoc_port_set_masked_raw,
	.port_set_bits_raw = gpio_ingsoc_port_set_bits_raw,
	.port_clear_bits_raw = gpio_ingsoc_port_clear_bits_raw,
	.port_toggle_bits = gpio_ingsoc_port_toggle_bits,
	.pin_interrupt_configure = gpio_ingsoc_pin_interrupt_configure,
};

#define GPIO_INGSOC_INIT(inst, kind)                                        \
	static const struct gpio_ingsoc_config gpio_ingsoc_config_##kind##_##inst = { \
		.common = {.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(inst)}, \
		.base = DT_INST_REG_ADDR(inst),                                \
		.layout = kind,                                                \
	};                                                                       \
	static struct gpio_ingsoc_data gpio_ingsoc_data_##kind##_##inst;         \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL,                                  \
			      &gpio_ingsoc_data_##kind##_##inst,                 \
			      &gpio_ingsoc_config_##kind##_##inst, POST_KERNEL,   \
			      CONFIG_GPIO_INIT_PRIORITY, &gpio_ingsoc_api);

#define DT_DRV_COMPAT ingchips_ing916_gpio
#define GPIO_INGSOC_916_INIT(inst) GPIO_INGSOC_INIT(inst, GPIO_INGSOC_916)
DT_INST_FOREACH_STATUS_OKAY(GPIO_INGSOC_916_INIT)
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT ingchips_ing918_gpio
#define GPIO_INGSOC_918_INIT(inst) GPIO_INGSOC_INIT(inst, GPIO_INGSOC_918)
DT_INST_FOREACH_STATUS_OKAY(GPIO_INGSOC_918_INIT)
