/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZEPHYR_SOC_INGCHIPS_COMMON_CLOCK_CONTROL_H_
#define ZEPHYR_SOC_INGCHIPS_COMMON_CLOCK_CONTROL_H_

#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

enum ingchips_clock_id {
	INGCHIPS_CLK_GPIO0,
	INGCHIPS_CLK_GPIO1,
	INGCHIPS_CLK_TIMER0,
	INGCHIPS_CLK_TIMER1,
	INGCHIPS_CLK_TIMER2,
	INGCHIPS_CLK_WDT,
	INGCHIPS_CLK_PWM,
	INGCHIPS_CLK_DMA,
	INGCHIPS_CLK_SPI0,
	INGCHIPS_CLK_SPI1,
	INGCHIPS_CLK_ADC,
	INGCHIPS_CLK_I2S,
	INGCHIPS_CLK_UART0,
	INGCHIPS_CLK_UART1,
	INGCHIPS_CLK_I2C0,
	INGCHIPS_CLK_I2C1,
};

static inline void ingchips_clock_enable(enum ingchips_clock_id id)
{
#if defined(CONFIG_SOC_SERIES_ING918)
	static const uint8_t bits[] = {
		[INGCHIPS_CLK_GPIO0] = 13, [INGCHIPS_CLK_GPIO1] = 13,
		[INGCHIPS_CLK_TIMER0] = 6, [INGCHIPS_CLK_TIMER1] = 7,
		[INGCHIPS_CLK_TIMER2] = 8, [INGCHIPS_CLK_WDT] = 6,
		[INGCHIPS_CLK_PWM] = 16, [INGCHIPS_CLK_SPI0] = 17,
		[INGCHIPS_CLK_SPI1] = 5, [INGCHIPS_CLK_UART0] = 9,
		[INGCHIPS_CLK_UART1] = 10, [INGCHIPS_CLK_I2C0] = 4,
		[INGCHIPS_CLK_I2C1] = 19,
	};

	/* ING918 has one clock-gate register at SYSCTRL + 0x00. */
	sys_set_bits(0x40070000U, BIT(bits[id]));
#else
	static const uint8_t cfg3_bits[] = {
		21, 22, 2, 3, 4, 1, 5, 12, 13, 14, 15, 16, 17, 18, 19, 20,
	};
	static const int8_t cfg5_bits[] = {
		18, 18, 0, 1, 2, 17, 10, -1, 6, 7, 12, 8, 4, 5, -1, -1,
	};

	/* ING916 and ING20 share the APB SYSCTRL clock-gate layout. */
	sys_set_bits(0x4000000cU, BIT(cfg3_bits[id]));
	if (id == INGCHIPS_CLK_DMA) {
		sys_set_bits(0x40000008U, BIT(0));
	} else if (id == INGCHIPS_CLK_SPI0) {
		sys_set_bits(0x40000008U, BIT(12));
	}
	if (cfg5_bits[id] >= 0) {
		sys_set_bits(0x40000014U, BIT(cfg5_bits[id]));
	}
#endif
}

#endif /* ZEPHYR_SOC_INGCHIPS_COMMON_CLOCK_CONTROL_H_ */
