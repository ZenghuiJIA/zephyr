/* SPDX-License-Identifier: Apache-2.0 */

#define DT_DRV_COMPAT ingchips_dma

#include <errno.h>
#include <stdint.h>

#include <platform_api.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <clock_control.h>

LOG_MODULE_REGISTER(dma_ingsoc, CONFIG_DMA_LOG_LEVEL);

#define ING_DMA_CHANNELS 8U

#define DMA_ABORT_OFFSET 0x24U
#define DMA_INT_STATUS_OFFSET 0x30U
#define DMA_CHANNEL_BASE 0x40U
#define DMA_CHANNEL_STRIDE 0x20U
#define DMA_DESC_CTRL_OFFSET 0x00U
#define DMA_DESC_SIZE_OFFSET 0x04U
#define DMA_DESC_SRC_OFFSET 0x08U
#define DMA_DESC_DST_OFFSET 0x10U
#define DMA_DESC_NEXT_OFFSET 0x18U

#define SYSCTRL_CGU_CFG2_OFFSET 0x08U
#define SYSCTRL_CGU_CFG3_OFFSET 0x0cU
#define SYSCTRL_DMA_RX_MUX_OFFSET 0x30U
#define SYSCTRL_DMA_TX_MUX_OFFSET 0x34U

#define DMA_DESC_ENABLE BIT(0)
#define DMA_DESC_HIGH_PRIORITY BIT(29)
#define DMA_DESC_DST_REQ_SHIFT 4U
#define DMA_DESC_SRC_REQ_SHIFT 8U
#define DMA_DESC_DST_ADDR_SHIFT 12U
#define DMA_DESC_SRC_ADDR_SHIFT 14U
#define DMA_DESC_DST_MODE BIT(16)
#define DMA_DESC_SRC_MODE BIT(17)
#define DMA_DESC_DST_WIDTH_SHIFT 18U
#define DMA_DESC_SRC_WIDTH_SHIFT 21U
#define DMA_DESC_SRC_BURST_SHIFT 24U

#define DMA_IRQ_ERROR(channel) BIT(channel)
#define DMA_IRQ_ABORT(channel) BIT(8U + (channel))
#define DMA_IRQ_COMPLETE(channel) BIT(16U + (channel))
#define DMA_IRQ_CHANNEL_MASK(channel) \
	(DMA_IRQ_ERROR(channel) | DMA_IRQ_ABORT(channel) | DMA_IRQ_COMPLETE(channel))
#define DMA_IRQ_ALL GENMASK(23, 0)

struct dma_ingsoc_config {
	mem_addr_t base;
	mem_addr_t sysctrl_base;
};

struct dma_ingsoc_channel {
	dma_callback_t callback;
	void *user_data;
	enum dma_channel_direction direction;
	uint32_t descriptor_ctrl;
	uint32_t source;
	uint32_t destination;
	uint32_t size;
	uint8_t source_width;
	uint8_t destination_width;
	bool configured;
	bool active;
	bool error_callback_enabled;
};

struct dma_ingsoc_data {
	struct dma_context context;
	atomic_t allocated;
	struct k_spinlock lock;
	struct dma_ingsoc_channel channels[ING_DMA_CHANNELS];
};

static inline mem_addr_t dma_channel_reg(const struct dma_ingsoc_config *cfg, uint32_t channel,
					 uint32_t offset)
{
	return cfg->base + DMA_CHANNEL_BASE + channel * DMA_CHANNEL_STRIDE + offset;
}

static int dma_ingsoc_width(uint32_t bytes, uint32_t *encoded)
{
	switch (bytes) {
	case 1U:
		*encoded = 0U;
		return 0;
	case 2U:
		*encoded = 1U;
		return 0;
	case 4U:
		*encoded = 2U;
		return 0;
	case 8U:
		*encoded = 3U;
		return 0;
	default:
		return -EINVAL;
	}
}

static int dma_ingsoc_burst(const struct dma_config *config, uint32_t *encoded)
{
	uint32_t burst_bytes = config->source_burst_length;
	uint32_t transfers;

	if (burst_bytes == 0U) {
		burst_bytes = config->source_data_size;
	}
	if ((burst_bytes % config->source_data_size) != 0U) {
		return -EINVAL;
	}
	transfers = burst_bytes / config->source_data_size;
	if (!is_power_of_two(transfers) || transfers > 128U) {
		return -EINVAL;
	}
	*encoded = find_msb_set(transfers) - 1U;
	return 0;
}

static int dma_ingsoc_validate(const struct dma_config *config,
			       const struct dma_block_config **block)
{
	if (config == NULL || config->head_block == NULL || config->block_count != 1U) {
		return -EINVAL;
	}
	*block = config->head_block;
	if ((*block)->next_block != NULL || (*block)->block_size == 0U) {
		return -ENOTSUP;
	}
	if (config->channel_direction != MEMORY_TO_PERIPHERAL &&
	    config->channel_direction != PERIPHERAL_TO_MEMORY) {
		return -ENOTSUP;
	}
	if (config->cyclic || config->source_chaining_en || config->dest_chaining_en ||
	    config->source_handshake || config->dest_handshake || config->linked_channel != 0U ||
	    config->half_complete_callback_en || (*block)->source_gather_en ||
	    (*block)->dest_scatter_en || (*block)->source_reload_en ||
	    (*block)->dest_reload_en) {
		return -ENOTSUP;
	}
	if ((*block)->source_addr_adj > DMA_ADDR_ADJ_NO_CHANGE ||
	    (*block)->dest_addr_adj > DMA_ADDR_ADJ_NO_CHANGE) {
		return -EINVAL;
	}
	if ((config->channel_direction == MEMORY_TO_PERIPHERAL &&
	     (config->dma_slot < 8U || config->dma_slot >= 16U ||
	      (*block)->dest_addr_adj != DMA_ADDR_ADJ_NO_CHANGE)) ||
	    (config->channel_direction == PERIPHERAL_TO_MEMORY &&
	     (config->dma_slot >= 8U || (*block)->source_addr_adj != DMA_ADDR_ADJ_NO_CHANGE))) {
		return -EINVAL;
	}
	return 0;
}

static int dma_ingsoc_configure(const struct device *dev, uint32_t channel,
				struct dma_config *config)
{
	struct dma_ingsoc_data *data = dev->data;
	struct dma_ingsoc_channel *chan;
	const struct dma_block_config *block;
	k_spinlock_key_t key;
	uint32_t src_width;
	uint32_t dst_width;
	uint32_t burst;
	uint32_t ctrl;
	int err;

	if (channel >= ING_DMA_CHANNELS) {
		return -EINVAL;
	}
	err = dma_ingsoc_validate(config, &block);
	if (err != 0) {
		return err;
	}
	err = dma_ingsoc_width(config->source_data_size, &src_width);
	if (err == 0) {
		err = dma_ingsoc_width(config->dest_data_size, &dst_width);
	}
	if (err == 0) {
		err = dma_ingsoc_burst(config, &burst);
	}
	if (err != 0 || (block->block_size % config->source_data_size) != 0U ||
	    (block->source_address % config->source_data_size) != 0U ||
	    (block->dest_address % config->dest_data_size) != 0U) {
		return -EINVAL;
	}

	ctrl = ((uint32_t)block->dest_addr_adj << DMA_DESC_DST_ADDR_SHIFT) |
	       ((uint32_t)block->source_addr_adj << DMA_DESC_SRC_ADDR_SHIFT) |
	       (dst_width << DMA_DESC_DST_WIDTH_SHIFT) |
	       (src_width << DMA_DESC_SRC_WIDTH_SHIFT) |
	       (burst << DMA_DESC_SRC_BURST_SHIFT);
	if (config->channel_direction == MEMORY_TO_PERIPHERAL) {
		ctrl |= DMA_DESC_DST_MODE | (config->dma_slot << DMA_DESC_DST_REQ_SHIFT);
	} else {
		ctrl |= DMA_DESC_SRC_MODE | (config->dma_slot << DMA_DESC_SRC_REQ_SHIFT);
	}
	if (config->channel_priority != 0U) {
		ctrl |= DMA_DESC_HIGH_PRIORITY;
	}

	key = k_spin_lock(&data->lock);
	chan = &data->channels[channel];
	if (chan->active) {
		k_spin_unlock(&data->lock, key);
		return -EBUSY;
	}
	chan->callback = config->dma_callback;
	chan->user_data = config->user_data;
	chan->direction = config->channel_direction;
	chan->descriptor_ctrl = ctrl;
	chan->source = block->source_address;
	chan->destination = block->dest_address;
	chan->size = block->block_size;
	chan->source_width = config->source_data_size;
	chan->destination_width = config->dest_data_size;
	chan->configured = true;
	chan->error_callback_enabled = !config->error_callback_dis;
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int dma_ingsoc_reload(const struct device *dev, uint32_t channel, uint32_t source,
			     uint32_t destination, size_t size)
{
	struct dma_ingsoc_data *data = dev->data;
	struct dma_ingsoc_channel *chan;
	k_spinlock_key_t key;

	if (channel >= ING_DMA_CHANNELS || size == 0U) {
		return -EINVAL;
	}
	key = k_spin_lock(&data->lock);
	chan = &data->channels[channel];
	if (!chan->configured || chan->active || (size % chan->source_width) != 0U ||
	    (source % chan->source_width) != 0U ||
	    (destination % chan->destination_width) != 0U) {
		k_spin_unlock(&data->lock, key);
		return -EINVAL;
	}
	chan->source = source;
	chan->destination = destination;
	chan->size = size;
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int dma_ingsoc_start(const struct device *dev, uint32_t channel)
{
	const struct dma_ingsoc_config *cfg = dev->config;
	struct dma_ingsoc_data *data = dev->data;
	struct dma_ingsoc_channel *chan;
	k_spinlock_key_t key;
	mem_addr_t descriptor;
	uint32_t count;

	if (channel >= ING_DMA_CHANNELS) {
		return -EINVAL;
	}
	key = k_spin_lock(&data->lock);
	chan = &data->channels[channel];
	if (chan->active) {
		k_spin_unlock(&data->lock, key);
		return 0;
	}
	if (!chan->configured) {
		k_spin_unlock(&data->lock, key);
		return -EINVAL;
	}
	count = chan->size / chan->source_width;
	descriptor = dma_channel_reg(cfg, channel, 0U);
	sys_write32(DMA_IRQ_CHANNEL_MASK(channel), cfg->base + DMA_INT_STATUS_OFFSET);
	sys_write32(chan->descriptor_ctrl, descriptor + DMA_DESC_CTRL_OFFSET);
	sys_write32(count, descriptor + DMA_DESC_SIZE_OFFSET);
	sys_write32(chan->source, descriptor + DMA_DESC_SRC_OFFSET);
	sys_write32(chan->destination, descriptor + DMA_DESC_DST_OFFSET);
	sys_write32(0U, descriptor + DMA_DESC_NEXT_OFFSET);
	chan->active = true;
	sys_write32(chan->descriptor_ctrl | DMA_DESC_ENABLE, descriptor + DMA_DESC_CTRL_OFFSET);
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int dma_ingsoc_stop(const struct device *dev, uint32_t channel)
{
	const struct dma_ingsoc_config *cfg = dev->config;
	struct dma_ingsoc_data *data = dev->data;
	k_spinlock_key_t key;

	if (channel >= ING_DMA_CHANNELS) {
		return -EINVAL;
	}
	key = k_spin_lock(&data->lock);
	if (data->channels[channel].active) {
		sys_write32(BIT(channel), cfg->base + DMA_ABORT_OFFSET);
		data->channels[channel].active = false;
	}
	sys_write32(DMA_IRQ_CHANNEL_MASK(channel), cfg->base + DMA_INT_STATUS_OFFSET);
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int dma_ingsoc_get_status(const struct device *dev, uint32_t channel,
				 struct dma_status *status)
{
	const struct dma_ingsoc_config *cfg = dev->config;
	struct dma_ingsoc_data *data = dev->data;
	struct dma_ingsoc_channel *chan;
	k_spinlock_key_t key;

	if (channel >= ING_DMA_CHANNELS || status == NULL) {
		return -EINVAL;
	}
	key = k_spin_lock(&data->lock);
	chan = &data->channels[channel];
	status->busy = chan->active;
	status->dir = chan->direction;
	status->pending_length = chan->active ?
		sys_read32(dma_channel_reg(cfg, channel, DMA_DESC_SIZE_OFFSET)) *
			chan->source_width : 0U;
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int dma_ingsoc_get_attribute(const struct device *dev, uint32_t type, uint32_t *value)
{
	ARG_UNUSED(dev);

	if (value == NULL) {
		return -EINVAL;
	}
	switch (type) {
	case DMA_ATTR_BUFFER_ADDRESS_ALIGNMENT:
	case DMA_ATTR_BUFFER_SIZE_ALIGNMENT:
	case DMA_ATTR_COPY_ALIGNMENT:
	case DMA_ATTR_MAX_BLOCK_COUNT:
		*value = 1U;
		return 0;
	default:
		return -EINVAL;
	}
}

static bool dma_ingsoc_chan_filter(const struct device *dev, int channel, void *filter_param)
{
	ARG_UNUSED(dev);

	if (filter_param == NULL) {
		return true;
	}
	return (*(const uint32_t *)filter_param & BIT(channel)) != 0U;
}

static void dma_ingsoc_chan_release(const struct device *dev, uint32_t channel)
{
	(void)dma_ingsoc_stop(dev, channel);
}

static uint32_t dma_ingsoc_platform_isr(void *arg)
{
	const struct device *dev = arg;
	const struct dma_ingsoc_config *cfg = dev->config;
	struct dma_ingsoc_data *data = dev->data;
	uint32_t pending = sys_read32(cfg->base + DMA_INT_STATUS_OFFSET) & DMA_IRQ_ALL;

	for (uint32_t channel = 0U; channel < ING_DMA_CHANNELS; channel++) {
		uint32_t channel_status = pending & DMA_IRQ_CHANNEL_MASK(channel);
		dma_callback_t callback;
		void *user_data;
		bool active;
		bool error_callback_enabled;
		int status;
		k_spinlock_key_t key;

		if (channel_status == 0U) {
			continue;
		}
		sys_write32(channel_status, cfg->base + DMA_INT_STATUS_OFFSET);
		key = k_spin_lock(&data->lock);
		active = data->channels[channel].active;
		data->channels[channel].active = false;
		callback = data->channels[channel].callback;
		user_data = data->channels[channel].user_data;
		error_callback_enabled = data->channels[channel].error_callback_enabled;
		k_spin_unlock(&data->lock, key);
		if (!active || callback == NULL) {
			continue;
		}
		status = (channel_status & DMA_IRQ_ERROR(channel)) != 0U ? -EIO :
			 (channel_status & DMA_IRQ_ABORT(channel)) != 0U ? -ECANCELED :
			 DMA_STATUS_COMPLETE;
		if (status == DMA_STATUS_COMPLETE || error_callback_enabled) {
			callback(dev, user_data, channel, status);
		}
	}
	return 0U;
}

static int dma_ingsoc_init(const struct device *dev)
{
	const struct dma_ingsoc_config *cfg = dev->config;
	struct dma_ingsoc_data *data = dev->data;
	void (*volatile set_callback)(platform_irq_callback_type_t, f_platform_irq_cb, void *) =
		platform_set_irq_callback;

	data->context.magic = DMA_MAGIC;
	data->context.dma_channels = ING_DMA_CHANNELS;
	data->context.atomic = &data->allocated;
	atomic_set(&data->allocated, 0);

	/* Match the SDK reset mapping while keeping all accesses register-level. */
	ingchips_clock_enable(INGCHIPS_CLK_DMA);
	sys_write32(0x76543210U, cfg->sysctrl_base + SYSCTRL_DMA_RX_MUX_OFFSET);
	sys_write32(0x76543210U, cfg->sysctrl_base + SYSCTRL_DMA_TX_MUX_OFFSET);
	sys_write32(DMA_IRQ_ALL, cfg->base + DMA_INT_STATUS_OFFSET);
	set_callback(PLATFORM_CB_IRQ_DMA, dma_ingsoc_platform_isr, (void *)dev);
	return 0;
}

static DEVICE_API(dma, dma_ingsoc_api) = {
	.config = dma_ingsoc_configure,
	.reload = dma_ingsoc_reload,
	.start = dma_ingsoc_start,
	.stop = dma_ingsoc_stop,
	.get_status = dma_ingsoc_get_status,
	.get_attribute = dma_ingsoc_get_attribute,
	.chan_filter = dma_ingsoc_chan_filter,
	.chan_release = dma_ingsoc_chan_release,
};

#define DMA_INGSOC_INIT(inst)                                                               \
	static struct dma_ingsoc_data dma_ingsoc_data_##inst;                               \
	static const struct dma_ingsoc_config dma_ingsoc_config_##inst = {                  \
		.base = DT_INST_REG_ADDR_BY_NAME(inst, dma),                                \
		.sysctrl_base = DT_INST_REG_ADDR_BY_NAME(inst, sysctrl),                    \
	};                                                                                       \
	DEVICE_DT_INST_DEFINE(inst, dma_ingsoc_init, NULL, &dma_ingsoc_data_##inst,             \
			      &dma_ingsoc_config_##inst, PRE_KERNEL_1, CONFIG_DMA_INIT_PRIORITY, \
			      &dma_ingsoc_api);

DT_INST_FOREACH_STATUS_OKAY(DMA_INGSOC_INIT)
