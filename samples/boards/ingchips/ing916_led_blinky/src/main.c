/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define BLINK_INTERVAL_MS 500

static const struct gpio_dt_spec leds[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
};

int main(void)
{
	int ret;

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		if (!gpio_is_ready_dt(&leds[i])) {
			printk("LED%u GPIO controller is not ready\n", (unsigned int)i);
			return 0;
		}

		ret = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			printk("Failed to configure LED%u: %d\n", (unsigned int)i, ret);
			return 0;
		}
	}

	printk("Blinking ING916 IO22 and IO23\n");

	while (true) {
		for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
			ret = gpio_pin_toggle_dt(&leds[i]);
			if (ret < 0) {
				printk("Failed to toggle LED%u: %d\n", (unsigned int)i, ret);
				return 0;
			}
		}

		k_msleep(BLINK_INTERVAL_MS);
	}

	return 0;
}
