/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Access to the WL1251 over TI's WSPI: 32-bit words, most significant bit
 * first. A transfer starts with a command word that carries the direction,
 * the length in bytes and a 17-bit address. Buffers hold the chip's
 * little-endian memory as it is, which the 32-bit word size takes care of.
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "wl1251.h"

LOG_MODULE_DECLARE(wifi_wl1251, CONFIG_WIFI_LOG_LEVEL);

#define WSPI_CMD_READ      BIT(30)
#define WSPI_CMD_LEN_SHIFT 17
#define WSPI_CMD_LEN_MASK  GENMASK(28, 17)
#define WSPI_CMD_ADDR_MASK GENMASK(16, 0)

/*
 * Words the chip clocks out before read data: one of them fixed, as the
 * init command asks for.
 */
#define WSPI_BUSY_WORDS       2
#define WSPI_FIXED_BUSY_WORDS (WSPI_BUSY_WORDS - 1)

/* Init command */
#define WSPI_INIT_TX              0x40
#define WSPI_INIT_FIXED_BUSY_MASK 0x07
#define WSPI_INIT_FIXED_BUSY_EN   0x80
#define WSPI_INIT_IOD             0x40
#define WSPI_INIT_IP              0x20
#define WSPI_INIT_CS              0x10
#define WSPI_INIT_WS              0x08
#define WSPI_INIT_WSPI            0x01
#define WSPI_INIT_END             0x01

#define WL1251_CMD_TIMEOUT_MS 2000

static uint32_t wspi_cmd(bool read, uint32_t addr, size_t len)
{
	return (read ? WSPI_CMD_READ : 0U) | ((len << WSPI_CMD_LEN_SHIFT) & WSPI_CMD_LEN_MASK) |
	       (addr & WSPI_CMD_ADDR_MASK);
}

int wl1251_raw_read(const struct device *dev, uint32_t addr, void *buf, size_t len)
{
	/*
	 * What goes out while the chip answers. With a transmit buffer for
	 * the whole transfer the SPI controller clocks word for word, rather
	 * than free-run in a receive-only mode.
	 */
	static const uint8_t idle[WSPI_BUSY_WORDS * sizeof(uint32_t) + WL1251_RING_CHUNK_SIZE];
	const struct wl1251_config *config = dev->config;
	uint32_t cmd = wspi_cmd(true, addr, len);
	uint32_t busy[WSPI_BUSY_WORDS];
	const struct spi_buf tx_bufs[] = {
		{.buf = &cmd, .len = sizeof(cmd)},
		{.buf = (void *)idle, .len = sizeof(busy) + len},
	};
	const struct spi_buf_set tx = {.buffers = tx_bufs, .count = ARRAY_SIZE(tx_bufs)};
	const struct spi_buf rx_bufs[] = {
		{.buf = NULL, .len = sizeof(cmd)},
		{.buf = busy, .len = sizeof(busy)},
		{.buf = buf, .len = len},
	};
	const struct spi_buf_set rx = {.buffers = rx_bufs, .count = ARRAY_SIZE(rx_bufs)};

	__ASSERT_NO_MSG(len % sizeof(uint32_t) == 0U);
	__ASSERT_NO_MSG(len <= WL1251_RING_CHUNK_SIZE);

	return spi_transceive_dt(&config->bus, &tx, &rx);
}

int wl1251_raw_write(const struct device *dev, uint32_t addr, const void *buf, size_t len)
{
	const struct wl1251_config *config = dev->config;
	uint32_t cmd = wspi_cmd(false, addr, len);
	const struct spi_buf tx_bufs[] = {
		{.buf = &cmd, .len = sizeof(cmd)},
		{.buf = (void *)buf, .len = len},
	};
	const struct spi_buf_set tx = {.buffers = tx_bufs, .count = ARRAY_SIZE(tx_bufs)};

	__ASSERT_NO_MSG(len % sizeof(uint32_t) == 0U);

	return spi_write_dt(&config->bus, &tx);
}

/* Brings the chip's SPI interface out of reset and sets up how it answers */
int wl1251_spi_reset_wake(const struct device *dev)
{
	const struct wl1251_config *config = dev->config;
	uint8_t init[8];
	uint32_t words[2] = {UINT32_MAX, UINT32_MAX};
	const struct spi_buf tx_buf = {.buf = words, .len = sizeof(words)};
	const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
	int ret;

	/* Reset: 64 clocks with the data line high */
	ret = spi_write_dt(&config->bus, &tx);
	if (ret < 0) {
		return ret;
	}

	/* The init command, in the order its octets go out */
	init[0] = 0xff;
	init[1] = 0xff;
	init[2] = WSPI_INIT_TX;
	init[3] = 0;
	init[4] = 0;
	init[5] = WSPI_FIXED_BUSY_WORDS & WSPI_INIT_FIXED_BUSY_MASK;
	init[6] = WSPI_INIT_FIXED_BUSY_EN | WSPI_INIT_IOD | WSPI_INIT_IP | WSPI_INIT_CS |
		  WSPI_INIT_WS | WSPI_INIT_WSPI;
	init[7] = crc7_be(0, &init[2], 5) | WSPI_INIT_END;

	words[0] = sys_get_be32(&init[0]);
	words[1] = sys_get_be32(&init[4]);

	return spi_write_dt(&config->bus, &tx);
}

int wl1251_set_partition(const struct device *dev, uint32_t mem_start, uint32_t mem_size,
			 uint32_t reg_start, uint32_t reg_size)
{
	struct wl1251_data *data = dev->data;
	uint32_t partition[4];

	sys_put_le32(mem_size, (uint8_t *)&partition[0]);
	sys_put_le32(mem_start, (uint8_t *)&partition[1]);
	sys_put_le32(reg_size, (uint8_t *)&partition[2]);
	sys_put_le32(reg_start, (uint8_t *)&partition[3]);

	data->mem_start = mem_start;
	data->mem_size = mem_size;
	data->reg_start = reg_start;

	return wl1251_raw_write(dev, WL1251_SPI_PARTITION, partition, sizeof(partition));
}

static uint32_t mem_addr(const struct device *dev, uint32_t addr)
{
	struct wl1251_data *data = dev->data;

	return addr - data->mem_start;
}

/* The register window follows the memory window */
static uint32_t reg_addr(const struct device *dev, uint32_t addr)
{
	struct wl1251_data *data = dev->data;

	return addr - data->reg_start + data->mem_size;
}

int wl1251_mem_read(const struct device *dev, uint32_t addr, void *buf, size_t len)
{
	return wl1251_raw_read(dev, mem_addr(dev, addr), buf, len);
}

int wl1251_mem_write(const struct device *dev, uint32_t addr, const void *buf, size_t len)
{
	return wl1251_raw_write(dev, mem_addr(dev, addr), buf, len);
}

static uint32_t raw_read32(const struct device *dev, uint32_t addr)
{
	uint32_t val;

	/* All ones is also what a chip that does not answer gives */
	if (wl1251_raw_read(dev, addr, &val, sizeof(val)) < 0) {
		return UINT32_MAX;
	}

	return sys_le32_to_cpu(val);
}

static void raw_write32(const struct device *dev, uint32_t addr, uint32_t val)
{
	val = sys_cpu_to_le32(val);
	(void)wl1251_raw_write(dev, addr, &val, sizeof(val));
}

uint32_t wl1251_mem_read32(const struct device *dev, uint32_t addr)
{
	return raw_read32(dev, mem_addr(dev, addr));
}

void wl1251_mem_write32(const struct device *dev, uint32_t addr, uint32_t val)
{
	raw_write32(dev, mem_addr(dev, addr), val);
}

uint32_t wl1251_reg_read32(const struct device *dev, uint32_t addr)
{
	return raw_read32(dev, reg_addr(dev, addr));
}

void wl1251_reg_write32(const struct device *dev, uint32_t addr, uint32_t val)
{
	raw_write32(dev, reg_addr(dev, addr), val);
}

void *wl1251_cmd_buf(const struct device *dev, size_t len)
{
	struct wl1251_data *data = dev->data;

	__ASSERT_NO_MSG(len <= sizeof(data->cmd.buf));
	memset(data->cmd.buf, 0, ROUND_UP(len, sizeof(uint32_t)));

	return data->cmd.buf;
}

/*
 * Writes a command to the mailbox and waits until the firmware has taken it.
 * The firmware leaves its answer, with the status, in the mailbox.
 */
int wl1251_cmd_send(const struct device *dev, uint16_t id, void *cmd, size_t len)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_cmd_hdr *hdr = cmd;
	k_timepoint_t end = sys_timepoint_calc(K_MSEC(WL1251_CMD_TIMEOUT_MS));
	int ret;

	hdr->id = sys_cpu_to_le16(id);
	hdr->status = 0;

	ret = wl1251_mem_write(dev, data->cmd_mbox, cmd, ROUND_UP(len, sizeof(uint32_t)));
	if (ret < 0) {
		return ret;
	}

	wl1251_reg_write32(dev, WL1251_REG_INTR_TRIG, WL1251_TRIG_CMD);

	while ((wl1251_reg_read32(dev, WL1251_REG_INTR_NO_CLEAR) & WL1251_INTR_CMD_COMPLETE) ==
	       0U) {
		if (sys_timepoint_expired(end)) {
			LOG_ERR("Command %u timed out", id);
			return -ETIMEDOUT;
		}
		k_msleep(1);
	}

	wl1251_reg_write32(dev, WL1251_REG_INTR_ACK, WL1251_INTR_CMD_COMPLETE);

	return 0;
}

int wl1251_cmd_status(const struct device *dev, uint16_t id)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_cmd_hdr hdr;
	int ret;

	ret = wl1251_mem_read(dev, data->cmd_mbox, &hdr, sizeof(hdr));
	if (ret < 0) {
		return ret;
	}

	if (sys_le16_to_cpu(hdr.status) != WL1251_CMD_STATUS_SUCCESS) {
		LOG_ERR("Command %u failed with status %u", id, sys_le16_to_cpu(hdr.status));
		return -EIO;
	}

	return 0;
}

int wl1251_cmd_interrogate(const struct device *dev, uint16_t id, void *ie, size_t len)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_ie_hdr *hdr = ie;
	int ret;

	hdr->id = sys_cpu_to_le16(id);
	hdr->len = sys_cpu_to_le16(len - sizeof(*hdr));

	ret = wl1251_cmd_send(dev, WL1251_CMD_INTERROGATE, ie, sizeof(*hdr));
	if (ret < 0) {
		return ret;
	}

	ret = wl1251_mem_read(dev, data->cmd_mbox, ie, ROUND_UP(len, sizeof(uint32_t)));
	if (ret < 0) {
		return ret;
	}

	if (sys_le16_to_cpu(hdr->cmd.status) != WL1251_CMD_STATUS_SUCCESS) {
		LOG_ERR("Reading IE 0x%04x failed with status %u", id,
			sys_le16_to_cpu(hdr->cmd.status));
		return -EIO;
	}

	return 0;
}

int wl1251_cmd_configure(const struct device *dev, uint16_t id, void *ie, size_t len)
{
	struct wl1251_ie_hdr *hdr = ie;
	int ret;

	hdr->id = sys_cpu_to_le16(id);
	hdr->len = sys_cpu_to_le16(len - sizeof(*hdr));

	ret = wl1251_cmd_send(dev, WL1251_CMD_CONFIGURE, ie, len);
	if (ret < 0) {
		return ret;
	}

	ret = wl1251_cmd_status(dev, WL1251_CMD_CONFIGURE);
	if (ret < 0) {
		LOG_ERR("Writing IE 0x%04x failed", id);
	}

	return ret;
}

/* A frame for the firmware to send on its own; NULL only reserves the space */
int wl1251_cmd_template(const struct device *dev, uint16_t id, const void *frame, size_t len)
{
	struct wl1251_cmd_template *cmd;
	int ret;

	if (len > WL1251_MAX_TEMPLATE_SIZE) {
		return -EMSGSIZE;
	}

	cmd = wl1251_cmd_buf(dev, sizeof(*cmd));
	cmd->size = sys_cpu_to_le16(len);
	if (frame != NULL) {
		memcpy(cmd->data, frame, len);
	}

	ret = wl1251_cmd_send(dev, id, cmd, offsetof(struct wl1251_cmd_template, data) + len);
	if (ret < 0) {
		return ret;
	}

	return wl1251_cmd_status(dev, id);
}

int wl1251_cfg_u8(const struct device *dev, uint16_t id, uint8_t value)
{
	struct wl1251_ie_u8 *ie = wl1251_cmd_buf(dev, sizeof(*ie));

	ie->value = value;

	return wl1251_cmd_configure(dev, id, ie, sizeof(*ie));
}

int wl1251_cfg_u16(const struct device *dev, uint16_t id, uint16_t value)
{
	struct wl1251_ie_u16 *ie = wl1251_cmd_buf(dev, sizeof(*ie));

	ie->value = sys_cpu_to_le16(value);

	return wl1251_cmd_configure(dev, id, ie, sizeof(*ie));
}

int wl1251_cfg_u32(const struct device *dev, uint16_t id, uint32_t value)
{
	struct wl1251_ie_u32 *ie = wl1251_cmd_buf(dev, sizeof(*ie));

	ie->value = sys_cpu_to_le32(value);

	return wl1251_cmd_configure(dev, id, ie, sizeof(*ie));
}
