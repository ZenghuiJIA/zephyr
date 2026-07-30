/* SPDX-License-Identifier: Apache-2.0 */

#define DT_DRV_COMPAT ingchips_ing916_adc

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <clock_control.h>

#define SADC_CFG0          0x00U
#define SADC_CFG1          0x04U
#define SADC_CFG2          0x08U
#define SADC_DATA          0x0cU
#define SADC_STATUS        0x10U
#define SADC_INT_MASK      0x30U
#define SADC_INT_STATUS    0x34U

#define SADC_CFG0_ENABLE   BIT(1)
#define SADC_CFG0_DIFF     BIT(8)
#define SADC_CFG0_RESET_N  BIT(28)
#define SADC_CFG2_START    BIT(2)
#define SADC_CFG2_CH_SHIFT 3U
#define SADC_CFG2_CH_MASK  GENMASK(14, 3)
#define SADC_STATUS_CLR    BIT(22)
#define SADC_STATUS_BUSY   BIT(23)
#define SADC_INT_EMPTY     BIT(0)
#define SADC_INT_READY     BIT(3)
#define SADC_RAW_MASK      GENMASK(13, 0)
#define SADC_DATA_CH_SHIFT 14U
#define SADC_MAX_CHANNELS  12U
#define SADC_TIMEOUT_US    100000U

struct adc_ingsoc_config {
	mem_addr_t base;
};

struct adc_ingsoc_data {
	struct k_mutex lock;
	uint16_t configured_channels;
	bool differential[SADC_MAX_CHANNELS];
};

static int adc_ingsoc_wait(const struct adc_ingsoc_config *cfg, mem_addr_t offset,
			   uint32_t mask, bool set)
{
	for (uint32_t elapsed = 0U; elapsed < SADC_TIMEOUT_US; elapsed++) {
		if (((sys_read32(cfg->base + offset) & mask) != 0U) == set) {
			return 0;
		}
		k_busy_wait(1U);
	}

	return -ETIMEDOUT;
}

static int adc_ingsoc_channel_setup(const struct device *dev,
				    const struct adc_channel_cfg *channel_cfg)
{
	struct adc_ingsoc_data *data = dev->data;

	if (channel_cfg->channel_id >= SADC_MAX_CHANNELS ||
	    channel_cfg->gain != ADC_GAIN_1 ||
	    channel_cfg->reference != ADC_REF_INTERNAL) {
		return -ENOTSUP;
	}

	data->configured_channels |= BIT(channel_cfg->channel_id);
	data->differential[channel_cfg->channel_id] = channel_cfg->differential;
	return 0;
}

static int adc_ingsoc_read(const struct device *dev, const struct adc_sequence *sequence)
{
	const struct adc_ingsoc_config *cfg = dev->config;
	struct adc_ingsoc_data *data = dev->data;
	uint16_t *buffer = sequence->buffer;
	uint32_t channels = sequence->channels;
	uint32_t expected = POPCOUNT(channels);
	uint32_t collected = 0U;
	uint32_t cfg0;
	uint32_t cfg2;
	bool differential;
	int ret = 0;

	if (sequence->resolution != 14U || sequence->oversampling != 0U ||
	    channels == 0U || (channels & ~GENMASK(SADC_MAX_CHANNELS - 1U, 0U)) != 0U ||
	    (channels & ~data->configured_channels) != 0U ||
	    sequence->buffer_size < expected * sizeof(*buffer)) {
		return -EINVAL;
	}

	if (sequence->options != NULL &&
	    (sequence->options->extra_samplings != 0U || sequence->options->interval_us != 0U)) {
		return -ENOTSUP;
	}
	differential = data->differential[find_lsb_set(channels) - 1U];
	for (uint32_t channel = 0U; channel < SADC_MAX_CHANNELS; channel++) {
		if ((channels & BIT(channel)) != 0U && data->differential[channel] != differential) {
			return -ENOTSUP;
		}
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	sys_write32(SADC_STATUS_CLR, cfg->base + SADC_STATUS);
	cfg0 = sys_read32(cfg->base + SADC_CFG0);
	cfg0 &= ~SADC_CFG0_DIFF;
	if (differential) {
		cfg0 |= SADC_CFG0_DIFF;
	}
	sys_write32(cfg0, cfg->base + SADC_CFG0);

	cfg2 = sys_read32(cfg->base + SADC_CFG2);
	cfg2 &= ~SADC_CFG2_CH_MASK;
	cfg2 |= channels << SADC_CFG2_CH_SHIFT;
	sys_write32(cfg2, cfg->base + SADC_CFG2);
	sys_set_bits(cfg->base + SADC_CFG0, SADC_CFG0_ENABLE);
	sys_set_bits(cfg->base + SADC_CFG2, SADC_CFG2_START);

	ret = adc_ingsoc_wait(cfg, SADC_INT_STATUS, SADC_INT_READY, true);
	if (ret != 0) {
		goto stop;
	}

	while (collected < expected &&
	       (sys_read32(cfg->base + SADC_INT_STATUS) & SADC_INT_EMPTY) == 0U) {
		uint32_t sample = sys_read32(cfg->base + SADC_DATA);
		uint32_t channel = (sample >> SADC_DATA_CH_SHIFT) & 0xfU;
		uint32_t index = 0U;

		if (channel >= SADC_MAX_CHANNELS || (channels & BIT(channel)) == 0U) {
			ret = -EIO;
			goto stop;
		}
		for (uint32_t bit = 0U; bit < channel; bit++) {
			index += (channels & BIT(bit)) != 0U;
		}
		buffer[index] = sample & SADC_RAW_MASK;
		collected++;
	}

	if (collected != expected) {
		ret = -EIO;
	}

stop:
	sys_clear_bits(cfg->base + SADC_CFG2, SADC_CFG2_START);
	(void)adc_ingsoc_wait(cfg, SADC_STATUS, SADC_STATUS_BUSY, false);
	sys_clear_bits(cfg->base + SADC_CFG0, SADC_CFG0_ENABLE);
	sys_write32(0U, cfg->base + SADC_INT_MASK);
	k_mutex_unlock(&data->lock);
	return ret;
}

static int adc_ingsoc_init(const struct device *dev)
{
	const struct adc_ingsoc_config *cfg = dev->config;
	struct adc_ingsoc_data *data = dev->data;

	ingchips_clock_enable(INGCHIPS_CLK_ADC);
	k_mutex_init(&data->lock);
	sys_clear_bits(cfg->base + SADC_CFG0, SADC_CFG0_RESET_N);
	sys_write32(0x100U, cfg->base + SADC_CFG1);
	sys_write32((4U << 16) | (4U << 20) | (0xaU << 24), cfg->base + SADC_CFG2);
	sys_write32(SADC_STATUS_CLR, cfg->base + SADC_STATUS);
	sys_write32(0U, cfg->base + SADC_INT_MASK);
	sys_set_bits(cfg->base + SADC_CFG0, SADC_CFG0_RESET_N);
	return 0;
}

static DEVICE_API(adc, adc_ingsoc_api) = {
	.channel_setup = adc_ingsoc_channel_setup,
	.read = adc_ingsoc_read,
	.ref_internal = 1200U,
};

#define ADC_INGSOC_INIT(inst)                                                    \
	static const struct adc_ingsoc_config adc_ingsoc_config_##inst = {        \
		.base = DT_INST_REG_ADDR(inst),                                     \
	};                                                                            \
	static struct adc_ingsoc_data adc_ingsoc_data_##inst;                         \
	DEVICE_DT_INST_DEFINE(inst, adc_ingsoc_init, NULL, &adc_ingsoc_data_##inst,   \
			      &adc_ingsoc_config_##inst, POST_KERNEL,                 \
			      CONFIG_ADC_INIT_PRIORITY, &adc_ingsoc_api);

DT_INST_FOREACH_STATUS_OKAY(ADC_INGSOC_INIT)
