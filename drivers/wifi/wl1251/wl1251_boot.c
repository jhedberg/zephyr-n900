/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Power-up of the WL1251: the chip has no firmware of its own, so the host
 * loads the calibration data (NVS) and the firmware into its memory, starts
 * its CPU and then sets up the running firmware.
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "wl1251.h"

LOG_MODULE_DECLARE(wifi_wl1251, CONFIG_WIFI_LOG_LEVEL);

/* Before the image: a version and the length of the image, big-endian */
#define FW_HDR_SIZE   8
#define FW_CHUNK_SIZE 512

/*
 * Neither file is part of this repository, so both come from Kconfig paths.
 * A build without them is good for nothing but a build test.
 */
#ifdef WL1251_FIRMWARE
static const uint8_t wl1251_fw[] = {
#include <wl1251_fw.inc>
};

static const uint8_t wl1251_nvs[] = {
#include <wl1251_nvs.inc>
};
#else
static const uint8_t wl1251_fw[FW_HDR_SIZE];
static const uint8_t wl1251_nvs[1];
#endif

/*
 * The NVS file starts with register bursts of a length (in words), a 16-bit
 * address and the data. The one for the MAC address is at a fixed place,
 * with the address stored last octet first.
 */
#define NVS_MAC_BURST     0x19
#define NVS_MAC_BURST_LEN 2
#define NVS_MAC_ADDR      0x546c
#define NVS_MAC_DATA      0x1c
/* Between the end of the bursts and the tables */
#define NVS_TABLES_GAP    7

#define POWER_ON_DELAY_MS     10
#define SOFT_RESET_TIMEOUT_MS 1000
#define INIT_TIMEOUT_MS       1000

enum {
	PLL_INT_DIV,
	PLL_FRAC_DIV,
	PLL_ATTN,
	PLL_ALPHA,
	PLL_STOP_TIME,
	PLL_LOOP_FILTER,
	PLL_NUM_PARAMS,
};

/* Baseband PLL settings for each reference clock the NVS can name */
static const uint32_t pll_params[][PLL_NUM_PARAMS] = {
	{83, 87381, 0xb, 5, 0xf00, 3},   /* 19.2 MHz */
	{61, 141154, 0xb, 5, 0x1450, 2}, /* 26 MHz */
	{41, 174763, 0xc, 6, 0x2d00, 1}, /* 38.4 MHz */
	{40, 0, 0xc, 6, 0x2ee0, 1},      /* 40 MHz */
	{47, 162280, 0xc, 6, 0x2760, 1}, /* 33.6 MHz */
};

void wl1251_power_off(const struct device *dev)
{
	const struct wl1251_config *config = dev->config;

	(void)gpio_pin_set_dt(&config->power, 0);
}

static int chip_wakeup(const struct device *dev)
{
	const struct wl1251_config *config = dev->config;
	struct wl1251_data *data = dev->data;
	uint32_t elp;
	int ret;

	ret = gpio_pin_set_dt(&config->power, 1);
	if (ret < 0) {
		return ret;
	}
	k_msleep(POWER_ON_DELAY_MS);

	ret = wl1251_spi_reset_wake(dev);
	if (ret < 0) {
		return ret;
	}

	/* Only registers are needed until the firmware is loaded */
	ret = wl1251_set_partition(dev, 0, 0, WL1251_REG_BASE, WL1251_REG_SIZE_BOOT);
	if (ret < 0) {
		return ret;
	}

	elp = sys_cpu_to_le32(WL1251_ELP_WAKE_UP);
	ret = wl1251_raw_write(dev, WL1251_SPI_ELP_CTRL, &elp, sizeof(elp));
	if (ret < 0) {
		return ret;
	}
	ret = wl1251_raw_read(dev, WL1251_SPI_ELP_CTRL, &elp, sizeof(elp));
	if (ret < 0) {
		return ret;
	}
	if ((sys_le32_to_cpu(elp) & WL1251_ELP_WLAN_READY) == 0U) {
		LOG_WRN("Chip not ready after wake-up (0x%08x)", sys_le32_to_cpu(elp));
	}

	data->chip_id = wl1251_reg_read32(dev, WL1251_REG_CHIP_ID);
	if (data->chip_id != WL1251_CHIP_ID_PG11 && data->chip_id != WL1251_CHIP_ID_PG12) {
		LOG_ERR("Unsupported chip ID 0x%08x", data->chip_id);
		return -ENODEV;
	}

	return 0;
}

static int soft_reset(const struct device *dev)
{
	k_timepoint_t end = sys_timepoint_calc(K_MSEC(SOFT_RESET_TIMEOUT_MS));

	wl1251_reg_write32(dev, WL1251_REG_SOFT_RESET, WL1251_SOFT_RESET);

	/* The bit clears itself */
	while ((wl1251_reg_read32(dev, WL1251_REG_SOFT_RESET) & WL1251_SOFT_RESET) != 0U) {
		if (sys_timepoint_expired(end)) {
			LOG_ERR("Soft reset timed out");
			return -ETIMEDOUT;
		}
		k_msleep(1);
	}

	/* No receive or transmit, and no calibration at start */
	wl1251_reg_write32(dev, WL1251_REG_ENABLE, 0);
	wl1251_reg_write32(dev, WL1251_REG_SPARE_A2, 0xffff);

	return 0;
}

static int upload_nvs(const struct device *dev)
{
	struct wl1251_data *data = dev->data;
	const uint8_t *nvs = wl1251_nvs;
	const uint8_t *end = nvs + sizeof(wl1251_nvs);
	const uint8_t *ptr = nvs;
	uint32_t addr;
	int ret;

	/* Register bursts, up to a zero length */
	while (ptr < end && ptr[0] != 0U) {
		uint8_t words = ptr[0];
		bool is_mac;

		if (end - ptr < 3 + words * 4) {
			return -EINVAL;
		}

		addr = sys_get_le16(&ptr[1]) & ~BIT(0);
		is_mac = (ptr - nvs == NVS_MAC_BURST && words == NVS_MAC_BURST_LEN &&
			  addr == NVS_MAC_ADDR);
		ptr += 3;

		for (uint8_t i = 0; i < words; i++, ptr += 4, addr += 4) {
			uint32_t val = sys_get_le32(ptr);

			if (is_mac) {
				/* The file holds a placeholder address */
				uint8_t mac[8] = {0};

				sys_memcpy_swap(mac, data->mac, sizeof(data->mac));
				val = sys_get_le32(&mac[i * 4]);
			}

			/* With no memory window these land in the registers */
			wl1251_mem_write32(dev, addr, val);
		}
	}

	ptr += NVS_TABLES_GAP;
	if (ptr >= end) {
		return -EINVAL;
	}

	/* The tables go right behind where the firmware will be */
	addr = sizeof(wl1251_fw);
	ret = wl1251_set_partition(dev, addr, WL1251_MEM_SIZE_BOOT, WL1251_REG_BASE,
				   WL1251_REG_SIZE_BOOT);
	if (ret < 0) {
		return ret;
	}

	for (; ptr < end; ptr += 4, addr += 4) {
		uint8_t word[4] = {0};

		memcpy(word, ptr, MIN(end - ptr, 4));
		wl1251_mem_write32(dev, addr, sys_get_le32(word));
	}

	wl1251_reg_write32(dev, WL1251_REG_NVS_ADDR, sizeof(wl1251_fw));

	return 0;
}

/* Clock and PLL setup, from what the NVS said about the reference clock */
static int init_clocks(const struct device *dev)
{
	uint32_t scr_pad6 = wl1251_reg_read32(dev, WL1251_REG_SCR_PAD6);
	uint32_t elp_cmd = wl1251_reg_read32(dev, WL1251_REG_ELP_CMD);
	uint32_t ref_clock = scr_pad6 & 0xff;
	const uint32_t *pll;
	uint32_t val;

	if (ref_clock >= ARRAY_SIZE(pll_params)) {
		LOG_ERR("Unknown reference clock %u", ref_clock);
		return -EINVAL;
	}
	pll = pll_params[ref_clock];

	LOG_DBG("Reference clock %u", ref_clock);

	/* Baseband calibration time of 300 us, clock buffer time of 210 us */
	wl1251_reg_write32(dev, WL1251_REG_PLL_CAL_TIME, 0x9);
	wl1251_reg_write32(dev, WL1251_REG_CLK_BUF_TIME, 0x6);

	/* Clock detection on wake-up, and the clock source type */
	wl1251_reg_write32(dev, WL1251_REG_ELP_CFG_MODE, ((scr_pad6 & 0xff00) << 4) | BIT(14));

	/* The baseband PLL fix of chip revision 1.2 */
	elp_cmd |= BIT(6);
	wl1251_reg_write32(dev, WL1251_REG_ELP_CMD, elp_cmd);

	/* PLL stable time of 1000 us */
	wl1251_reg_write32(dev, WL1251_REG_PLL_SYNC_CNT, 0x20);

	/* Clock request time: 1 ms less than the clock's settling time */
	val = wl1251_reg_read32(dev, WL1251_REG_CLK_REQ_TIME);
	wl1251_reg_write32(dev, WL1251_REG_CLK_REQ_TIME, val > 0x21 ? val - 0x21 : 0);

	wl1251_reg_write32(dev, WL1251_REG_RF_AFE_PLL, 0x4b5);
	wl1251_reg_write32(dev, WL1251_REG_RF_AFE_5, 0x50);
	wl1251_reg_write32(dev, WL1251_REG_RF_AFE_CTRL_2, 0x11c001);
	/* Dividers of both PLLs for the VCO clock, and the VCO bias current */
	wl1251_reg_write32(dev, WL1251_REG_RF_AFE_13, 0x1e);

	wl1251_reg_write32(dev, WL1251_REG_BB_PLL_DIV, pll[PLL_INT_DIV] | 0x17000);
	wl1251_reg_write32(dev, WL1251_REG_BB_PLL_FRAC, pll[PLL_FRAC_DIV]);
	wl1251_reg_write32(dev, WL1251_REG_BB_PLL_SIGMA, 0x3039);
	wl1251_reg_write32(dev, WL1251_REG_BB_PLL_LOOP,
			   (pll[PLL_ATTN] << 16) | (pll[PLL_ALPHA] << 12) | 0x1);
	wl1251_reg_write32(dev, WL1251_REG_BB_PLL_STOP, pll[PLL_STOP_TIME] | 0xa0000);
	wl1251_reg_write32(dev, WL1251_REG_RF_AFE_FILTER, pll[PLL_LOOP_FILTER] | 0x30);
	wl1251_reg_write32(dev, WL1251_REG_RF_AFE_REFDIV, 0x29);

	/* Restart the wake-up sequence with the new settings */
	wl1251_reg_write32(dev, WL1251_REG_ELP_CMD, elp_cmd | BIT(0));
	k_msleep(2);

	return 0;
}

static int upload_firmware(const struct device *dev)
{
	struct wl1251_data *data = dev->data;
	const uint8_t *image = wl1251_fw + FW_HDR_SIZE;
	uint32_t len = sys_get_be32(&wl1251_fw[4]);
	uint32_t window = WL1251_MEM_START_BOOT;
	int ret;

	if (len % 4 != 0U || len > sizeof(wl1251_fw) - FW_HDR_SIZE) {
		LOG_ERR("Bad firmware image");
		return -EINVAL;
	}

	ret = wl1251_set_partition(dev, window, WL1251_MEM_SIZE_BOOT, WL1251_REG_BASE,
				   WL1251_REG_SIZE_BOOT);
	if (ret < 0) {
		return ret;
	}

	for (uint32_t offset = 0; offset < len; offset += FW_CHUNK_SIZE) {
		uint32_t chunk = MIN(len - offset, FW_CHUNK_SIZE);

		/* Move the memory window along with the image, a chunk early */
		if (offset + 2 * FW_CHUNK_SIZE > window + WL1251_MEM_SIZE_BOOT) {
			window = offset;
			ret = wl1251_set_partition(dev, window, WL1251_MEM_SIZE_BOOT,
						   WL1251_REG_BASE, WL1251_REG_SIZE_BOOT);
			if (ret < 0) {
				return ret;
			}
		}

		/* The image is const: the SPI driver gets a copy */
		memcpy(data->frame, image + offset, chunk);
		ret = wl1251_mem_write(dev, offset, data->frame, chunk);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

static int run_firmware(const struct device *dev)
{
	struct wl1251_data *data = dev->data;
	k_timepoint_t end = sys_timepoint_calc(K_MSEC(INIT_TIMEOUT_MS));
	struct wl1251_ie_fw_rev *rev;
	uint32_t val;
	int ret;

	val = wl1251_reg_read32(dev, WL1251_REG_ECPU_CTRL);
	wl1251_reg_write32(dev, WL1251_REG_ECPU_CTRL, val & ~WL1251_ECPU_HALT);

	if (wl1251_reg_read32(dev, WL1251_REG_CHIP_ID) != data->chip_id) {
		LOG_ERR("Chip ID changed after the firmware started");
		return -EIO;
	}

	while (true) {
		val = wl1251_reg_read32(dev, WL1251_REG_INTR_NO_CLEAR);
		if (val == UINT32_MAX) {
			LOG_ERR("No answer from the firmware");
			return -EIO;
		}
		if ((val & WL1251_INTR_INIT_COMPLETE) != 0U) {
			break;
		}
		if (sys_timepoint_expired(end)) {
			LOG_ERR("Firmware did not start");
			return -ETIMEDOUT;
		}
		k_usleep(100);
	}
	wl1251_reg_write32(dev, WL1251_REG_INTR_ACK, WL1251_INTR_INIT_COMPLETE);

	data->cmd_mbox = wl1251_reg_read32(dev, WL1251_REG_CMD_MBOX_PTR);
	data->event_mbox[0] = wl1251_reg_read32(dev, WL1251_REG_EVENT_MBOX_PTR);
	data->event_mbox[1] = data->event_mbox[0] + sizeof(struct wl1251_event_mbox);

	ret = wl1251_set_partition(dev, WL1251_MEM_START_RUN, WL1251_MEM_SIZE_RUN, WL1251_REG_BASE,
				   WL1251_REG_SIZE_RUN);
	if (ret < 0) {
		return ret;
	}

	rev = wl1251_cmd_buf(dev, sizeof(*rev));
	ret = wl1251_cmd_interrogate(dev, WL1251_IE_FW_REV, rev, sizeof(*rev));
	if (ret < 0) {
		return ret;
	}
	memcpy(data->fw_version, rev->fw_version, sizeof(rev->fw_version));
	data->fw_version[sizeof(rev->fw_version)] = '\0';

	return 0;
}

static int cfg_templates(const struct device *dev)
{
	/*
	 * Sizes of the largest probe request, null data, PS-Poll, QoS null
	 * data, probe response and beacon the firmware is to keep room for.
	 */
	static const struct {
		uint16_t id;
		uint16_t size;
	} templates[] = {
		{WL1251_CMD_PROBE_REQ, 24 + 34 + 2 * 10},
		{WL1251_CMD_NULL_DATA, 24},
		{WL1251_CMD_PS_POLL, 16},
		{WL1251_CMD_QOS_NULL_DATA, 26},
		{WL1251_CMD_PROBE_RESP, 24 + 12 + 34 + 2 * 10 + 3 + 29},
		{WL1251_CMD_BEACON, 24 + 12 + 34 + 2 * 10 + 3 + 29},
	};
	struct wl1251_cmd_vbm *vbm;
	int ret;

	ARRAY_FOR_EACH(templates, i) {
		ret = wl1251_cmd_template(dev, templates[i].id, NULL, templates[i].size);
		if (ret < 0) {
			return ret;
		}
	}

	/* The TIM: first the room for a full one, then an empty one */
	for (uint16_t bitmap_len = sizeof(vbm->bitmap); bitmap_len > 0U; bitmap_len = 1) {
		vbm = wl1251_cmd_buf(dev, sizeof(*vbm));
		vbm->id = 5;
		vbm->length = bitmap_len + 3;
		vbm->len = sys_cpu_to_le16(bitmap_len + 5);
		ret = wl1251_cmd_send(dev, WL1251_CMD_VBM, vbm, sizeof(*vbm));
		if (ret < 0) {
			return ret;
		}
		if (bitmap_len == 1U) {
			break;
		}
	}

	return 0;
}

static int cfg_memory(const struct device *dev)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_ie_mem_cfg *cfg;
	struct wl1251_ie_mem_map *map;
	struct wl1251_ie_data_path *path;
	struct wl1251_ie_data_path_resp *resp;
	int ret;

	cfg = wl1251_cmd_buf(dev, sizeof(*cfg));
	cfg->num_stations = sys_cpu_to_le16(1);
	cfg->rx_mem_block_num = 35;
	cfg->tx_min_mem_block_num = 64;
	cfg->num_tx_queues = 4;
	/* Frames go through the packet rings */
	cfg->host_if_options = 3;
	cfg->num_ssid_profiles = 1;
	cfg->debug_buffer_size = sys_cpu_to_le16(256);
	cfg->rx_queue.num_descs = 32;
	cfg->rx_queue.type = 0x07;
	for (uint8_t i = 0; i < cfg->num_tx_queues; i++) {
		cfg->tx_queue[i].num_descs = 16;
		cfg->tx_queue[i].attributes = i;
	}
	ret = wl1251_cmd_configure(dev, WL1251_IE_MEM_CFG, cfg, sizeof(*cfg));
	if (ret < 0) {
		return ret;
	}

	map = wl1251_cmd_buf(dev, sizeof(*map));
	ret = wl1251_cmd_interrogate(dev, WL1251_IE_MEM_MAP, map, sizeof(*map));
	if (ret < 0) {
		return ret;
	}
	data->tx_blocks = sys_le32_to_cpu(map->num_tx_mem_blocks);

	path = wl1251_cmd_buf(dev, sizeof(*path));
	path->rx_chunk_size = sys_cpu_to_le16(WL1251_RING_CHUNK_SIZE);
	path->tx_chunk_size = sys_cpu_to_le16(WL1251_RING_CHUNK_SIZE);
	path->rx_chunk_num = 2;
	path->tx_chunk_num = WL1251_TX_RING_CHUNKS;
	path->tx_complete_threshold = 1;
	path->tx_complete_ring_depth = WL1251_TX_RESULTS;
	/* In ms */
	path->tx_complete_timeout = sys_cpu_to_le32(20);
	ret = wl1251_cmd_configure(dev, WL1251_IE_DATA_PATH_PARAMS, path, sizeof(*path));
	if (ret < 0) {
		return ret;
	}

	resp = wl1251_cmd_buf(dev, sizeof(*resp));
	ret = wl1251_cmd_interrogate(dev, WL1251_IE_DATA_PATH_PARAMS, resp, sizeof(*resp));
	if (ret < 0) {
		return ret;
	}
	data->path = *resp;

	LOG_DBG("%u TX blocks, RX ring 0x%x, TX ring 0x%x", data->tx_blocks,
		sys_le32_to_cpu(data->path.rx_ring_addr), sys_le32_to_cpu(data->path.tx_ring_addr));

	return 0;
}

static int cfg_tx_queues(const struct device *dev)
{
	struct wl1251_data *data = dev->data;
	/*
	 * Best effort, background, video and voice, with the default EDCA
	 * parameters of each; the transmit opportunity is in us.
	 */
	static const struct {
		uint8_t high_percent;
		uint8_t low_percent;
		uint8_t cw_min;
		uint16_t cw_max;
		uint8_t aifsn;
		uint16_t txop;
	} acs[] = {
		{35, 25, 15, 1023, 3, 0},
		{25, 15, 15, 1023, 7, 0},
		{35, 25, 7, 15, 2, 3008},
		{35, 25, 3, 7, 2, 1504},
	};
	int ret;

	ARRAY_FOR_EACH(acs, i) {
		struct wl1251_ie_tx_queue_cfg *queue = wl1251_cmd_buf(dev, sizeof(*queue));

		queue->qid = i;
		queue->high_threshold =
			sys_cpu_to_le16(acs[i].high_percent * data->tx_blocks / 100U);
		queue->low_threshold = sys_cpu_to_le16(acs[i].low_percent * data->tx_blocks / 100U);
		ret = wl1251_cmd_configure(dev, WL1251_IE_TX_QUEUE_CFG, queue, sizeof(*queue));
		if (ret < 0) {
			return ret;
		}
	}

	ARRAY_FOR_EACH(acs, i) {
		struct wl1251_ie_ac_cfg *ac = wl1251_cmd_buf(dev, sizeof(*ac));

		ac->ac = i;
		ac->cw_min = acs[i].cw_min;
		ac->cw_max = sys_cpu_to_le16(acs[i].cw_max);
		ac->aifsn = acs[i].aifsn;
		ac->txop_limit = sys_cpu_to_le16(acs[i].txop);
		ret = wl1251_cmd_configure(dev, WL1251_IE_AC_CFG, ac, sizeof(*ac));
		if (ret < 0) {
			return ret;
		}
	}

	/*
	 * How each queue gets to the medium. The firmware sends nothing
	 * from a queue that has not been told.
	 */
	ARRAY_FOR_EACH(acs, i) {
		struct wl1251_ie_tid_cfg *tid = wl1251_cmd_buf(dev, sizeof(*tid));

		tid->queue = i;
		/* EDCA, with legacy power save and acknowledgements */
		tid->channel_type = 1;
		tid->tsid = i;
		ret = wl1251_cmd_configure(dev, WL1251_IE_TID_CFG, tid, sizeof(*tid));
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

static int cfg_radio(const struct device *dev)
{
	struct wl1251_ie_slot *slot;
	struct wl1251_ie_group_addr_tbl *groups;
	struct wl1251_ie_rx_timeout *rx_timeout;
	struct wl1251_ie_conn_monit *monit;
	struct wl1251_ie_beacon_filter_opt *filter_opt;
	struct wl1251_ie_beacon_filter_tbl *filter_tbl;
	struct wl1251_ie_cca_threshold *cca;
	struct wl1251_ie_bcn_dtim *bcn;
	int ret;

	/* Packet detection threshold: the firmware's default */
	ret = wl1251_cfg_u32(dev, WL1251_IE_PD_THRESHOLD, 0);
	if (ret < 0) {
		return ret;
	}

	/* Short slot time */
	slot = wl1251_cmd_buf(dev, sizeof(*slot));
	slot->slot_time = 1;
	ret = wl1251_cmd_configure(dev, WL1251_IE_SLOT, slot, sizeof(*slot));
	if (ret < 0) {
		return ret;
	}

	/* No multicast address filter: the network stack sorts them out */
	groups = wl1251_cmd_buf(dev, sizeof(*groups));
	ret = wl1251_cmd_configure(dev, WL1251_IE_GROUP_ADDRESS_TBL, groups, sizeof(*groups));
	if (ret < 0) {
		return ret;
	}

	rx_timeout = wl1251_cmd_buf(dev, sizeof(*rx_timeout));
	rx_timeout->ps_poll_timeout = sys_cpu_to_le16(15);
	rx_timeout->upsd_timeout = sys_cpu_to_le16(15);
	ret = wl1251_cmd_configure(dev, WL1251_IE_SERVICE_PERIOD_TMO, rx_timeout,
				   sizeof(*rx_timeout));
	if (ret < 0) {
		return ret;
	}

	/* No RTS/CTS */
	ret = wl1251_cfg_u16(dev, WL1251_IE_RTS_THRESHOLD, 2347);
	if (ret < 0) {
		return ret;
	}

	/* The BSS counts as lost after 10 missed beacons or 500 ms without one */
	monit = wl1251_cmd_buf(dev, sizeof(*monit));
	monit->synch_fail_threshold = sys_cpu_to_le32(10);
	monit->bss_lose_timeout = sys_cpu_to_le32(500);
	ret = wl1251_cmd_configure(dev, WL1251_IE_CONN_MONIT_PARAMS, monit, sizeof(*monit));
	if (ret < 0) {
		return ret;
	}

	/* All beacons go to the host */
	filter_opt = wl1251_cmd_buf(dev, sizeof(*filter_opt));
	ret = wl1251_cmd_configure(dev, WL1251_IE_BEACON_FILTER_OPT, filter_opt,
				   sizeof(*filter_opt));
	if (ret < 0) {
		return ret;
	}

	/* Once filtered, a channel switch announcement still passes */
	filter_tbl = wl1251_cmd_buf(dev, sizeof(*filter_tbl));
	filter_tbl->num_ie = 1;
	filter_tbl->table[0] = 37;
	filter_tbl->table[1] = BIT(1);
	ret = wl1251_cmd_configure(dev, WL1251_IE_BEACON_FILTER_TBL, filter_tbl,
				   sizeof(*filter_tbl));
	if (ret < 0) {
		return ret;
	}

	/* No energy detection */
	cca = wl1251_cmd_buf(dev, sizeof(*cca));
	cca->rx_cca_threshold = sys_cpu_to_le16(0xffef);
	ret = wl1251_cmd_configure(dev, WL1251_IE_CCA_THRESHOLD, cca, sizeof(*cca));
	if (ret < 0) {
		return ret;
	}

	bcn = wl1251_cmd_buf(dev, sizeof(*bcn));
	bcn->beacon_rx_timeout = sys_cpu_to_le16(10000);
	bcn->broadcast_timeout = sys_cpu_to_le16(20000);
	bcn->rx_broadcast_in_ps = 1;
	bcn->ps_poll_threshold = 4;

	return wl1251_cmd_configure(dev, WL1251_IE_BCN_DTIM_OPTIONS, bcn, sizeof(*bcn));
}

/*
 * Coexistence with the Bluetooth controller that shares the antenna: the two
 * chips are wired to each other, and the firmware arbitrates.
 */
static int cfg_bt_coex(const struct device *dev)
{
	struct wl1251_ie_sg_cfg *sg;
	int ret;

	/* 0 enables it */
	ret = wl1251_cfg_u8(dev, WL1251_IE_SG_ENABLE, 0);
	if (ret < 0) {
		return ret;
	}

	/* TI's defaults; times in us */
	sg = wl1251_cmd_buf(dev, sizeof(*sg));
	sg->min_rate = sys_cpu_to_le32(9);
	sg->bt_hp_max_time = sys_cpu_to_le16(2000);
	sg->wlan_hp_max_time = sys_cpu_to_le16(5000);
	sg->sense_disable_timer = sys_cpu_to_le16(1350);
	sg->rx_time_bt_hp = sys_cpu_to_le16(1500);
	sg->tx_time_bt_hp = sys_cpu_to_le16(1500);
	sg->rx_time_bt_hp_fast = sys_cpu_to_le16(1500);
	sg->tx_time_bt_hp_fast = sys_cpu_to_le16(3000);
	sg->wlan_cycle_fast = sys_cpu_to_le16(8700);
	sg->bt_anti_starvation_period = sys_cpu_to_le16(500);
	sg->next_bt_lp_packet = sys_cpu_to_le16(3000);
	sg->wake_up_beacon = sys_cpu_to_le16(6300);
	sg->hp_dm_max_guard_time = sys_cpu_to_le16(1600);
	sg->next_wlan_packet = sys_cpu_to_le16(2550);
	sg->signal_type = 1;
	sg->max_cts = 3;
	sg->wlan_packets_num = 2;
	sg->bt_packets_num = 2;
	sg->missed_rx_avalanche = 5;
	sg->bt_anti_starvation_cycles = 4;
	sg->ack_mode_dual_ant = 1;
	sg->pa_sd_enable = 1;
	sg->bt_hp_respected_num = 3;

	return wl1251_cmd_configure(dev, WL1251_IE_SG_CFG, sg, sizeof(*sg));
}

/* Brings the firmware to where it can scan, join and pass frames */
static int configure(const struct device *dev)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_ie_feature_cfg *feature;
	struct wl1251_ie_rx_cfg *rx_cfg;
	struct wl1251_ie_station_id *station;
	struct wl1251_cmd_path *path;
	int ret;

	/* Encryption and decryption in the chip */
	feature = wl1251_cmd_buf(dev, sizeof(*feature));
	ret = wl1251_cmd_configure(dev, WL1251_IE_FEATURE_CFG, feature, sizeof(*feature));
	if (ret < 0) {
		return ret;
	}

	ret = wl1251_cfg_u8(dev, WL1251_IE_DEFAULT_KEY, 0);
	if (ret < 0) {
		return ret;
	}

	ret = cfg_templates(dev);
	if (ret < 0) {
		return ret;
	}

	ret = cfg_memory(dev);
	if (ret < 0) {
		return ret;
	}

	/* In TU */
	ret = wl1251_cfg_u32(dev, WL1251_IE_RX_MSDU_LIFE_TIME, 512000);
	if (ret < 0) {
		return ret;
	}

	rx_cfg = wl1251_cmd_buf(dev, sizeof(*rx_cfg));
	rx_cfg->config = sys_cpu_to_le32(WL1251_RX_CFG_ALL_GOOD | WL1251_RX_CFG_TSF);
	rx_cfg->filter = sys_cpu_to_le32(data->rx_filter);
	ret = wl1251_cmd_configure(dev, WL1251_IE_RX_CFG, rx_cfg, sizeof(*rx_cfg));
	if (ret < 0) {
		return ret;
	}

	ret = cfg_tx_queues(dev);
	if (ret < 0) {
		return ret;
	}

	ret = cfg_radio(dev);
	if (ret < 0) {
		return ret;
	}

	ret = cfg_bt_coex(dev);
	if (ret < 0) {
		return ret;
	}

	path = wl1251_cmd_buf(dev, sizeof(*path));
	path->channel = data->bss_params.channel;
	ret = wl1251_cmd_send(dev, WL1251_CMD_ENABLE_RX, path, sizeof(*path));
	if (ret == 0) {
		ret = wl1251_cmd_status(dev, WL1251_CMD_ENABLE_RX);
	}
	if (ret < 0) {
		return ret;
	}

	path = wl1251_cmd_buf(dev, sizeof(*path));
	path->channel = data->bss_params.channel;
	ret = wl1251_cmd_send(dev, WL1251_CMD_ENABLE_TX, path, sizeof(*path));
	if (ret == 0) {
		ret = wl1251_cmd_status(dev, WL1251_CMD_ENABLE_TX);
	}
	if (ret < 0) {
		return ret;
	}

	/* Always awake */
	ret = wl1251_cfg_u8(dev, WL1251_IE_SLEEP_AUTH, 0);
	if (ret < 0) {
		return ret;
	}

	station = wl1251_cmd_buf(dev, sizeof(*station));
	sys_memcpy_swap(station->mac, data->mac, sizeof(data->mac));

	return wl1251_cmd_configure(dev, WL1251_IE_STATION_ID, station, sizeof(*station));
}

int wl1251_boot(const struct device *dev)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_ie_event_mask *mask;
	uint32_t val;
	int ret;

	if (!IS_ENABLED(WL1251_FIRMWARE)) {
		LOG_ERR("Built without the firmware and NVS files");
		return -ENOENT;
	}

	ret = chip_wakeup(dev);
	if (ret < 0) {
		return ret;
	}

	/* The chip's CPU stays halted while its memory is filled */
	wl1251_reg_write32(dev, WL1251_REG_ECPU_CTRL, WL1251_ECPU_HALT);

	ret = soft_reset(dev);
	if (ret < 0) {
		return ret;
	}

	ret = upload_nvs(dev);
	if (ret < 0) {
		LOG_ERR("Bad NVS file");
		return ret;
	}

	LOG_DBG("Radio type 0x%02x, NVS version %u.%u",
		(wl1251_reg_read32(dev, WL1251_REG_SCR_PAD2) >> 8) & 0xff,
		(wl1251_reg_read32(dev, WL1251_REG_SCR_PAD2) >> 16) & 0xff,
		(wl1251_reg_read32(dev, WL1251_REG_SCR_PAD3) >> 16) & 0xff);

	ret = init_clocks(dev);
	if (ret < 0) {
		return ret;
	}

	val = wl1251_reg_read32(dev, WL1251_REG_ECPU_CTRL);
	if ((val & WL1251_ECPU_HALT) == 0U) {
		LOG_ERR("Chip CPU is not halted (0x%08x)", val);
		return -EIO;
	}

	ret = upload_firmware(dev);
	if (ret < 0) {
		return ret;
	}

	ret = run_firmware(dev);
	if (ret < 0) {
		return ret;
	}

	/* The interrupt line follows the unmasked interrupts */
	wl1251_reg_write32(dev, WL1251_REG_INTR_MASK, ~data->intr_mask);
	wl1251_reg_write32(dev, WL1251_REG_HI_CFG, WL1251_HI_CFG_DEFAULT);

	mask = wl1251_cmd_buf(dev, sizeof(*mask));
	/* The mask has a bit set for each event that is not wanted */
	mask->event_mask = sys_cpu_to_le32(
		~(WL1251_EVENT_SCAN_COMPLETE | WL1251_EVENT_JOIN_COMPLETE | WL1251_EVENT_BSS_LOSE));
	mask->high_event_mask = UINT32_MAX;
	ret = wl1251_cmd_configure(dev, WL1251_IE_EVENT_MBOX_MASK, mask, sizeof(*mask));
	if (ret < 0) {
		return ret;
	}

	return configure(dev);
}
