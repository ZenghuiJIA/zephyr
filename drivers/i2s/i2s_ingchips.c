/* SPDX-License-Identifier: Apache-2.0 */
#define DT_DRV_COMPAT ingchips_ing916_i2s
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <clock_control.h>

#define MODE 0x00
#define CLKDIV 0x04
#define CONFIG 0x08
#define TX 0x0c
#define RX 0x10
#define STATUS 0x18
#define FIFO_STATUS 0x1c
#define TX_ENABLE BIT(0)
#define RX_ENABLE BIT(1)
#define TX_FIFO_COUNT GENMASK(4, 0)
#define RX_FIFO_COUNT GENMASK(12, 8)

struct ing_i2s_config { uintptr_t base; uint32_t clock; };
struct ing_i2s_data {
	struct k_mutex lock;
	struct i2s_config tx_cfg;
	struct i2s_config rx_cfg;
	bool tx_configured;
	bool rx_configured;
	bool tx_running;
	bool rx_running;
};

static bool ing_i2s_wait(const struct ing_i2s_config *c, bool tx, int32_t timeout_ms)
{
	int64_t end = timeout_ms < 0 ? INT64_MAX : k_uptime_get() + timeout_ms;
	do {
		uint32_t status = sys_read32(c->base + FIFO_STATUS);
		uint32_t count = tx ? FIELD_GET(TX_FIFO_COUNT, status) :
			FIELD_GET(RX_FIFO_COUNT, status);

		if ((tx && count < 16U) || (!tx && count > 0U)) { return true; }
		k_busy_wait(1);
	} while (k_uptime_get() <= end);
	return false;
}

static int ing_i2s_configure(const struct device *dev, enum i2s_dir dir,
			     const struct i2s_config *cfg)
{
	const struct ing_i2s_config *c = dev->config;
	struct ing_i2s_data *d = dev->data;
	uint32_t format, role, frame_bits, bclk_div;
	bool *configured;

	if (dir == I2S_DIR_TX) { configured = &d->tx_configured; }
	else if (dir == I2S_DIR_RX) { configured = &d->rx_configured; }
	else { return -ENOTSUP; }
	if (cfg->frame_clk_freq == 0U) { *configured = false; return 0; }
	if (cfg->channels != 2U || (cfg->word_size != 16U && cfg->word_size != 24U &&
	    cfg->word_size != 32U) || cfg->mem_slab == NULL || cfg->block_size == 0U) {
		return -EINVAL;
	}
	format = cfg->format & I2S_FMT_DATA_FORMAT_MASK;
	if (format == I2S_FMT_DATA_FORMAT_I2S) { format = 0; }
	else if (format == I2S_FMT_DATA_FORMAT_LEFT_JUSTIFIED) { format = 1; }
	else { return -ENOTSUP; }
	if ((cfg->options & (I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER)) ==
	    (I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER)) { role = 0; }
	else if (!(cfg->options & (I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER))) { role = 1; }
	else { return -ENOTSUP; }
	frame_bits = cfg->word_size * cfg->channels;
	bclk_div = DIV_ROUND_CLOSEST(c->clock, cfg->frame_clk_freq * frame_bits);
	if (bclk_div == 0U || bclk_div > 256U || frame_bits > 256U) { return -EINVAL; }

	k_mutex_lock(&d->lock, K_FOREVER);
	sys_write32((bclk_div - 1U) << 8 | (frame_bits - 1U), c->base + CLKDIV);
	sys_write32((role << 0) |
		((cfg->format & I2S_FMT_CLK_FORMAT_MASK) == I2S_FMT_CLK_NF_IB ? BIT(2) : 0) |
		((cfg->word_size <= 16U ? 0U : cfg->word_size - 1U) << 8), c->base + CONFIG);
	sys_write32(format << 2, c->base + MODE);
	if (dir == I2S_DIR_TX) { d->tx_cfg = *cfg; }
	else { d->rx_cfg = *cfg; }
	*configured = true;
	k_mutex_unlock(&d->lock);
	return 0;
}

static const struct i2s_config *ing_i2s_config_get(const struct device *dev, enum i2s_dir dir)
{
	struct ing_i2s_data *d = dev->data;
	if (dir == I2S_DIR_TX && d->tx_configured) { return &d->tx_cfg; }
	if (dir == I2S_DIR_RX && d->rx_configured) { return &d->rx_cfg; }
	return NULL;
}

static int ing_i2s_trigger(const struct device *dev, enum i2s_dir dir, enum i2s_trigger_cmd cmd)
{
	const struct ing_i2s_config *c = dev->config;
	struct ing_i2s_data *d = dev->data;
	bool *running;
	uint32_t bit, v;
	if (dir == I2S_DIR_TX) { running = &d->tx_running; bit = TX_ENABLE; }
	else if (dir == I2S_DIR_RX) { running = &d->rx_running; bit = RX_ENABLE; }
	else { return -ENOTSUP; }
	if (cmd == I2S_TRIGGER_START) { *running = true; }
	else if (cmd == I2S_TRIGGER_STOP || cmd == I2S_TRIGGER_DROP || cmd == I2S_TRIGGER_DRAIN) {
		*running = false;
	} else if (cmd != I2S_TRIGGER_PREPARE) { return -ENOTSUP; }
	v = sys_read32(c->base + MODE);
	sys_write32(*running ? v | bit : v & ~bit, c->base + MODE);
	return 0;
}

static int ing_i2s_write(const struct device *dev, void *block, size_t size)
{
	const struct ing_i2s_config *c = dev->config;
	struct ing_i2s_data *d = dev->data;
	uint32_t *words = block;
	if (!d->tx_running || !d->tx_configured || size > d->tx_cfg.block_size || (size & 3U)) {
		return -EIO;
	}
	for (size_t i = 0; i < size / 4U; i++) {
		if (!ing_i2s_wait(c, true, d->tx_cfg.timeout)) {
			k_mem_slab_free(d->tx_cfg.mem_slab, block);
			return -ETIMEDOUT;
		}
		sys_write32(words[i], c->base + TX);
	}
	k_mem_slab_free(d->tx_cfg.mem_slab, block);
	return 0;
}

static int ing_i2s_read(const struct device *dev, void **block, size_t *size)
{
	const struct ing_i2s_config *c = dev->config;
	struct ing_i2s_data *d = dev->data;
	uint32_t *words;
	int ret;
	if (!d->rx_running || !d->rx_configured) { return -EIO; }
	ret = k_mem_slab_alloc(d->rx_cfg.mem_slab, block,
		d->rx_cfg.timeout < 0 ? K_FOREVER : K_MSEC(d->rx_cfg.timeout));
	if (ret) { return ret; }
	words = *block;
	for (size_t i = 0; i < d->rx_cfg.block_size / 4U; i++) {
		if (!ing_i2s_wait(c, false, d->rx_cfg.timeout)) {
			k_mem_slab_free(d->rx_cfg.mem_slab, *block); return -ETIMEDOUT;
		}
		words[i] = sys_read32(c->base + RX);
	}
	*size = d->rx_cfg.block_size;
	return 0;
}

static int ing_i2s_init(const struct device *dev)
{
	const struct ing_i2s_config *c = dev->config;
	struct ing_i2s_data *d = dev->data;
	ingchips_clock_enable(INGCHIPS_CLK_I2S);
	k_mutex_init(&d->lock);
	sys_write32(BIT(6) | BIT(7), c->base + CONFIG);
	sys_write32(0, c->base + MODE);
	sys_write32(3, c->base + STATUS);
	return 0;
}

static DEVICE_API(i2s, ing_i2s_api) = {
	.configure = ing_i2s_configure, .config_get = ing_i2s_config_get,
	.trigger = ing_i2s_trigger, .read = ing_i2s_read, .write = ing_i2s_write,
};

#define ING_I2S_DEFINE(inst) \
	static struct ing_i2s_data ing_i2s_data_##inst; \
	static const struct ing_i2s_config ing_i2s_cfg_##inst = { \
		.base = DT_INST_REG_ADDR(inst), .clock = DT_INST_PROP(inst, input_clock_frequency) }; \
	DEVICE_DT_INST_DEFINE(inst, ing_i2s_init, NULL, &ing_i2s_data_##inst, &ing_i2s_cfg_##inst, \
		POST_KERNEL, CONFIG_I2S_INIT_PRIORITY, &ing_i2s_api)
DT_INST_FOREACH_STATUS_OKAY(ING_I2S_DEFINE)
