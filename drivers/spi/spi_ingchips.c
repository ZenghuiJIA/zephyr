/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <clock_control.h>
#define LOG_LEVEL CONFIG_SPI_LOG_LEVEL
LOG_MODULE_REGISTER(spi_ingchips);
#include "spi_context.h"

enum ing_spi_layout { ING_SPI_916, ING_SPI_918 };
struct ing_spi_cfg { uintptr_t base; uint32_t pclk; enum ing_spi_layout layout; };
struct ing_spi_data { struct spi_context ctx; uint8_t dfs; };

static bool ing_spi_wait(uintptr_t addr, uint32_t mask, bool set)
{
	int64_t end = k_uptime_get() + CONFIG_SPI_INGCHIPS_TRANSFER_TIMEOUT_MS;
	do {
		if (!!(sys_read32(addr) & mask) == set) { return true; }
		k_busy_wait(1);
	} while (k_uptime_get() <= end);
	return false;
}

#define S916_FMT 0x10
#define S916_CTRL 0x20
#define S916_DATA 0x2c
#define S916_GCTRL 0x30
#define S916_STATUS 0x34
#define S916_TIMING 0x40
#define S916_ACTIVE BIT(0)
#define S916_RX_EMPTY BIT(14)
#define S916_TX_FULL BIT(23)
#define S916_RX_RST BIT(1)
#define S916_TX_RST BIT(2)

#define S918_CR0 0x00
#define S918_CR1 0x04
#define S918_DATA 0x08
#define S918_STATUS 0x0c
#define S918_PRESCALE 0x10
#define S918_TNF BIT(1)
#define S918_RNE BIT(2)
#define S918_BUSY BIT(4)
#define S918_ENABLE BIT(1)

static int ing_spi_config(const struct device *dev, const struct spi_config *cfg)
{
	const struct ing_spi_cfg *c = dev->config;
	struct ing_spi_data *d = dev->data;
	uint32_t bits = SPI_WORD_SIZE_GET(cfg->operation);

	if (cfg->operation & (SPI_OP_MODE_SLAVE | SPI_HALF_DUPLEX | SPI_MODE_LOOP)) {
		return -ENOTSUP;
	}
	if (IS_ENABLED(CONFIG_SPI_EXTENDED_MODES) &&
	    (cfg->operation & SPI_LINES_MASK) != SPI_LINES_SINGLE) {
		return -ENOTSUP;
	}
	if ((bits != 8U && bits != 16U) || !cfg->frequency || cfg->frequency > c->pclk / 2U) {
		return -EINVAL;
	}
	d->dfs = bits / 8U;
	if (c->layout == ING_SPI_916) {
		uint32_t div = DIV_ROUND_UP(c->pclk, cfg->frequency);
		uint32_t fmt = ((bits - 1U) << 8);
		if (cfg->operation & SPI_MODE_CPHA) { fmt |= BIT(0); }
		if (cfg->operation & SPI_MODE_CPOL) { fmt |= BIT(1); }
		if (cfg->operation & SPI_TRANSFER_LSB) { fmt |= BIT(3); }
		sys_write32(S916_RX_RST | S916_TX_RST, c->base + S916_GCTRL);
		sys_write32(fmt, c->base + S916_FMT);
		sys_write32(0, c->base + S916_CTRL);
		sys_write32(MAX(div, 2U) - 1U, c->base + S916_TIMING);
	} else {
		uint32_t prescale = CLAMP(DIV_ROUND_UP(c->pclk, cfg->frequency), 2U, 254U);
		if (cfg->operation & SPI_TRANSFER_LSB) { return -ENOTSUP; }
		prescale = ROUND_UP(prescale, 2U);
		sys_write32(0, c->base + S918_CR1);
		sys_write32((bits - 1U) |
			((cfg->operation & SPI_MODE_CPOL) ? BIT(6) : 0) |
			((cfg->operation & SPI_MODE_CPHA) ? BIT(7) : 0), c->base + S918_CR0);
		sys_write32(prescale, c->base + S918_PRESCALE);
		sys_write32(S918_ENABLE, c->base + S918_CR1);
	}
	return 0;
}

static uint32_t ing_spi_tx(struct spi_context *ctx, uint8_t dfs)
{
	uint16_t value;

	if (!spi_context_tx_buf_on(ctx)) { return 0; }
	if (dfs == 1U) { return *(const uint8_t *)ctx->tx_buf; }
	memcpy(&value, ctx->tx_buf, sizeof(value));
	return value;
}

static void ing_spi_rx(struct spi_context *ctx, uint8_t dfs, uint32_t v)
{
	uint16_t value = v;

	if (!spi_context_rx_buf_on(ctx)) { return; }
	if (dfs == 1U) { *(uint8_t *)ctx->rx_buf = v; }
	else { memcpy(ctx->rx_buf, &value, sizeof(value)); }
}

static int ing_spi_transceive(const struct device *dev, const struct spi_config *cfg,
			      const struct spi_buf_set *tx, const struct spi_buf_set *rx)
{
	const struct ing_spi_cfg *c = dev->config;
	struct ing_spi_data *d = dev->data;
	int ret;

	spi_context_lock(&d->ctx, false, NULL, NULL, cfg);
	ret = ing_spi_config(dev, cfg);
	if (ret) { goto out; }
	spi_context_buffers_setup(&d->ctx, tx, rx, d->dfs);
	spi_context_cs_control(&d->ctx, true);
	while (spi_context_tx_on(&d->ctx) || spi_context_rx_on(&d->ctx)) {
		uint32_t v = ing_spi_tx(&d->ctx, d->dfs);
		if (c->layout == ING_SPI_916) {
			if (!ing_spi_wait(c->base + S916_STATUS, S916_TX_FULL, false)) {
				ret = -ETIMEDOUT; break;
			}
			sys_write32(v, c->base + S916_DATA);
			if (!ing_spi_wait(c->base + S916_STATUS, S916_RX_EMPTY, false)) {
				ret = -ETIMEDOUT; break;
			}
			v = sys_read32(c->base + S916_DATA);
		} else {
			if (!ing_spi_wait(c->base + S918_STATUS, S918_TNF, true)) {
				ret = -ETIMEDOUT; break;
			}
			sys_write32(v, c->base + S918_DATA);
			if (!ing_spi_wait(c->base + S918_STATUS, S918_RNE, true)) {
				ret = -ETIMEDOUT; break;
			}
			v = sys_read32(c->base + S918_DATA);
		}
		ing_spi_rx(&d->ctx, d->dfs, v);
		spi_context_update_tx(&d->ctx, d->dfs, 1);
		spi_context_update_rx(&d->ctx, d->dfs, 1);
	}
	if (!ret && c->layout == ING_SPI_916 &&
	    !ing_spi_wait(c->base + S916_STATUS, S916_ACTIVE, false)) {
		ret = -ETIMEDOUT;
	} else if (!ret && c->layout == ING_SPI_918 &&
		   !ing_spi_wait(c->base + S918_STATUS, S918_BUSY, false)) {
		ret = -ETIMEDOUT;
	}
	spi_context_cs_control(&d->ctx, false);
out:
	spi_context_release(&d->ctx, ret);
	return ret;
}

static int ing_spi_release(const struct device *dev, const struct spi_config *cfg)
{
	struct ing_spi_data *d = dev->data;
	ARG_UNUSED(cfg);
	spi_context_unlock_unconditionally(&d->ctx);
	return 0;
}

static int ing_spi_init(const struct device *dev)
{
	const struct ing_spi_cfg *c = dev->config;
	struct ing_spi_data *d = dev->data;
	ingchips_clock_enable(c->base == 0x40060000U ? INGCHIPS_CLK_SPI0 : INGCHIPS_CLK_SPI1);
	spi_context_unlock_unconditionally(&d->ctx);
	return 0;
}

static DEVICE_API(spi, ing_spi_api) = { .transceive = ing_spi_transceive, .release = ing_spi_release };

#define ING_SPI_DEFINE(inst, kind) \
	static struct ing_spi_data ing_spi_data_##kind##_##inst = { \
		SPI_CONTEXT_INIT_LOCK(ing_spi_data_##kind##_##inst, ctx), \
		SPI_CONTEXT_INIT_SYNC(ing_spi_data_##kind##_##inst, ctx), \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(inst), ctx) }; \
	static const struct ing_spi_cfg ing_spi_cfg_##kind##_##inst = { \
		.base = DT_INST_REG_ADDR(inst), .pclk = DT_INST_PROP(inst, input_clock_frequency), \
		.layout = kind }; \
	SPI_DEVICE_DT_INST_DEFINE(inst, ing_spi_init, NULL, &ing_spi_data_##kind##_##inst, \
		&ing_spi_cfg_##kind##_##inst, POST_KERNEL, CONFIG_SPI_INIT_PRIORITY, &ing_spi_api)

#define DT_DRV_COMPAT ingchips_ing916_spi
#define ING916_SPI(inst) ING_SPI_DEFINE(inst, ING_SPI_916)
DT_INST_FOREACH_STATUS_OKAY(ING916_SPI)
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT ingchips_ing918_spi
#define ING918_SPI(inst) ING_SPI_DEFINE(inst, ING_SPI_918)
DT_INST_FOREACH_STATUS_OKAY(ING918_SPI)
