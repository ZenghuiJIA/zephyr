/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#define I2C916_STATUS       0x18
#define I2C916_ADDR         0x1c
#define I2C916_DATA         0x20
#define I2C916_CTRL         0x24
#define I2C916_CMD          0x28
#define I2C916_SETUP        0x2c
#define I2C916_TPM          0x30
#define I2C916_CMPL         BIT(9)
#define I2C916_NACK         BIT(10)
#define I2C916_ARB_LOST     BIT(4)
#define I2C916_FIFO_EMPTY   BIT(0)
#define I2C916_FIFO_FULL    BIT(1)
#define I2C916_PHASE_START  BIT(12)
#define I2C916_PHASE_ADDR   BIT(11)
#define I2C916_PHASE_DATA   BIT(10)
#define I2C916_PHASE_STOP   BIT(9)
#define I2C916_DIR_READ     BIT(8)

#define I2C918_CTRL0        0x00
#define I2C918_TIMING0      0x10
#define I2C918_TIMING1      0x20
#define I2C918_TIMING2      0x30
#define I2C918_CTRL1        0x40
#define I2C918_STAT         0x50
#define I2C918_DATA         0xa0
#define I2C918_RUN          BIT(29)
#define I2C918_MASTER       BIT(17)
#define I2C918_ADDR_EN      BIT(18)
#define I2C918_START        BIT(19)
#define I2C918_STOP         BIT(20)
#define I2C918_DIR_READ     BIT(16)
#define I2C918_NAK_LAST     BIT(25)
#define I2C918_DONE         BIT(6)
#define I2C918_ERRORS       (BIT(2) | BIT(3) | BIT(4) | BIT(5))

enum ing_i2c_layout { ING_I2C_916, ING_I2C_918 };

struct ing_i2c_config {
	uintptr_t base;
	uint32_t pclk;
	enum ing_i2c_layout layout;
};

struct ing_i2c_data {
	struct k_mutex lock;
	uint32_t config;
};

static bool wait_mask(uintptr_t addr, uint32_t mask, bool set)
{
	int64_t end = k_uptime_get() + CONFIG_I2C_TRANSFER_TIMEOUT_MS;

	do {
		if (!!(sys_read32(addr) & mask) == set) {
			return true;
		}
		k_busy_wait(1);
	} while (k_uptime_get() <= end);
	return false;
}

static int ing_i2c916_timing(uintptr_t b, uint32_t pclk, uint32_t rate)
{
	uint32_t period = DIV_ROUND_UP(pclk, rate);
	uint32_t tpm = MAX(period / 512U, 1U) - 1U;
	uint32_t high = period / (2U * (tpm + 1U));

	if (tpm > 31U || high < 3U || high > 255U) {
		return -EINVAL;
	}
	sys_write32(tpm, b + I2C916_TPM);
	sys_write32((sys_read32(b + I2C916_SETUP) & 0xfU) |
		    ((high - 2U) << 4) | (1U << 21) | (5U << 24),
		    b + I2C916_SETUP);
	return 0;
}

static int ing_i2c_configure(const struct device *dev, uint32_t cfg)
{
	const struct ing_i2c_config *c = dev->config;
	struct ing_i2c_data *d = dev->data;
	uint32_t rate;
	int ret = 0;

	if (!(cfg & I2C_MODE_CONTROLLER) || (cfg & I2C_ADDR_10_BITS)) {
		return -ENOTSUP;
	}
	switch (I2C_SPEED_GET(cfg)) {
	case I2C_SPEED_STANDARD: rate = 100000U; break;
	case I2C_SPEED_FAST: rate = 400000U; break;
	case I2C_SPEED_FAST_PLUS: rate = 1000000U; break;
	default: return -ENOTSUP;
	}

	if (c->layout == ING_I2C_916) {
		sys_write32(BIT(2) | BIT(0), c->base + I2C916_SETUP);
		ret = ing_i2c916_timing(c->base, c->pclk, rate);
	} else {
		uint32_t half = MAX(DIV_ROUND_UP(c->pclk, rate * 2U), 2U);
		if (half > 0x3ffU) {
			return -EINVAL;
		}
		sys_write32((half << 16) | half, c->base + I2C918_TIMING0);
		sys_write32((half << 16) | half, c->base + I2C918_TIMING1);
		sys_write32((half << 16) | half, c->base + I2C918_TIMING2);
		sys_write32(0, c->base + I2C918_CTRL1);
	}
	if (!ret) {
		d->config = cfg;
	}
	return ret;
}

static int ing_i2c916_msg(const struct ing_i2c_config *c, struct i2c_msg *m,
			  uint16_t addr)
{
	uint32_t ctrl = m->len | I2C916_PHASE_START | I2C916_PHASE_ADDR |
			I2C916_PHASE_DATA;

	if (m->len > UINT8_MAX) {
		return -EMSGSIZE;
	}
	if (m->flags & I2C_MSG_READ) {
		ctrl |= I2C916_DIR_READ;
	}
	if (m->flags & I2C_MSG_STOP) {
		ctrl |= I2C916_PHASE_STOP;
	}
	sys_write32(addr, c->base + I2C916_ADDR);
	sys_write32(I2C916_CMPL | GENMASK(8, 3), c->base + I2C916_STATUS);
	sys_write32(ctrl, c->base + I2C916_CTRL);

	if (!(m->flags & I2C_MSG_READ)) {
		for (size_t i = 0; i < m->len; i++) {
			if (!wait_mask(c->base + I2C916_STATUS, I2C916_FIFO_FULL, false)) {
				return -ETIMEDOUT;
			}
			sys_write32(m->buf[i], c->base + I2C916_DATA);
		}
	}
	sys_write32(1, c->base + I2C916_CMD);
	if (m->flags & I2C_MSG_READ) {
		for (size_t i = 0; i < m->len; i++) {
			if (!wait_mask(c->base + I2C916_STATUS, I2C916_FIFO_EMPTY, false)) {
				return -ETIMEDOUT;
			}
			m->buf[i] = sys_read32(c->base + I2C916_DATA);
		}
	}
	if (!wait_mask(c->base + I2C916_STATUS, I2C916_CMPL, true)) {
		return -ETIMEDOUT;
	}
	return (sys_read32(c->base + I2C916_STATUS) & (I2C916_NACK | I2C916_ARB_LOST)) ?
		-EIO : 0;
}

static int ing_i2c918_msg(const struct ing_i2c_config *c, struct i2c_msg *m,
			  uint16_t addr)
{
	uint32_t ctrl = m->len | I2C918_MASTER | I2C918_ADDR_EN | I2C918_START;

	if (m->len > UINT16_MAX) {
		return -EMSGSIZE;
	}
	if (m->flags & I2C_MSG_READ) {
		ctrl |= I2C918_DIR_READ | I2C918_NAK_LAST;
	} else {
		for (size_t i = 0; i < m->len; i++) {
			sys_write32(m->buf[i], c->base + I2C918_DATA);
		}
	}
	if (m->flags & I2C_MSG_STOP) {
		ctrl |= I2C918_STOP;
	}
	/* The master address byte occupies CTRL1[23:16] in PIO mode. */
	sys_write32(((addr << 1) | !!(m->flags & I2C_MSG_READ)) << 16,
		    c->base + I2C918_CTRL1);
	sys_write32(ctrl | I2C918_RUN, c->base + I2C918_CTRL0);
	if (!wait_mask(c->base + I2C918_STAT, I2C918_DONE, true)) {
		return -ETIMEDOUT;
	}
	if (sys_read32(c->base + I2C918_STAT) & I2C918_ERRORS) {
		return -EIO;
	}
	if (m->flags & I2C_MSG_READ) {
		for (size_t i = 0; i < m->len; i++) {
			m->buf[i] = sys_read32(c->base + I2C918_DATA);
		}
	}
	return 0;
}

static int ing_i2c_transfer(const struct device *dev, struct i2c_msg *msgs,
			    uint8_t num, uint16_t addr)
{
	const struct ing_i2c_config *c = dev->config;
	struct ing_i2c_data *d = dev->data;
	int ret = 0;

	if (!num || addr > 0x7fU) {
		return -EINVAL;
	}
	k_mutex_lock(&d->lock, K_FOREVER);
	for (uint8_t i = 0; i < num && !ret; i++) {
		ret = c->layout == ING_I2C_916 ? ing_i2c916_msg(c, &msgs[i], addr) :
			ing_i2c918_msg(c, &msgs[i], addr);
	}
	k_mutex_unlock(&d->lock);
	return ret;
}

static int ing_i2c_get_config(const struct device *dev, uint32_t *cfg)
{
	*cfg = ((struct ing_i2c_data *)dev->data)->config;
	return 0;
}

static int ing_i2c_init(const struct device *dev)
{
	struct ing_i2c_data *d = dev->data;
	k_mutex_init(&d->lock);
	return ing_i2c_configure(dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
}

static DEVICE_API(i2c, ing_i2c_api) = {
	.configure = ing_i2c_configure,
	.get_config = ing_i2c_get_config,
	.transfer = ing_i2c_transfer,
};

#define ING_I2C_DEFINE(inst, compat_layout) \
	static struct ing_i2c_data ing_i2c_data_##compat_layout##_##inst; \
	static const struct ing_i2c_config ing_i2c_cfg_##compat_layout##_##inst = { \
		.base = DT_INST_REG_ADDR(inst), \
		.pclk = DT_INST_PROP(inst, input_clock_frequency), \
		.layout = compat_layout, \
	}; \
	I2C_DEVICE_DT_INST_DEFINE(inst, ing_i2c_init, NULL, \
		&ing_i2c_data_##compat_layout##_##inst, &ing_i2c_cfg_##compat_layout##_##inst, \
		POST_KERNEL, CONFIG_I2C_INIT_PRIORITY, &ing_i2c_api)

#define DT_DRV_COMPAT ingchips_ing916_i2c
#define ING_I2C916_DEFINE(inst) ING_I2C_DEFINE(inst, ING_I2C_916)
DT_INST_FOREACH_STATUS_OKAY(ING_I2C916_DEFINE)
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT ingchips_ing918_i2c
#define ING_I2C918_DEFINE(inst) ING_I2C_DEFINE(inst, ING_I2C_918)
DT_INST_FOREACH_STATUS_OKAY(ING_I2C918_DEFINE)
