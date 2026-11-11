/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * TI WL1251 802.11b/g controller on SPI.
 *
 * The firmware scans, keeps the link to the access point (beacons, power
 * save) and encrypts. It passes whole 802.11 frames to and from the host,
 * which authenticates and associates and turns data frames into Ethernet
 * frames for the network stack.
 */

#define DT_DRV_COMPAT ti_wl1251

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/wifi_utils.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>

#include "wl1251.h"

LOG_MODULE_REGISTER(wifi_wl1251, CONFIG_WIFI_LOG_LEVEL);

/* Passes through the interrupt register before the line is released */
#define IRQ_MAX_LOOPS 10

/* Per channel, in TU */
#define SCAN_MIN_DURATION  30000
#define SCAN_MAX_DURATION  60000
#define SCAN_NUM_PROBES    3
#define SCAN_FIRST_CHANNEL 1
#define SCAN_LAST_CHANNEL  13

#define RX_FILTER_DEFAULT                                                                          \
	(WL1251_RX_FILTER_PROBE_RESP | WL1251_RX_FILTER_MGMT | WL1251_RX_FILTER_DATA |             \
	 WL1251_RX_FILTER_BEACON | WL1251_RX_FILTER_AUTH | WL1251_RX_FILTER_ASSOC)

/* How long a frame waits for room in the chip */
#define TX_WAIT_MS 500

#define JOIN_TIMEOUT_MS 500

/* Frames in a row that move the transmit rate up or down */
#define RATE_UP_AFTER   10
#define RATE_DOWN_AFTER 2

#define ETH_TYPE_MIN 0x0600

#define AUTH_ALG_OPEN 0

static const uint8_t wpa_oui_type[] = {0x00, 0x50, 0xf2, 0x01};
static const uint8_t rsn_oui[] = {0x00, 0x0f, 0xac};
static const uint8_t snap_rfc1042[] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};
static const uint8_t snap_bridge_tunnel[] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0xf8};
static const uint8_t broadcast[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/*
 * The rates, with their values in a Supported Rates element (in 500 kbit/s),
 * in the order the rate adaptation steps through them. It skips 6 and 9
 * Mbit/s, which reach no further than 11 Mbit/s.
 */
static const struct {
	uint16_t rate;
	uint8_t ie;
	bool step;
} rates[] = {
	{WL1251_RATE_1M, 2, true},   {WL1251_RATE_2M, 4, true},   {WL1251_RATE_5_5M, 11, true},
	{WL1251_RATE_6M, 12, false}, {WL1251_RATE_9M, 18, false}, {WL1251_RATE_11M, 22, true},
	{WL1251_RATE_12M, 24, true}, {WL1251_RATE_18M, 36, true}, {WL1251_RATE_24M, 48, true},
	{WL1251_RATE_36M, 72, true}, {WL1251_RATE_48M, 96, true}, {WL1251_RATE_54M, 108, true},
};

#define RATE_IDX_11M 5

/* What this station says it supports, basic rates marked */
static const uint8_t supp_rates_ie[] = {
	WL1251_IE_ID_RATES, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
};
static const uint8_t ext_rates_ie[] = {WL1251_IE_ID_EXT_RATES, 4, 0x30, 0x48, 0x60, 0x6c};

uint16_t wl1251_ie_rates(const uint8_t *ie)
{
	uint16_t mask = 0;

	if (ie == NULL) {
		return 0;
	}

	for (uint8_t i = 0; i < ie[1]; i++) {
		ARRAY_FOR_EACH_PTR(rates, rate) {
			if ((ie[2 + i] & 0x7f) == rate->ie) {
				mask |= rate->rate;
			}
		}
	}

	return mask;
}

/* The first element with the given ID, or NULL */
const uint8_t *wl1251_find_ie(const uint8_t *ies, size_t len, uint8_t id)
{
	while (len >= 2U && len >= 2U + ies[1]) {
		if (ies[0] == id) {
			return ies;
		}
		len -= 2U + ies[1];
		ies += 2U + ies[1];
	}

	return NULL;
}

static bool has_wpa_ie(const uint8_t *ies, size_t len)
{
	while (len >= 2U && len >= 2U + ies[1]) {
		if (ies[0] == WL1251_IE_ID_VENDOR && ies[1] >= sizeof(wpa_oui_type) &&
		    memcmp(&ies[2], wpa_oui_type, sizeof(wpa_oui_type)) == 0) {
			return true;
		}
		len -= 2U + ies[1];
		ies += 2U + ies[1];
	}

	return false;
}

/* What the network asks for, from its RSN element's key management suites */
static enum wifi_security_type bss_security(const struct wl1251_bss *bss,
					    enum wifi_mfp_options *mfp)
{
	const uint8_t *rsn = wl1251_find_ie(bss->ies, bss->ies_len, WL1251_IE_ID_RSN);
	enum wifi_security_type security = WIFI_SECURITY_TYPE_UNKNOWN;
	bool psk = false;
	bool sae = false;
	const uint8_t *ptr;
	size_t len;
	uint16_t count;

	*mfp = WIFI_MFP_DISABLE;

	if (rsn == NULL) {
		if (has_wpa_ie(bss->ies, bss->ies_len)) {
			return WIFI_SECURITY_TYPE_WPA_PSK;
		}

		return (bss->capability & WL1251_CAPA_PRIVACY) != 0U ? WIFI_SECURITY_TYPE_WEP
								     : WIFI_SECURITY_TYPE_NONE;
	}

	/* Version, group cipher, then the pairwise cipher list */
	ptr = &rsn[2];
	len = rsn[1];
	if (len < 8U) {
		return security;
	}
	count = sys_get_le16(&ptr[6]);
	if (len < 8U + count * 4U + 2U) {
		return security;
	}
	ptr += 8U + count * 4U;
	len -= 8U + count * 4U;

	count = sys_get_le16(ptr);
	ptr += 2;
	len -= 2U;
	if (len < count * 4U) {
		return security;
	}

	for (uint16_t i = 0; i < count; i++, ptr += 4, len -= 4U) {
		if (memcmp(ptr, rsn_oui, sizeof(rsn_oui)) != 0) {
			continue;
		}

		switch (ptr[3]) {
		case 1:
		case 5:
			security = WIFI_SECURITY_TYPE_EAP;
			break;
		case 2:
			psk = true;
			security = WIFI_SECURITY_TYPE_PSK;
			break;
		case 6:
			psk = true;
			security = WIFI_SECURITY_TYPE_PSK_SHA256;
			break;
		case 8:
			sae = true;
			security = WIFI_SECURITY_TYPE_SAE;
			break;
		default:
			break;
		}
	}

	if (psk && sae) {
		security = WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL;
	}

	/* RSN capabilities: management frame protection capable and required */
	if (len >= 2U) {
		uint16_t capa = sys_get_le16(ptr);

		if ((capa & BIT(6)) != 0U) {
			*mfp = WIFI_MFP_REQUIRED;
		} else if ((capa & BIT(7)) != 0U) {
			*mfp = WIFI_MFP_OPTIONAL;
		}
	}

	return security;
}

/* Keeps a beacon or probe response for the scan results */
static void scan_rx(const struct device *dev, const struct wl1251_rx_desc *desc,
		    const uint8_t *frame, size_t len)
{
	struct wl1251_data *data = dev->data;
	const struct wl1251_80211_hdr *hdr = (const void *)frame;
	const struct wl1251_80211_beacon *beacon = (const void *)(hdr + 1);
	const uint8_t *ies = (const uint8_t *)(beacon + 1);
	size_t ies_len = len - sizeof(*hdr) - sizeof(*beacon);
	struct wl1251_bss *bss = NULL;
	const uint8_t *ds;

	ARRAY_FOR_EACH_PTR(data->bss, entry) {
		if (!entry->valid) {
			if (bss == NULL) {
				bss = entry;
			}
		} else if (memcmp(entry->bssid, hdr->addr3, sizeof(entry->bssid)) == 0) {
			bss = entry;
			break;
		}
	}

	if (bss == NULL) {
		LOG_DBG("No room for more scan results");
		return;
	}

	/*
	 * A probe response is preferred to a beacon: the beacon of a hidden
	 * network has no SSID.
	 */
	if (bss->valid &&
	    (sys_le16_to_cpu(hdr->frame_control) & WL1251_FC_STYPE_MASK) == WL1251_FC_BEACON &&
	    bss->probe_resp) {
		bss->rssi = desc->rssi;
		return;
	}

	memcpy(bss->bssid, hdr->addr3, sizeof(bss->bssid));
	bss->valid = true;
	bss->probe_resp =
		(sys_le16_to_cpu(hdr->frame_control) & WL1251_FC_STYPE_MASK) != WL1251_FC_BEACON;
	bss->rssi = desc->rssi;
	bss->beacon_interval = sys_le16_to_cpu(beacon->beacon_interval);
	bss->capability = sys_le16_to_cpu(beacon->capability);
	bss->ies_len = MIN(ies_len, sizeof(bss->ies));
	memcpy(bss->ies, ies, bss->ies_len);

	/* The channel the network is on, not the one it was heard on */
	ds = wl1251_find_ie(ies, ies_len, WL1251_IE_ID_DS_PARAMS);
	bss->channel = (ds != NULL && ds[1] == 1U) ? ds[2] : desc->channel;
}

/* Ends a scan, whoever asked for it */
static void scan_report(const struct device *dev, bool aborted)
{
	struct wl1251_data *data = dev->data;
	scan_result_cb_t cb = data->scan_cb;

	data->scanning = false;
	data->scan_cb = NULL;

	if (data->scan_from_supp) {
		data->scan_from_supp = false;
		if (IS_ENABLED(CONFIG_WIFI_NM_WPA_SUPPLICANT)) {
			wl1251_supp_scan_done(dev, aborted);
		}
		return;
	}

	if (cb == NULL) {
		return;
	}

	ARRAY_FOR_EACH_PTR(data->bss, bss) {
		struct wifi_scan_result result = {0};
		const uint8_t *ssid;

		if (!bss->valid || aborted) {
			continue;
		}

		ssid = wl1251_find_ie(bss->ies, bss->ies_len, WL1251_IE_ID_SSID);
		if (ssid != NULL) {
			result.ssid_length = MIN(ssid[1], WIFI_SSID_MAX_LEN);
			memcpy(result.ssid, &ssid[2], result.ssid_length);
		}
		result.band = WIFI_FREQ_BAND_2_4_GHZ;
		result.channel = bss->channel;
		result.security = bss_security(bss, &result.mfp);
		result.rssi = bss->rssi;
		memcpy(result.mac, bss->bssid, sizeof(bss->bssid));
		result.mac_length = sizeof(bss->bssid);

		cb(data->iface, 0, &result);
	}

	cb(data->iface, aborted ? -ECANCELED : 0, NULL);
}

/*
 * Hands the frame in the transmit buffer, behind the room for its
 * descriptor, to the firmware. -EBUSY when there is no room for it yet.
 */
static int tx_frame(const struct device *dev, size_t len, uint8_t queue, uint16_t rate, bool adapt)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_tx_desc *desc = (void *)data->tx_frame;
	uint32_t addr = sys_le32_to_cpu(data->path.tx_ring_addr);
	uint32_t out_count;
	uint8_t id;
	int ret;

	/* Both halves of the ring still hold frames the firmware has not taken */
	out_count = wl1251_mem_read32(dev, sys_le32_to_cpu(data->path.tx_control_addr)) & 0xf;
	if (((data->tx_in_count - out_count) & 0xf) >= WL1251_TX_RING_CHUNKS) {
		return -EBUSY;
	}

	if (data->tx_busy == UINT16_MAX) {
		return -EBUSY;
	}
	id = find_lsb_set(~data->tx_busy) - 1;

	memset(desc, 0, sizeof(*desc));
	desc->length = sys_cpu_to_le16(len);
	desc->rate = sys_cpu_to_le16(rate);
	desc->expiry_time = sys_cpu_to_le32(BIT(16));
	desc->xmit_queue = queue;
	desc->id = id;
	desc->control = sys_cpu_to_le16(WL1251_TX_CTRL_TX_COMPLETE);
	/* No fragmentation */
	desc->frag_threshold = sys_cpu_to_le16(WL1251_MAX_FRAME_LEN);
	/* With room for what encryption adds */
	desc->num_mem_blocks = (len + 16U) / WL1251_TX_BLOCK_SIZE + 1U;

	if ((data->tx_in_count & 1U) != 0U) {
		addr += sys_le16_to_cpu(data->path.tx_chunk_size);
	}

	ret = wl1251_mem_write(dev, addr, data->tx_frame, ROUND_UP(sizeof(*desc) + len, 4));
	if (ret < 0) {
		return ret;
	}

	if ((data->tx_in_count & 1U) != 0U) {
		wl1251_reg_write32(dev, WL1251_REG_INTR_TRIG_H, WL1251_TRIG_TX_PROC1);
	} else {
		wl1251_reg_write32(dev, WL1251_REG_INTR_TRIG, WL1251_TRIG_TX_PROC0);
	}

	data->tx_in_count = (data->tx_in_count + 1U) & 0xf;
	data->tx_busy |= BIT(id);
	WRITE_BIT(data->tx_adapt, id, adapt);

	return 0;
}

/* The same, waiting for room. Needs the transmit lock as well. */
static int tx_frame_wait(const struct device *dev, size_t len, uint8_t queue, uint16_t rate,
			 bool adapt)
{
	struct wl1251_data *data = dev->data;
	k_timepoint_t end = sys_timepoint_calc(K_MSEC(TX_WAIT_MS));
	int ret;

	while (true) {
		ret = tx_frame(dev, len, queue, rate, adapt);
		if (ret != -EBUSY) {
			return ret;
		}
		if (sys_timepoint_expired(end)) {
			return -ENOBUFS;
		}

		/* The driver thread needs the lock to make room */
		k_mutex_unlock(&data->lock);
		(void)k_sem_take(&data->tx_sem, K_MSEC(10));
		k_mutex_lock(&data->lock, K_FOREVER);

		if (!data->running) {
			return -ENETDOWN;
		}
	}
}

uint32_t wl1251_tx_rate_kbps(const struct device *dev)
{
	struct wl1251_data *data = dev->data;

	return rates[data->tx_rate].ie * 500U;
}

/* Moves the transmit rate to the next one the access point takes */
static void tx_rate_step(const struct device *dev, bool up)
{
	struct wl1251_data *data = dev->data;
	int idx = data->tx_rate;

	while (true) {
		idx += up ? 1 : -1;
		if (idx < 0 || idx >= ARRAY_SIZE(rates)) {
			return;
		}
		if (rates[idx].step && (data->bss_params.rates & rates[idx].rate) != 0U) {
			break;
		}
	}

	LOG_DBG("TX rate %u -> %u", rates[data->tx_rate].ie / 2U, rates[idx].ie / 2U);
	data->tx_rate = idx;
}

static void handle_tx_results(const struct device *dev)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_tx_result results[WL1251_TX_RESULTS];
	uint32_t addr = sys_le32_to_cpu(data->path.tx_complete_addr);
	uint8_t first = data->tx_next_result;
	uint8_t count = 0;

	if (wl1251_mem_read(dev, addr, results, sizeof(results)) < 0) {
		return;
	}

	/* The firmware fills the entries in turn */
	while (count < WL1251_TX_RESULTS) {
		struct wl1251_tx_result *result = &results[data->tx_next_result];
		bool adapt = false;

		if (result->done_1 != 1U || result->done_2 != 1U) {
			break;
		}

		if (result->id < WL1251_TX_RESULTS) {
			adapt = (data->tx_adapt & BIT(result->id)) != 0U;
			data->tx_busy &= ~BIT(result->id);
			data->tx_adapt &= ~BIT(result->id);
		}

		if (result->status != 0U) {
			LOG_DBG("TX failed: status 0x%02x after %u retries", result->status,
				result->ack_failures);
		}

		if (!adapt) {
			/* Sent at a fixed rate */
		} else if (result->status != 0U || result->ack_failures > 1U) {
			data->tx_good = 0;
			if (++data->tx_bad >= RATE_DOWN_AFTER) {
				data->tx_bad = 0;
				tx_rate_step(dev, false);
			}
		} else if (result->ack_failures == 0U) {
			data->tx_bad = 0;
			if (++data->tx_good >= RATE_UP_AFTER) {
				data->tx_good = 0;
				tx_rate_step(dev, true);
			}
		}

		result->done_1 = 0;
		result->done_2 = 0;
		data->tx_next_result = (data->tx_next_result + 1U) % WL1251_TX_RESULTS;
		count++;
	}

	if (count == 0U) {
		return;
	}

	/*
	 * Free the entries for the firmware: these and no others, as it may
	 * have filled more of them since they were read.
	 */
	if (first + count <= WL1251_TX_RESULTS) {
		(void)wl1251_mem_write(dev, addr + first * sizeof(results[0]), &results[first],
				       count * sizeof(results[0]));
	} else {
		uint8_t tail = WL1251_TX_RESULTS - first;

		(void)wl1251_mem_write(dev, addr + first * sizeof(results[0]), &results[first],
				       tail * sizeof(results[0]));
		(void)wl1251_mem_write(dev, addr, results, (count - tail) * sizeof(results[0]));
	}

	k_sem_give(&data->tx_sem);
}

/* Starts a management frame to the access point in the transmit buffer */
static uint8_t *mgmt_frame(const struct device *dev, uint16_t frame_control)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_80211_hdr *hdr = (void *)&data->tx_frame[sizeof(struct wl1251_tx_desc)];

	hdr->frame_control = sys_cpu_to_le16(frame_control);
	hdr->duration = 0;
	memcpy(hdr->addr1, data->bss_params.bssid, sizeof(hdr->addr1));
	memcpy(hdr->addr2, data->mac, sizeof(hdr->addr2));
	memcpy(hdr->addr3, data->bss_params.bssid, sizeof(hdr->addr3));
	hdr->seq_ctrl = sys_cpu_to_le16(data->tx_seq++ << 4);

	return (uint8_t *)(hdr + 1);
}

static int mgmt_send(const struct device *dev, const uint8_t *end)
{
	struct wl1251_data *data = dev->data;
	size_t len = end - &data->tx_frame[sizeof(struct wl1251_tx_desc)];

	return tx_frame_wait(dev, len, WL1251_QUEUE_VO, WL1251_RATE_1M, false);
}

/*
 * Tunes the firmware to the network: its channel, beacons and frames. Lets
 * go of the lock while it waits, for the driver thread to tell.
 */
static int join(const struct device *dev)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_80211_hdr null_data = {
		.frame_control = sys_cpu_to_le16(WL1251_FC_NULL_DATA | WL1251_FC_TO_DS),
	};
	struct wl1251_ie_frame_rates *frame_rates;
	struct wl1251_cmd_join *cmd;
	int ret;

	/* What the firmware sends on its own goes out at 2 Mbit/s, long preamble */
	frame_rates = wl1251_cmd_buf(dev, sizeof(*frame_rates));
	frame_rates->ctrl_rate = 0x14;
	frame_rates->mgmt_rate = 0x14;
	ret = wl1251_cmd_configure(dev, WL1251_IE_FW_GEN_FRAME_RATES, frame_rates,
				   sizeof(*frame_rates));
	if (ret < 0) {
		return ret;
	}

	memcpy(null_data.addr1, data->bss_params.bssid, sizeof(null_data.addr1));
	memcpy(null_data.addr2, data->mac, sizeof(null_data.addr2));
	memcpy(null_data.addr3, data->bss_params.bssid, sizeof(null_data.addr3));
	ret = wl1251_cmd_template(dev, WL1251_CMD_NULL_DATA, &null_data, sizeof(null_data));
	if (ret < 0) {
		return ret;
	}

	cmd = wl1251_cmd_buf(dev, sizeof(*cmd));
	sys_memcpy_swap(cmd->bssid, data->bss_params.bssid, sizeof(cmd->bssid));
	cmd->beacon_interval = sys_cpu_to_le16(data->bss_params.beacon_interval);
	/* Frames of this network to this station, and all of its multicast */
	cmd->rx_config = sys_cpu_to_le32(WL1251_RX_CFG_UNICAST | WL1251_RX_CFG_BSSID);
	cmd->rx_filter = sys_cpu_to_le32(data->rx_filter);
	cmd->basic_rate_set = sys_cpu_to_le16(WL1251_RATE_1M | WL1251_RATE_2M | WL1251_RATE_5_5M |
					      WL1251_RATE_11M);
	cmd->dtim_interval = 1;
	cmd->bss_type = WL1251_BSS_TYPE_STA;
	cmd->channel = data->bss_params.channel;
	cmd->ctrl = WL1251_JOIN_CTRL_TX_FLUSH;

	/* The firmware drops what was still to be sent */
	data->tx_busy = 0;

	k_sem_reset(&data->join_sem);
	ret = wl1251_cmd_send(dev, WL1251_CMD_START_JOIN, cmd, sizeof(*cmd));
	if (ret == 0) {
		ret = wl1251_cmd_status(dev, WL1251_CMD_START_JOIN);
	}
	if (ret < 0) {
		return ret;
	}

	k_mutex_unlock(&data->lock);
	ret = k_sem_take(&data->join_sem, K_MSEC(JOIN_TIMEOUT_MS));
	k_mutex_lock(&data->lock, K_FOREVER);

	if (!data->running) {
		return -ENETDOWN;
	}
	if (ret < 0) {
		LOG_WRN("Join did not complete");
		return -ETIMEDOUT;
	}

	data->joined = true;

	return 0;
}

/*
 * Sets or removes a key. The firmware wants the key itself for removing it
 * as well, so the keys are kept in the slot they were set from: the group
 * keys by index, then the pairwise key.
 */
static int key_cmd(const struct device *dev, uint16_t action, bool pairwise, uint8_t idx)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_cmd_set_keys *cmd = wl1251_cmd_buf(dev, sizeof(*cmd));
	const uint8_t *key = data->keys[pairwise ? WL1251_PAIRWISE_KEY_SLOT : idx];
	int ret;

	memcpy(cmd->addr, pairwise ? data->bss_params.bssid : broadcast, sizeof(cmd->addr));
	cmd->action = sys_cpu_to_le16(action);
	cmd->key_type = pairwise ? WL1251_KEY_AES_PAIRWISE : WL1251_KEY_AES_GROUP;
	cmd->id = idx;
	cmd->key_size = WL1251_KEY_SIZE;
	memcpy(cmd->key, key, WL1251_KEY_SIZE);

	ret = wl1251_cmd_send(dev, WL1251_CMD_SET_KEYS, cmd, sizeof(*cmd));
	if (ret == 0) {
		ret = wl1251_cmd_status(dev, WL1251_CMD_SET_KEYS);
	}

	/* Not to be left lying in the scratch buffer */
	memset(cmd, 0, sizeof(*cmd));

	return ret;
}

static void remove_keys(const struct device *dev)
{
	struct wl1251_data *data = dev->data;

	if (data->pairwise_key) {
		data->pairwise_key = false;
		if (data->running) {
			(void)key_cmd(dev, WL1251_KEY_REMOVE, true, data->pairwise_key_idx);
		}
	}

	for (uint8_t idx = 0; data->group_keys != 0U; idx++) {
		if ((data->group_keys & BIT(idx)) != 0U) {
			data->group_keys &= ~BIT(idx);
			if (data->running) {
				(void)key_cmd(dev, WL1251_KEY_REMOVE, false, idx);
			}
		}
	}

	memset(data->keys, 0, sizeof(data->keys));
}

/* Back to no network, without telling the access point */
static void disconnected(const struct device *dev)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_cmd_template *cmd;

	if (data->state == WL1251_STATE_ASSOCIATED) {
		net_if_carrier_off(data->iface);
	}

	data->state = WL1251_STATE_IDLE;
	data->authorized = false;
	remove_keys(dev);

	if (data->joined && data->running) {
		/* The firmware stops following the network's beacons */
		cmd = wl1251_cmd_buf(dev, sizeof(*cmd));
		(void)wl1251_cmd_send(dev, WL1251_CMD_DISCONNECT, cmd,
				      offsetof(struct wl1251_cmd_template, data));
	}
	data->joined = false;
}

int wl1251_authenticate(const struct device *dev, const struct wl1251_bss_params *bss,
			uint16_t algorithm)
{
	struct wl1251_data *data = dev->data;
	uint8_t *ptr;
	int ret;

	k_mutex_lock(&data->tx_lock, K_FOREVER);
	k_mutex_lock(&data->lock, K_FOREVER);

	if (!data->running) {
		ret = -ENETDOWN;
		goto out;
	}

	disconnected(dev);

	data->bss_params = *bss;
	ret = join(dev);
	if (ret < 0) {
		goto out;
	}

	/* Algorithm, transaction 1 of it, and a status of no meaning yet */
	ptr = mgmt_frame(dev, WL1251_FC_AUTH);
	sys_put_le16(algorithm, &ptr[0]);
	sys_put_le16(1, &ptr[2]);
	sys_put_le16(0, &ptr[4]);
	ret = mgmt_send(dev, &ptr[6]);
	if (ret < 0) {
		goto out;
	}

	data->state = WL1251_STATE_AUTHENTICATING;

out:
	k_mutex_unlock(&data->lock);
	k_mutex_unlock(&data->tx_lock);

	return ret;
}

int wl1251_associate(const struct device *dev, const uint8_t *ssid, size_t ssid_len,
		     const uint8_t *ies, size_t ies_len)
{
	struct wl1251_data *data = dev->data;
	uint16_t capability = WL1251_CAPA_ESS;
	uint8_t *ptr;
	uint8_t *start;
	int ret;

	if (ssid_len > WIFI_SSID_MAX_LEN ||
	    2U + ssid_len + sizeof(supp_rates_ie) + sizeof(ext_rates_ie) + ies_len >
		    sizeof(data->assoc_ies)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->tx_lock, K_FOREVER);
	k_mutex_lock(&data->lock, K_FOREVER);

	if (!data->running) {
		ret = -ENETDOWN;
		goto out;
	}
	if (data->state != WL1251_STATE_AUTHENTICATED) {
		ret = -ENOTCONN;
		goto out;
	}

	/* The network's choices that this station can follow */
	capability |= data->bss_params.capability &
		      (WL1251_CAPA_PRIVACY | WL1251_CAPA_SHORT_PRE | WL1251_CAPA_SHORT_SLOT);

	/* Capability, and a listen interval of one beacon */
	ptr = mgmt_frame(dev, WL1251_FC_ASSOC_REQ);
	sys_put_le16(capability, &ptr[0]);
	sys_put_le16(1, &ptr[2]);
	ptr += 4;

	start = ptr;
	*ptr++ = WL1251_IE_ID_SSID;
	*ptr++ = ssid_len;
	memcpy(ptr, ssid, ssid_len);
	ptr += ssid_len;
	memcpy(ptr, supp_rates_ie, sizeof(supp_rates_ie));
	ptr += sizeof(supp_rates_ie);
	memcpy(ptr, ext_rates_ie, sizeof(ext_rates_ie));
	ptr += sizeof(ext_rates_ie);
	if (ies_len > 0U) {
		memcpy(ptr, ies, ies_len);
		ptr += ies_len;
	}

	data->assoc_ies_len = ptr - start;
	memcpy(data->assoc_ies, start, data->assoc_ies_len);

	ret = mgmt_send(dev, ptr);
	if (ret < 0) {
		goto out;
	}

	data->state = WL1251_STATE_ASSOCIATING;

out:
	k_mutex_unlock(&data->lock);
	k_mutex_unlock(&data->tx_lock);

	return ret;
}

int wl1251_deauthenticate(const struct device *dev, uint16_t reason)
{
	struct wl1251_data *data = dev->data;
	uint8_t *ptr;

	k_mutex_lock(&data->tx_lock, K_FOREVER);
	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->running && data->state != WL1251_STATE_IDLE) {
		ptr = mgmt_frame(dev, WL1251_FC_DEAUTH);
		sys_put_le16(reason, ptr);
		(void)mgmt_send(dev, &ptr[2]);
		/* Let it out before the firmware leaves the channel */
		k_msleep(20);
	}

	disconnected(dev);

	k_mutex_unlock(&data->lock);
	k_mutex_unlock(&data->tx_lock);

	return 0;
}

/* A CCMP key, for the chip to encrypt and decrypt with */
int wl1251_set_key(const struct device *dev, bool pairwise, uint8_t idx, const uint8_t *key,
		   size_t key_len)
{
	struct wl1251_data *data = dev->data;
	int ret;

	if (key_len != WL1251_KEY_SIZE || idx >= WL1251_PAIRWISE_KEY_SLOT) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (!data->running || data->state != WL1251_STATE_ASSOCIATED) {
		ret = -ENOTCONN;
		goto out;
	}

	memcpy(data->keys[pairwise ? WL1251_PAIRWISE_KEY_SLOT : idx], key, key_len);
	ret = key_cmd(dev, WL1251_KEY_ADD, pairwise, idx);
	if (ret < 0) {
		goto out;
	}

	if (pairwise) {
		data->pairwise_key = true;
		data->pairwise_key_idx = idx;
		/* A new key starts its packet numbers over */
		data->tx_pn = 0;
	} else {
		data->group_keys |= BIT(idx);
	}

out:
	k_mutex_unlock(&data->lock);

	return ret;
}

int wl1251_remove_key(const struct device *dev, bool pairwise, uint8_t idx)
{
	struct wl1251_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (pairwise) {
		if (data->pairwise_key && data->pairwise_key_idx == idx) {
			data->pairwise_key = false;
			if (data->running) {
				ret = key_cmd(dev, WL1251_KEY_REMOVE, true, idx);
			}
		}
	} else if (idx < WL1251_PAIRWISE_KEY_SLOT && (data->group_keys & BIT(idx)) != 0U) {
		data->group_keys &= ~BIT(idx);
		if (data->running) {
			ret = key_cmd(dev, WL1251_KEY_REMOVE, false, idx);
		}
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

void wl1251_set_authorized(const struct device *dev, bool authorized)
{
	struct wl1251_data *data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->authorized = authorized && data->state == WL1251_STATE_ASSOCIATED;
	k_mutex_unlock(&data->lock);
}

/* The access point took this station in */
static void associated(const struct device *dev, uint16_t aid)
{
	struct wl1251_data *data = dev->data;
	/* A PS-Poll: frame control, the AID with its two top bits set, addresses */
	struct {
		uint16_t frame_control;
		uint16_t aid;
		uint8_t bssid[6];
		uint8_t ta[6];
	} __packed ps_poll = {
		.frame_control = sys_cpu_to_le16(WL1251_FC_PS_POLL),
		.aid = sys_cpu_to_le16(aid | 0xc000),
	};
	struct wl1251_ie_slot *slot;

	data->aid = aid;
	data->state = WL1251_STATE_ASSOCIATED;
	data->tx_good = 0;
	data->tx_bad = 0;

	/* Start at 11 Mbit/s, or the best there is below it */
	data->tx_rate = RATE_IDX_11M;
	if ((data->bss_params.rates & WL1251_RATE_11M) == 0U) {
		data->tx_rate = 0;
		for (int i = RATE_IDX_11M; i > 0; i--) {
			if ((data->bss_params.rates & rates[i].rate) != 0U) {
				data->tx_rate = i;
				break;
			}
		}
	}

	memcpy(ps_poll.bssid, data->bss_params.bssid, sizeof(ps_poll.bssid));
	memcpy(ps_poll.ta, data->mac, sizeof(ps_poll.ta));
	(void)wl1251_cmd_template(dev, WL1251_CMD_PS_POLL, &ps_poll, sizeof(ps_poll));

	(void)wl1251_cfg_u16(dev, WL1251_IE_AID, aid);

	slot = wl1251_cmd_buf(dev, sizeof(*slot));
	slot->slot_time = (data->bss_params.capability & WL1251_CAPA_SHORT_SLOT) != 0U ? 1 : 0;
	(void)wl1251_cmd_configure(dev, WL1251_IE_SLOT, slot, sizeof(*slot));

	(void)wl1251_cfg_u8(dev, WL1251_IE_PREAMBLE_TYPE,
			    (data->bss_params.capability & WL1251_CAPA_SHORT_PRE) != 0U ? 1 : 0);

	net_if_carrier_on(data->iface);
}

static void handle_mgmt(const struct device *dev, const struct wl1251_rx_desc *desc,
			const uint8_t *frame, size_t len)
{
	struct wl1251_data *data = dev->data;
	const struct wl1251_80211_hdr *hdr = (const void *)frame;
	const uint8_t *body = (const uint8_t *)(hdr + 1);
	size_t body_len = len - sizeof(*hdr);
	uint16_t stype = sys_le16_to_cpu(hdr->frame_control) & WL1251_FC_STYPE_MASK;
	bool from_bss = data->state != WL1251_STATE_IDLE &&
			memcmp(hdr->addr3, data->bss_params.bssid, sizeof(hdr->addr3)) == 0;
	bool to_me = memcmp(hdr->addr1, data->mac, sizeof(hdr->addr1)) == 0;

	if (from_bss) {
		data->rssi = desc->rssi;
	}

	switch (stype) {
	case WL1251_FC_BEACON:
	case WL1251_FC_PROBE_RESP:
		if (data->scanning && body_len >= sizeof(struct wl1251_80211_beacon)) {
			scan_rx(dev, desc, frame, len);
		}
		return;
	case WL1251_FC_AUTH:
		if (!from_bss || !to_me || data->state != WL1251_STATE_AUTHENTICATING ||
		    body_len < 6U) {
			return;
		}
		/* The second and last frame of open system authentication */
		if (sys_get_le16(&body[0]) == AUTH_ALG_OPEN && sys_get_le16(&body[2]) == 2U &&
		    sys_get_le16(&body[4]) == 0U) {
			data->state = WL1251_STATE_AUTHENTICATED;
		}
		break;
	case WL1251_FC_ASSOC_RESP:
	case WL1251_FC_REASSOC_RESP:
		if (!from_bss || !to_me || data->state != WL1251_STATE_ASSOCIATING ||
		    body_len < 6U) {
			return;
		}
		/* Capability, status and the association ID */
		if (sys_get_le16(&body[2]) == 0U) {
			associated(dev, sys_get_le16(&body[4]) & 0x3fff);
		} else {
			data->state = WL1251_STATE_AUTHENTICATED;
		}
		break;
	case WL1251_FC_DEAUTH:
	case WL1251_FC_DISASSOC:
		if (!from_bss || body_len < 2U || (!to_me && (hdr->addr1[0] & BIT(0)) == 0U)) {
			return;
		}
		LOG_DBG("%s, reason %u",
			stype == WL1251_FC_DEAUTH ? "Deauthenticated" : "Disassociated",
			sys_get_le16(body));
		break;
	default:
		return;
	}

	if (IS_ENABLED(CONFIG_WIFI_NM_WPA_SUPPLICANT)) {
		wl1251_supp_mgmt_rx(dev, frame, len);
	}

	if (stype == WL1251_FC_DEAUTH || stype == WL1251_FC_DISASSOC) {
		disconnected(dev);
	}
}

/* A data frame from the access point, on to the network stack as Ethernet */
static void handle_data(const struct device *dev, uint16_t flags, const uint8_t *frame, size_t len)
{
	struct wl1251_data *data = dev->data;
	const struct wl1251_80211_hdr *hdr = (const void *)frame;
	uint16_t fc = sys_le16_to_cpu(hdr->frame_control);
	size_t hdr_len = sizeof(*hdr);
	struct net_eth_hdr eth;
	struct net_pkt *pkt;
	const uint8_t *payload;
	size_t payload_len;
	uint16_t type;

	if (data->state != WL1251_STATE_ASSOCIATED ||
	    (fc & (WL1251_FC_TO_DS | WL1251_FC_FROM_DS)) != WL1251_FC_FROM_DS ||
	    (fc & WL1251_FC_STYPE_NULL) != 0U ||
	    memcmp(hdr->addr2, data->bss_params.bssid, sizeof(hdr->addr2)) != 0 ||
	    memcmp(hdr->addr3, data->mac, sizeof(hdr->addr3)) == 0) {
		return;
	}

	if ((fc & WL1251_FC_STYPE_QOS) != 0U) {
		hdr_len += 2U;
	}

	/* The chip has decrypted the frame and taken out the CCMP header */
	if ((fc & WL1251_FC_PROTECTED) != 0U &&
	    ((flags & WL1251_RX_FLAG_ENCRYPTED) == 0U ||
	     (flags & (WL1251_RX_FLAG_DECRYPT_FAIL | WL1251_RX_FLAG_MIC_FAIL)) != 0U)) {
		LOG_DBG("Not decrypted (0x%04x)", flags);
		return;
	}

	if (len < hdr_len + 2U) {
		return;
	}
	payload = frame + hdr_len;
	payload_len = len - hdr_len;

	if (payload_len >= sizeof(snap_rfc1042) + 2U &&
	    (memcmp(payload, snap_rfc1042, sizeof(snap_rfc1042)) == 0 ||
	     memcmp(payload, snap_bridge_tunnel, sizeof(snap_bridge_tunnel)) == 0)) {
		type = sys_get_be16(&payload[sizeof(snap_rfc1042)]);
		payload += sizeof(snap_rfc1042) + 2U;
		payload_len -= sizeof(snap_rfc1042) + 2U;
	} else {
		/* An 802.3 frame with a length */
		type = payload_len;
	}

	/*
	 * Until the keys are set only the key handshake passes, and after
	 * that nothing else that came unencrypted.
	 */
	if (type != NET_ETH_PTYPE_EAPOL &&
	    (!data->authorized || (data->pairwise_key && (fc & WL1251_FC_PROTECTED) == 0U))) {
		return;
	}

	memcpy(eth.dst.addr, hdr->addr1, sizeof(eth.dst.addr));
	memcpy(eth.src.addr, hdr->addr3, sizeof(eth.src.addr));
	eth.type = sys_cpu_to_be16(type);

	pkt = net_pkt_rx_alloc_with_buffer(data->iface, sizeof(eth) + payload_len, NET_AF_UNSPEC, 0,
					   K_NO_WAIT);
	if (pkt == NULL) {
		LOG_DBG("No buffer for a received frame");
		return;
	}

	if (net_pkt_write(pkt, &eth, sizeof(eth)) < 0 ||
	    net_pkt_write(pkt, payload, payload_len) < 0 || net_recv_data(data->iface, pkt) < 0) {
		net_pkt_unref(pkt);
	}
}

/* Takes one frame out of the receive buffer whose turn it is */
static void handle_rx(const struct device *dev)
{
	struct wl1251_data *data = dev->data;
	uint32_t addr = sys_le32_to_cpu(data->path.rx_ring_addr);
	struct wl1251_rx_desc desc;
	const struct wl1251_80211_hdr *hdr = (const void *)data->frame;
	uint16_t flags;
	size_t len;

	if (data->rx_buffer != 0U) {
		addr += sys_le16_to_cpu(data->path.rx_chunk_size);
	}

	if (wl1251_mem_read(dev, addr, &desc, sizeof(desc)) < 0) {
		return;
	}

	len = sys_le16_to_cpu(desc.length);
	flags = sys_le16_to_cpu(desc.flags);

	if (len < WL1251_RX_PLCP_LEN || len - WL1251_RX_PLCP_LEN > sizeof(data->frame)) {
		LOG_WRN("Bad receive length %zu", len);
	} else if (wl1251_mem_read(dev, addr + sizeof(desc) + WL1251_RX_FRAME_OFFSET, data->frame,
				   ROUND_UP(len - WL1251_RX_PLCP_LEN, 4)) == 0) {
		uint16_t type = sys_le16_to_cpu(hdr->frame_control) & WL1251_FC_TYPE_MASK;

		len -= WL1251_RX_PLCP_LEN;

		if ((flags & WL1251_RX_FLAG_VALID_FCS) == 0U || len < sizeof(*hdr)) {
			/* Nothing to do with it */
		} else if (type == WL1251_FC_TYPE_MGMT) {
			handle_mgmt(dev, &desc, data->frame, len);
		} else if (type == WL1251_FC_TYPE_DATA) {
			handle_data(dev, flags, data->frame, len);
		}
	}

	/* Hand the buffer back; the firmware fills the other one next */
	if (data->rx_buffer != 0U) {
		wl1251_reg_write32(dev, WL1251_REG_INTR_TRIG_H, WL1251_TRIG_RX_PROC1);
	} else {
		wl1251_reg_write32(dev, WL1251_REG_INTR_TRIG, WL1251_TRIG_RX_PROC0);
	}
	data->rx_buffer ^= 1U;
}

static void handle_event(const struct device *dev, uint8_t mbox_num)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_event_mbox mbox;
	uint32_t events;

	if (wl1251_mem_read(dev, data->event_mbox[mbox_num], &mbox, sizeof(mbox)) < 0) {
		return;
	}

	events = sys_le32_to_cpu(mbox.events_vector) & ~sys_le32_to_cpu(mbox.events_mask);
	LOG_DBG("Events 0x%08x", events);

	if ((events & WL1251_EVENT_SCAN_COMPLETE) != 0U && data->scanning) {
		scan_report(dev, false);
	}

	if ((events & WL1251_EVENT_JOIN_COMPLETE) != 0U) {
		k_sem_give(&data->join_sem);
	}

	/* No beacons for long enough that the network counts as gone */
	if ((events & WL1251_EVENT_BSS_LOSE) != 0U && data->state != WL1251_STATE_IDLE) {
		LOG_INF("Lost the network");
		if (IS_ENABLED(CONFIG_WIFI_NM_WPA_SUPPLICANT)) {
			wl1251_supp_link_lost(dev);
		}
		disconnected(dev);
	}

	wl1251_reg_write32(dev, WL1251_REG_INTR_TRIG, WL1251_TRIG_EVENT_ACK);
}

static void handle_irq(const struct device *dev)
{
	struct wl1251_data *data = dev->data;
	uint32_t intr;

	/* Nothing new on the line until this is through */
	wl1251_reg_write32(dev, WL1251_REG_INTR_MASK, UINT32_MAX);

	for (int i = 0; i < IRQ_MAX_LOOPS; i++) {
		uint32_t frames;

		/* Reading clears */
		intr = wl1251_reg_read32(dev, WL1251_REG_INTR_CLEAR);
		if (intr == UINT32_MAX) {
			LOG_ERR("Chip does not answer");
			break;
		}

		/*
		 * The receive interrupts can get lost: the firmware's frame
		 * counter says how many of the two buffers are full.
		 */
		data->rx_counter =
			wl1251_mem_read32(dev, sys_le32_to_cpu(data->path.rx_control_addr));
		frames = (data->rx_counter - data->rx_handled) & 0xf;
		data->rx_handled = data->rx_counter;
		if (frames > 2U) {
			LOG_WRN("Receive out of sync by %u", frames);
			frames = ((intr & WL1251_INTR_RX0_DATA) != 0U ? 1U : 0U) +
				 ((intr & WL1251_INTR_RX1_DATA) != 0U ? 1U : 0U);
		}

		intr &= data->intr_mask;
		if (intr == 0U && frames == 0U) {
			break;
		}

		while (frames-- > 0U) {
			handle_rx(dev);
		}

		if ((intr & WL1251_INTR_TX_RESULT) != 0U) {
			handle_tx_results(dev);
		}
		if ((intr & WL1251_INTR_EVENT_A) != 0U) {
			handle_event(dev, 0);
		}
		if ((intr & WL1251_INTR_EVENT_B) != 0U) {
			handle_event(dev, 1);
		}
	}

	wl1251_reg_write32(dev, WL1251_REG_INTR_MASK, ~data->intr_mask);
}

static void wl1251_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	struct wl1251_data *data = dev->data;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sem_take(&data->work_sem, K_FOREVER);

		k_mutex_lock(&data->lock, K_FOREVER);
		if (data->running) {
			handle_irq(dev);
		}
		k_mutex_unlock(&data->lock);
	}
}

static void wl1251_irq(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	struct wl1251_data *data = CONTAINER_OF(cb, struct wl1251_data, irq_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	k_sem_give(&data->work_sem);
}

int wl1251_scan_start(const struct device *dev, const struct wl1251_scan_req *req,
		      scan_result_cb_t cb)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_80211_hdr *hdr = (void *)data->frame;
	struct wl1251_cmd_trigger_scan_to *trigger;
	struct wl1251_cmd_scan *cmd;
	uint8_t *ptr;
	int ret;

	if (req->ssid_len > WIFI_SSID_MAX_LEN) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (!data->running) {
		ret = -ENETDOWN;
		goto out;
	}
	if (data->scanning) {
		ret = -EINPROGRESS;
		goto out;
	}

	/* The probe request that the firmware sends on each channel */
	memset(hdr, 0, sizeof(*hdr));
	hdr->frame_control = sys_cpu_to_le16(WL1251_FC_PROBE_REQ);
	memcpy(hdr->addr1, broadcast, sizeof(hdr->addr1));
	memcpy(hdr->addr2, data->mac, sizeof(hdr->addr2));
	memcpy(hdr->addr3, broadcast, sizeof(hdr->addr3));
	ptr = (uint8_t *)(hdr + 1);
	*ptr++ = WL1251_IE_ID_SSID;
	*ptr++ = req->ssid_len;
	if (req->ssid_len > 0U) {
		memcpy(ptr, req->ssid, req->ssid_len);
		ptr += req->ssid_len;
	}
	memcpy(ptr, supp_rates_ie, sizeof(supp_rates_ie));
	ptr += sizeof(supp_rates_ie);
	memcpy(ptr, ext_rates_ie, sizeof(ext_rates_ie));
	ptr += sizeof(ext_rates_ie);
	ret = wl1251_cmd_template(dev, WL1251_CMD_PROBE_REQ, data->frame, ptr - data->frame);
	if (ret < 0) {
		goto out;
	}

	/* The scan starts at once, not at some traffic trigger */
	trigger = wl1251_cmd_buf(dev, sizeof(*trigger));
	ret = wl1251_cmd_send(dev, WL1251_CMD_TRIGGER_SCAN_TO, trigger, sizeof(*trigger));
	if (ret < 0) {
		goto out;
	}

	cmd = wl1251_cmd_buf(dev, sizeof(*cmd));
	cmd->rx_config = sys_cpu_to_le32(WL1251_RX_CFG_ALL_GOOD);
	cmd->rx_filter = sys_cpu_to_le32(WL1251_RX_FILTER_PROBE_RESP | WL1251_RX_FILTER_MGMT |
					 WL1251_RX_FILTER_BEACON);
	/*
	 * Without the high priority the firmware fits the scan in around the
	 * beacons of a network it is not in, which can take minutes.
	 */
	cmd->options = sys_cpu_to_le16((req->passive ? WL1251_SCAN_OPT_PASSIVE : 0U) |
				       (data->joined ? 0U : WL1251_SCAN_OPT_PRIORITY_HIGH));
	cmd->num_probe_requests = SCAN_NUM_PROBES;
	cmd->tx_rate = sys_cpu_to_le16(WL1251_RATE_2M);
	if (req->ssid_len > 0U) {
		cmd->ssid_len = req->ssid_len;
		memcpy(cmd->ssid, req->ssid, req->ssid_len);
	}

	for (uint8_t channel = SCAN_FIRST_CHANNEL; channel <= SCAN_LAST_CHANNEL; channel++) {
		uint8_t i = cmd->num_channels;
		bool wanted = req->num_channels == 0U;

		for (uint8_t j = 0; j < req->num_channels; j++) {
			if (req->channels[j] == channel) {
				wanted = true;
			}
		}
		if (!wanted) {
			continue;
		}

		cmd->channels[i].min_duration = sys_cpu_to_le32(SCAN_MIN_DURATION);
		cmd->channels[i].max_duration = sys_cpu_to_le32(SCAN_MAX_DURATION);
		memset(cmd->channels[i].bssid, 0xff, sizeof(cmd->channels[i].bssid));
		cmd->channels[i].channel = channel;
		cmd->num_channels++;
	}

	if (cmd->num_channels == 0U) {
		ret = -EINVAL;
		goto out;
	}

	ARRAY_FOR_EACH_PTR(data->bss, bss) {
		bss->valid = false;
	}

	ret = wl1251_cmd_send(dev, WL1251_CMD_SCAN, cmd, sizeof(*cmd));
	if (ret < 0) {
		goto out;
	}

	data->scanning = true;
	data->scan_cb = cb;
	data->scan_from_supp = cb == NULL;

out:
	k_mutex_unlock(&data->lock);

	return ret;
}

static int wl1251_scan(const struct device *dev, struct net_if *iface,
		       struct wifi_scan_params *params, scan_result_cb_t cb)
{
	struct wl1251_scan_req req = {0};
	uint8_t channels[WL1251_SCAN_MAX_CHANNELS];

	ARG_UNUSED(iface);

	if (cb == NULL) {
		return -EINVAL;
	}

	if (params != NULL) {
		req.passive = params->scan_type == WIFI_SCAN_TYPE_PASSIVE;

		if (params->ssids[0] != NULL) {
			req.ssid = params->ssids[0];
			req.ssid_len = strlen(params->ssids[0]);
		}

		req.channels = channels;
		ARRAY_FOR_EACH_PTR(params->band_chan, chan) {
			if (chan->band == WIFI_FREQ_BAND_2_4_GHZ && chan->channel != 0U &&
			    req.num_channels < ARRAY_SIZE(channels)) {
				channels[req.num_channels++] = chan->channel;
			}
		}
	}

	return wl1251_scan_start(dev, &req, cb);
}

static int wl1251_iface_status(const struct device *dev, struct net_if *iface,
			       struct wifi_iface_status *status)
{
	struct wl1251_data *data = dev->data;

	ARG_UNUSED(iface);

	memset(status, 0, sizeof(*status));

	k_mutex_lock(&data->lock, K_FOREVER);

	status->iface_mode = WIFI_MODE_INFRA;
	status->band = WIFI_FREQ_BAND_2_4_GHZ;
	status->link_mode = WIFI_3;

	if (!data->running) {
		status->state = WIFI_STATE_INTERFACE_DISABLED;
	} else if (data->state == WL1251_STATE_ASSOCIATED) {
		status->state = data->authorized ? WIFI_STATE_COMPLETED : WIFI_STATE_4WAY_HANDSHAKE;
	} else if (data->state == WL1251_STATE_AUTHENTICATING) {
		status->state = WIFI_STATE_AUTHENTICATING;
	} else if (data->state != WL1251_STATE_IDLE) {
		status->state = WIFI_STATE_ASSOCIATING;
	} else if (data->scanning) {
		status->state = WIFI_STATE_SCANNING;
	} else {
		status->state = WIFI_STATE_DISCONNECTED;
	}

	if (data->state != WL1251_STATE_IDLE) {
		memcpy(status->bssid, data->bss_params.bssid, sizeof(data->bss_params.bssid));
		status->channel = data->bss_params.channel;
		status->beacon_interval = data->bss_params.beacon_interval;
		status->rssi = data->rssi;
		status->current_phy_tx_rate = wl1251_tx_rate_kbps(dev) / 1000.0f;
	}

	k_mutex_unlock(&data->lock);

	return 0;
}

static int wl1251_start(const struct device *dev, struct net_if *iface)
{
	const struct wl1251_config *config = dev->config;
	struct wl1251_data *data = dev->data;
	int64_t start = k_uptime_get();
	int ret;

	ARG_UNUSED(iface);

	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->running) {
		ret = 0;
		goto out;
	}

	data->intr_mask = WL1251_INTR_RX0_DATA | WL1251_INTR_RX1_DATA | WL1251_INTR_TX_RESULT |
			  WL1251_INTR_EVENT_A | WL1251_INTR_EVENT_B | WL1251_INTR_INIT_COMPLETE;
	data->rx_filter = RX_FILTER_DEFAULT;
	data->rx_counter = 0;
	data->rx_handled = 0;
	data->rx_buffer = 0;
	data->tx_in_count = 0;
	data->tx_next_result = 0;
	data->tx_busy = 0;
	data->bss_params.channel = SCAN_FIRST_CHANNEL;
	data->state = WL1251_STATE_IDLE;
	data->joined = false;
	data->authorized = false;
	data->pairwise_key = false;
	data->group_keys = 0;
	data->scanning = false;
	data->tx_seq = 0;

	ret = wl1251_boot(dev);
	if (ret < 0) {
		LOG_ERR("Could not start the chip (%d)", ret);
		wl1251_power_off(dev);
		goto out;
	}

	LOG_INF("Chip ID 0x%08x, firmware %s, up in %lld ms", data->chip_id, data->fw_version,
		k_uptime_delta(&start));

	data->running = true;

	ret = gpio_pin_interrupt_configure_dt(&config->irq, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		data->running = false;
		wl1251_power_off(dev);
	}

out:
	k_mutex_unlock(&data->lock);

	return ret;
}

static int wl1251_stop(const struct device *dev, struct net_if *iface)
{
	const struct wl1251_config *config = dev->config;
	struct wl1251_data *data = dev->data;

	ARG_UNUSED(iface);

	(void)gpio_pin_interrupt_configure_dt(&config->irq, GPIO_INT_DISABLE);

	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->running) {
		data->running = false;
		wl1251_power_off(dev);

		disconnected(dev);
		if (data->scanning) {
			scan_report(dev, true);
		}
	}

	k_mutex_unlock(&data->lock);

	return 0;
}

/*
 * Starts a data frame to the access point in the transmit buffer: the 802.11
 * header, the CCMP header if it is to be encrypted, and the LLC/SNAP header
 * that carries the Ethernet type. Returns where the payload goes.
 */
static uint8_t *data_frame(const struct device *dev, const uint8_t *dst, const uint8_t *src,
			   uint16_t type, bool encrypt)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_80211_hdr *hdr = (void *)&data->tx_frame[sizeof(struct wl1251_tx_desc)];
	uint8_t *ptr = (uint8_t *)(hdr + 1);
	uint16_t fc = WL1251_FC_DATA | WL1251_FC_TO_DS;

	if (encrypt) {
		fc |= WL1251_FC_PROTECTED;
	}

	hdr->frame_control = sys_cpu_to_le16(fc);
	hdr->duration = 0;
	memcpy(hdr->addr1, data->bss_params.bssid, sizeof(hdr->addr1));
	memcpy(hdr->addr2, src, sizeof(hdr->addr2));
	memcpy(hdr->addr3, dst, sizeof(hdr->addr3));
	hdr->seq_ctrl = sys_cpu_to_le16(data->tx_seq++ << 4);

	if (encrypt) {
		/*
		 * The packet number, which never repeats for a key; the chip
		 * encrypts and adds the MIC.
		 */
		uint64_t pn = ++data->tx_pn;

		*ptr++ = pn;
		*ptr++ = pn >> 8;
		*ptr++ = 0;
		*ptr++ = BIT(5) | (data->pairwise_key_idx << 6);
		*ptr++ = pn >> 16;
		*ptr++ = pn >> 24;
		*ptr++ = pn >> 32;
		*ptr++ = pn >> 40;
	}

	if (type >= ETH_TYPE_MIN) {
		memcpy(ptr, snap_rfc1042, sizeof(snap_rfc1042));
		ptr += sizeof(snap_rfc1042);
		sys_put_be16(type, ptr);
		ptr += 2;
	}

	return ptr;
}

/* Sends it, at the rate of the rate adaptation or at the lowest one */
static int data_send(const struct device *dev, const uint8_t *end, uint8_t queue, bool adapt)
{
	struct wl1251_data *data = dev->data;
	size_t len = end - &data->tx_frame[sizeof(struct wl1251_tx_desc)];

	return tx_frame_wait(dev, len, queue, adapt ? rates[data->tx_rate].rate : WL1251_RATE_1M,
			     adapt);
}

/*
 * A frame of the key handshake. It is in the chip when this returns, so
 * that a key set right after it does not get to encrypt it.
 */
int wl1251_send_eapol(const struct device *dev, const uint8_t *dst, uint16_t type,
		      const uint8_t *buf, size_t len, bool encrypt)
{
	struct wl1251_data *data = dev->data;
	uint8_t *ptr;
	int ret;

	if (len > NET_ETH_MTU) {
		return -EMSGSIZE;
	}

	k_mutex_lock(&data->tx_lock, K_FOREVER);
	k_mutex_lock(&data->lock, K_FOREVER);

	if (!data->running || data->state != WL1251_STATE_ASSOCIATED) {
		ret = -ENOTCONN;
		goto out;
	}

	ptr = data_frame(dev, dst, data->mac, type, encrypt && data->pairwise_key);
	memcpy(ptr, buf, len);

	/* Like management frames, slowly but surely */
	ret = data_send(dev, ptr + len, WL1251_QUEUE_VO, false);

out:
	k_mutex_unlock(&data->lock);
	k_mutex_unlock(&data->tx_lock);

	return ret;
}

/* An Ethernet frame from the network stack, out as an 802.11 data frame */
static int wl1251_send(const struct device *dev, struct net_pkt *pkt)
{
	struct wl1251_data *data = dev->data;
	size_t len = net_pkt_get_len(pkt);
	struct net_eth_hdr eth;
	uint16_t type;
	uint8_t *ptr;
	int ret;

	if (len < sizeof(eth) || len > sizeof(eth) + NET_ETH_MTU) {
		return -EMSGSIZE;
	}

	k_mutex_lock(&data->tx_lock, K_FOREVER);
	k_mutex_lock(&data->lock, K_FOREVER);

	net_pkt_cursor_init(pkt);
	ret = net_pkt_read(pkt, &eth, sizeof(eth));
	if (ret < 0) {
		goto out;
	}
	len -= sizeof(eth);
	type = sys_be16_to_cpu(eth.type);

	/* Nothing but the key handshake passes before the keys are set */
	if (!data->running || data->state != WL1251_STATE_ASSOCIATED ||
	    (!data->authorized && type != NET_ETH_PTYPE_EAPOL)) {
		ret = -ENETDOWN;
		goto out;
	}

	ptr = data_frame(dev, eth.dst.addr, eth.src.addr, type, data->pairwise_key);
	ret = net_pkt_read(pkt, ptr, len);
	if (ret < 0) {
		goto out;
	}

	ret = data_send(dev, ptr + len, WL1251_QUEUE_BE, true);

out:
	k_mutex_unlock(&data->lock);
	k_mutex_unlock(&data->tx_lock);

	return ret;
}

/*
 * The chip has no address of its own and the NVS file only a placeholder, so
 * unless the devicetree gives one, make one up: from the device ID where
 * there is one, so that it stays the same, and marked as locally
 * administered.
 */
static void wl1251_default_mac(uint8_t *mac)
{
	uint8_t id[16];
	ssize_t len = -ENOTSUP;

	if (IS_ENABLED(CONFIG_HWINFO)) {
		len = hwinfo_get_device_id(id, sizeof(id));
	}

	if (len > 0) {
		memset(mac, 0, NET_ETH_ADDR_LEN);
		for (ssize_t i = 0; i < len; i++) {
			mac[i % NET_ETH_ADDR_LEN] ^= id[i];
		}
	} else {
		sys_rand_get(mac, NET_ETH_ADDR_LEN);
	}

	mac[0] = (mac[0] & ~BIT(0)) | BIT(1);
}

static void wl1251_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct wl1251_data *data = dev->data;
	struct ethernet_context *eth_ctx = net_if_l2_data(iface);

	data->iface = iface;
	eth_ctx->eth_if_type = L2_ETH_IF_TYPE_WIFI;

	net_if_set_link_addr(iface, data->mac, sizeof(data->mac), NET_LINK_ETHERNET);
	ethernet_init(iface);

	/* No link before a network is joined, and no power before it is asked for */
	net_if_carrier_off(iface);
	net_if_flag_set(iface, NET_IF_NO_AUTO_START);
}

static int wl1251_init(const struct device *dev)
{
	const struct wl1251_config *config = dev->config;
	struct wl1251_data *data = dev->data;
	int ret;

	data->dev = dev;
	k_mutex_init(&data->lock);
	k_mutex_init(&data->tx_lock);
	k_sem_init(&data->work_sem, 0, 1);
	k_sem_init(&data->tx_sem, 0, 1);
	k_sem_init(&data->join_sem, 0, 1);

	if (!spi_is_ready_dt(&config->bus) || !gpio_is_ready_dt(&config->power) ||
	    !gpio_is_ready_dt(&config->irq)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&config->power, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	ret = gpio_pin_configure_dt(&config->irq, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	gpio_init_callback(&data->irq_cb, wl1251_irq, BIT(config->irq.pin));
	ret = gpio_add_callback(config->irq.port, &data->irq_cb);
	if (ret < 0) {
		return ret;
	}

	if (net_eth_mac_load(&config->mac, data->mac) < 0) {
		wl1251_default_mac(data->mac);
	}

	k_thread_create(&data->thread, config->stack, CONFIG_WIFI_WL1251_STACK_SIZE, wl1251_thread,
			(void *)dev, NULL, NULL, K_PRIO_COOP(CONFIG_WIFI_WL1251_THREAD_PRIO), 0,
			K_NO_WAIT);
	k_thread_name_set(&data->thread, "wl1251");

	return 0;
}

static const struct wifi_mgmt_ops wl1251_mgmt_ops = {
	.scan = wl1251_scan,
	.iface_status = wl1251_iface_status,
};

static const struct net_wifi_mgmt_offload wl1251_api = {
	.wifi_iface.iface_api.init = wl1251_iface_init,
	.wifi_iface.start = wl1251_start,
	.wifi_iface.stop = wl1251_stop,
	.wifi_iface.send = wl1251_send,
	.wifi_mgmt_api = &wl1251_mgmt_ops,
#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT
	.wifi_drv_ops = &wl1251_supp_ops,
#endif
};

#define WL1251_INIT(n)                                                                             \
	static K_KERNEL_STACK_DEFINE(wl1251_stack_##n, CONFIG_WIFI_WL1251_STACK_SIZE);             \
                                                                                                   \
	static const struct wl1251_config wl1251_config_##n = {                                    \
		.bus = SPI_DT_SPEC_INST_GET(n, SPI_WORD_SET(32) | SPI_TRANSFER_MSB),               \
		.power = GPIO_DT_SPEC_INST_GET(n, power_gpios),                                    \
		.irq = GPIO_DT_SPEC_INST_GET(n, irq_gpios),                                        \
		.mac = NET_ETH_MAC_DT_INST_CONFIG_INIT(n),                                         \
		.stack = wl1251_stack_##n,                                                         \
	};                                                                                         \
                                                                                                   \
	static struct wl1251_data wl1251_data_##n;                                                 \
                                                                                                   \
	NET_DEVICE_DT_INST_DEFINE(n, wl1251_init, NULL, &wl1251_data_##n, &wl1251_config_##n,      \
				  CONFIG_WIFI_INIT_PRIORITY, &wl1251_api, ETHERNET_L2,             \
				  NET_L2_GET_CTX_TYPE(ETHERNET_L2), NET_ETH_MTU);

DT_INST_FOREACH_STATUS_OKAY(WL1251_INIT)
