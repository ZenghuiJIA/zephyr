/* SPDX-License-Identifier: Apache-2.0 */

#define DT_DRV_COMPAT ingchips_uart

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <clock_control.h>

#define UART_DR 0x00
#define UART_RSR 0x04
#define UART_FR 0x18
#define UART_IBRD 0x24
#define UART_FBRD 0x28
#define UART_LCR_H 0x2c
#define UART_CR 0x30
#define UART_DMACR 0x48
#define UART_FR_RXFE BIT(4)
#define UART_FR_TXFF BIT(5)
#define UART_LCR_H_FEN BIT(4)
#define UART_LCR_H_WLEN_8 (3U << 5)
#define UART_CR_UARTEN BIT(0)
#define UART_CR_TXE BIT(8)
#define UART_CR_RXE BIT(9)
#define UART_DMACR_RX_ENABLE BIT(0)
#define UART_DMACR_TX_ENABLE BIT(1)

struct uart_ingsoc_config {
	mem_addr_t base;
	uint32_t clock_frequency;
	uint32_t baudrate;
	bool dma_rx;
	bool dma_tx;
	uint8_t instance;
};

static inline uint32_t reg_read(mem_addr_t address)
{
	return sys_read32(address);
}

static inline void reg_write(uint32_t value, mem_addr_t address)
{
	sys_write32(value, address);
}

static int uart_ingsoc_poll_in(const struct device *dev, unsigned char *c)
{
	const struct uart_ingsoc_config *cfg = dev->config;

	if ((reg_read(cfg->base + UART_FR) & UART_FR_RXFE) != 0U) {
		return -1;
	}
	*c = (unsigned char)reg_read(cfg->base + UART_DR);
	return 0;
}

static void uart_ingsoc_poll_out(const struct device *dev, unsigned char c)
{
	const struct uart_ingsoc_config *cfg = dev->config;

	while ((reg_read(cfg->base + UART_FR) & UART_FR_TXFF) != 0U) {
	}
	reg_write(c, cfg->base + UART_DR);
}

static int uart_ingsoc_err_check(const struct device *dev)
{
	const struct uart_ingsoc_config *cfg = dev->config;
	uint32_t errors = reg_read(cfg->base + UART_RSR) & 0x0fU;

	if (errors != 0U) {
		reg_write(errors, cfg->base + UART_RSR);
	}
	return (int)errors;
}

static int uart_ingsoc_init(const struct device *dev)
{
	const struct uart_ingsoc_config *cfg = dev->config;
	uint32_t scaled_divisor;
	uint32_t integer_divisor;
	uint32_t fractional_divisor;

	ingchips_clock_enable(cfg->instance == 0U ? INGCHIPS_CLK_UART0 : INGCHIPS_CLK_UART1);

	if (cfg->baudrate == 0U || cfg->clock_frequency < (16U * cfg->baudrate)) {
		return -EINVAL;
	}
	scaled_divisor = (cfg->clock_frequency << 3) / cfg->baudrate;
	integer_divisor = scaled_divisor / 128U;
	fractional_divisor = ((scaled_divisor - (integer_divisor << 7)) + 1U) / 2U;
	reg_write(0U, cfg->base + UART_CR);
	reg_write(integer_divisor, cfg->base + UART_IBRD);
	reg_write(fractional_divisor, cfg->base + UART_FBRD);
	reg_write(UART_LCR_H_FEN | UART_LCR_H_WLEN_8, cfg->base + UART_LCR_H);
	reg_write((cfg->dma_rx ? UART_DMACR_RX_ENABLE : 0U) |
		  (cfg->dma_tx ? UART_DMACR_TX_ENABLE : 0U), cfg->base + UART_DMACR);
	reg_write(UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE, cfg->base + UART_CR);
	return 0;
}

static DEVICE_API(uart, uart_ingsoc_api) = {
	.poll_in = uart_ingsoc_poll_in,
	.poll_out = uart_ingsoc_poll_out,
	.err_check = uart_ingsoc_err_check,
};

#define UART_INGSOC_INIT(inst)                                              \
	static const struct uart_ingsoc_config uart_ingsoc_config_##inst = { \
		.base = DT_INST_REG_ADDR(inst),                               \
		.clock_frequency = DT_INST_PROP(inst, clock_frequency),       \
		.baudrate = DT_INST_PROP(inst, current_speed),                 \
		.dma_rx = IS_ENABLED(CONFIG_DMA_INGCHIPS) &&                    \
			  DT_DMAS_HAS_NAME(DT_DRV_INST(inst), rx),              \
		.dma_tx = IS_ENABLED(CONFIG_DMA_INGCHIPS) &&                    \
			  DT_DMAS_HAS_NAME(DT_DRV_INST(inst), tx),              \
		.instance = inst,                                                \
	};                                                                      \
	DEVICE_DT_INST_DEFINE(inst, uart_ingsoc_init, NULL, NULL,               \
			      &uart_ingsoc_config_##inst, PRE_KERNEL_1,         \
			      CONFIG_SERIAL_INIT_PRIORITY, &uart_ingsoc_api);

DT_INST_FOREACH_STATUS_OKAY(UART_INGSOC_INIT)
