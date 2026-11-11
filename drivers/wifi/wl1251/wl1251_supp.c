/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The WL1251 under wpa_supplicant. The supplicant picks the network and
 * runs the connection (its SME): it asks for scans, for authentication and
 * association with an access point, and for the keys of its handshake to be
 * set, and it hears of the access point's answers.
 */

#include <zephyr/logging/log.h>
#include <zephyr/net/wifi_utils.h>
#include <zephyr/sys/byteorder.h>

#include "wl1251.h"

#include <drivers/driver_zephyr.h>

LOG_MODULE_DECLARE(wifi_wl1251, CONFIG_WIFI_LOG_LEVEL);

struct wl1251_supp {
	struct zep_drv_if_ctx *if_ctx;
	struct zep_wpa_supp_dev_callbk_fns cb;
};

/* In units of 100 kbit/s */
static const uint16_t wl1251_bitrates[] = {10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540};

#define FIRST_CHANNEL 1
#define LAST_CHANNEL  13
/* In dBm */
#define MAX_TX_POWER  20

static void *wl1251_supp_init(void *supp_drv_if_ctx, const char *iface_name,
			      struct zep_wpa_supp_dev_callbk_fns *callbk_fns)
{
	struct zep_drv_if_ctx *if_ctx = supp_drv_if_ctx;
	const struct device *dev = net_if_get_device(if_ctx->iface);
	struct wl1251_data *data = dev->data;
	struct wl1251_supp *supp;

	ARG_UNUSED(iface_name);

	supp = os_zalloc(sizeof(*supp));
	if (supp == NULL) {
		return NULL;
	}

	supp->if_ctx = if_ctx;
	supp->cb = *callbk_fns;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->supp = supp;
	k_mutex_unlock(&data->lock);

	return (void *)dev;
}

static void wl1251_supp_deinit(void *if_priv)
{
	const struct device *dev = if_priv;
	struct wl1251_data *data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	os_free(data->supp);
	data->supp = NULL;
	k_mutex_unlock(&data->lock);
}

static int wl1251_supp_scan2(void *if_priv, struct wpa_driver_scan_params *params)
{
	const struct device *dev = if_priv;
	uint8_t channels[WL1251_SCAN_MAX_CHANNELS];
	struct wl1251_scan_req req = {
		/* Without a network to probe for the supplicant means to listen */
		.passive = params->num_ssids == 0U,
		.channels = channels,
	};

	if (params->num_ssids > 0U) {
		req.ssid = params->ssids[0].ssid;
		req.ssid_len = params->ssids[0].ssid_len;
	}

	for (int i = 0; params->freqs != NULL && params->freqs[i] != 0; i++) {
		uint8_t channel = wifi_utils_freq_to_chan(params->freqs[i]);

		if (channel >= FIRST_CHANNEL && channel <= LAST_CHANNEL &&
		    req.num_channels < ARRAY_SIZE(channels)) {
			channels[req.num_channels++] = channel;
		}
	}

	if (params->freqs != NULL && req.num_channels == 0U) {
		return -EINVAL;
	}

	return wl1251_scan_start(dev, &req, NULL);
}

void wl1251_supp_scan_done(const struct device *dev, bool aborted)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_supp *supp = data->supp;
	union wpa_event_data event = {0};

	if (supp == NULL) {
		return;
	}

	event.scan_info.aborted = aborted;
	supp->cb.scan_done(supp->if_ctx, &event);
}

static int wl1251_supp_get_scan_results2(void *if_priv)
{
	const struct device *dev = if_priv;
	struct wl1251_data *data = dev->data;
	struct wl1251_supp *supp;
	struct wpa_scan_res *res;
	size_t left = 0;
	int ret = 0;

	res = os_zalloc(sizeof(*res) + sizeof(data->bss[0].ies));
	if (res == NULL) {
		return -ENOMEM;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	supp = data->supp;
	if (supp == NULL) {
		ret = -ENODEV;
		goto out;
	}

	ARRAY_FOR_EACH_PTR(data->bss, bss) {
		if (bss->valid) {
			left++;
		}
	}

	if (left == 0U) {
		/* What the callback does with the last result */
		supp->if_ctx->scan_res2_get_in_prog = false;
		k_sem_give(&supp->if_ctx->drv_resp_sem);
		goto out;
	}

	ARRAY_FOR_EACH_PTR(data->bss, bss) {
		if (!bss->valid) {
			continue;
		}

		memset(res, 0, sizeof(*res));
		res->flags = WPA_SCAN_LEVEL_DBM | WPA_SCAN_QUAL_INVALID | WPA_SCAN_NOISE_INVALID;
		memcpy(res->bssid, bss->bssid, sizeof(bss->bssid));
		res->freq = wifi_utils_chan_to_freq(WIFI_FREQ_BAND_2_4_GHZ, bss->channel);
		res->max_cw = CHAN_WIDTH_20_NOHT;
		res->beacon_int = bss->beacon_interval;
		res->caps = bss->capability;
		res->level = bss->rssi;
		res->ie_len = bss->ies_len;
		memcpy(res + 1, bss->ies, bss->ies_len);

		/* Makes its own copy */
		supp->cb.scan_res(supp->if_ctx, res, --left > 0U);
	}

out:
	k_mutex_unlock(&data->lock);
	os_free(res);

	return ret;
}

static int wl1251_supp_authenticate(void *if_priv, struct wpa_driver_auth_params *params,
				    struct wpa_bss *curr_bss)
{
	const struct device *dev = if_priv;
	struct wl1251_bss_params bss = {0};

	/* Open system, as WPA2 uses it; no shared key, SAE or fast transition */
	if (params->bssid == NULL || params->auth_alg != WPA_AUTH_ALG_OPEN) {
		return -ENOTSUP;
	}

	memcpy(bss.bssid, params->bssid, sizeof(bss.bssid));
	bss.channel = wifi_utils_freq_to_chan(params->freq);
	bss.beacon_interval = curr_bss->beacon_int;
	bss.capability = curr_bss->caps;
	bss.rates = wl1251_ie_rates(wpa_bss_get_ie(curr_bss, WLAN_EID_SUPP_RATES)) |
		    wl1251_ie_rates(wpa_bss_get_ie(curr_bss, WLAN_EID_EXT_SUPP_RATES));
	if (bss.rates == 0U) {
		bss.rates = WL1251_RATE_1M | WL1251_RATE_2M;
	}

	if (bss.channel < FIRST_CHANNEL || bss.channel > LAST_CHANNEL) {
		return -EINVAL;
	}

	return wl1251_authenticate(dev, &bss, WLAN_AUTH_OPEN);
}

static int wl1251_supp_associate(void *if_priv, struct wpa_driver_associate_params *params)
{
	const struct device *dev = if_priv;

	/* The supplicant's elements, with the RSN one, go out as they are */
	return wl1251_associate(dev, params->ssid, params->ssid_len, params->wpa_ie,
				params->wpa_ie_len);
}

static int wl1251_supp_deauthenticate(void *if_priv, const char *addr, unsigned short reason_code)
{
	const struct device *dev = if_priv;

	ARG_UNUSED(addr);

	return wl1251_deauthenticate(dev, reason_code);
}

/* Passes what the access point said on as the event the supplicant expects */
void wl1251_supp_mgmt_rx(const struct device *dev, const uint8_t *frame, size_t len)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_supp *supp = data->supp;
	const struct wl1251_80211_hdr *hdr = (const void *)frame;
	const uint8_t *body = (const uint8_t *)(hdr + 1);
	size_t body_len = len - sizeof(*hdr);
	union wpa_event_data event = {0};
	uint16_t status;

	if (supp == NULL) {
		return;
	}

	switch (sys_le16_to_cpu(hdr->frame_control) & WL1251_FC_STYPE_MASK) {
	case WL1251_FC_AUTH:
		/* Algorithm, transaction and status, then elements */
		memcpy(event.auth.peer, hdr->addr2, sizeof(event.auth.peer));
		event.auth.auth_type = sys_get_le16(&body[0]);
		event.auth.auth_transaction = sys_get_le16(&body[2]);
		event.auth.status_code = sys_get_le16(&body[4]);
		if (body_len > 6U) {
			event.auth.ies = &body[6];
			event.auth.ies_len = body_len - 6U;
		}
		supp->cb.auth_resp(supp->if_ctx, &event);
		break;
	case WL1251_FC_ASSOC_RESP:
	case WL1251_FC_REASSOC_RESP:
		/* Capability, status and association ID, then elements */
		status = sys_get_le16(&body[2]);
		if (status == WLAN_STATUS_SUCCESS) {
			event.assoc_info.addr = hdr->addr3;
			event.assoc_info.req_ies = data->assoc_ies;
			event.assoc_info.req_ies_len = data->assoc_ies_len;
			event.assoc_info.resp_ies = &body[6];
			event.assoc_info.resp_ies_len = body_len - 6U;
			event.assoc_info.resp_frame = frame;
			event.assoc_info.resp_frame_len = len;
			event.assoc_info.freq = wifi_utils_chan_to_freq(WIFI_FREQ_BAND_2_4_GHZ,
									data->bss_params.channel);
		} else {
			event.assoc_reject.bssid = hdr->addr3;
			event.assoc_reject.resp_ies = &body[6];
			event.assoc_reject.resp_ies_len = body_len - 6U;
			event.assoc_reject.status_code = status;
		}
		supp->cb.assoc_resp(supp->if_ctx, &event, status);
		break;
	case WL1251_FC_DEAUTH:
		event.deauth_info.addr = hdr->addr2;
		event.deauth_info.reason_code = sys_get_le16(body);
		supp->cb.deauth(supp->if_ctx, &event, (const struct ieee80211_mgmt *)frame);
		break;
	case WL1251_FC_DISASSOC:
		event.disassoc_info.addr = hdr->addr2;
		event.disassoc_info.reason_code = sys_get_le16(body);
		supp->cb.disassoc(supp->if_ctx, &event);
		break;
	default:
		break;
	}
}

/* The network went away without a word: as if this station had left it */
void wl1251_supp_link_lost(const struct device *dev)
{
	struct wl1251_data *data = dev->data;
	struct wl1251_supp *supp = data->supp;
	union wpa_event_data event = {0};
	union {
		struct wl1251_80211_hdr hdr;
		struct ieee80211_mgmt mgmt;
	} frame = {0};

	if (supp == NULL) {
		return;
	}

	memcpy(frame.hdr.addr3, data->bss_params.bssid, sizeof(frame.hdr.addr3));

	event.deauth_info.addr = data->bss_params.bssid;
	event.deauth_info.reason_code = WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY;
	event.deauth_info.locally_generated = 1;
	supp->cb.deauth(supp->if_ctx, &event, &frame.mgmt);
}

static int wl1251_supp_set_key(void *if_priv, const unsigned char *ifname, enum wpa_alg alg,
			       const unsigned char *addr, int key_idx, int set_tx,
			       const unsigned char *seq, size_t seq_len, const unsigned char *key,
			       size_t key_len, enum key_flag key_flag)
{
	const struct device *dev = if_priv;
	bool pairwise = (key_flag & KEY_FLAG_PAIRWISE) != 0;

	ARG_UNUSED(ifname);
	ARG_UNUSED(addr);
	ARG_UNUSED(set_tx);
	/* The chip keeps the receive counters, starting from zero */
	ARG_UNUSED(seq);
	ARG_UNUSED(seq_len);

	if (key_idx < 0) {
		return -EINVAL;
	}

	if (alg == WPA_ALG_NONE) {
		/* The supplicant clears more key slots than there are */
		(void)wl1251_remove_key(dev, pairwise, key_idx);
		return 0;
	}

	if (alg != WPA_ALG_CCMP) {
		return -ENOTSUP;
	}

	return wl1251_set_key(dev, pairwise, key_idx, key, key_len);
}

static int wl1251_supp_set_supp_port(void *if_priv, int authorized, char *bssid)
{
	const struct device *dev = if_priv;

	ARG_UNUSED(bssid);

	wl1251_set_authorized(dev, authorized != 0);

	return 0;
}

/*
 * The frames of the key handshake come this way rather than through the
 * network stack, whose queue would let a key be set before the frame that
 * was sent ahead of it is out.
 */
static int wl1251_supp_tx_control_port(void *if_priv, const unsigned char *dest,
				       unsigned short proto, const unsigned char *buf, size_t len,
				       int no_encrypt)
{
	const struct device *dev = if_priv;

	return wl1251_send_eapol(dev, dest, proto, buf, len, no_encrypt == 0);
}

static int wl1251_supp_signal_poll(void *if_priv, struct wpa_signal_info *si, unsigned char *bssid)
{
	const struct device *dev = if_priv;
	struct wl1251_data *data = dev->data;

	ARG_UNUSED(bssid);

	memset(si, 0, sizeof(*si));

	k_mutex_lock(&data->lock, K_FOREVER);
	si->frequency = wifi_utils_chan_to_freq(WIFI_FREQ_BAND_2_4_GHZ, data->bss_params.channel);
	si->data.signal = data->rssi;
	si->data.current_tx_rate = wl1251_tx_rate_kbps(dev);
	si->current_noise = WPA_INVALID_NOISE;
	si->chanwidth = CHAN_WIDTH_20_NOHT;
	k_mutex_unlock(&data->lock);

	return 0;
}

static int wl1251_supp_get_conn_info(void *if_priv, struct wpa_conn_info *info)
{
	const struct device *dev = if_priv;
	struct wl1251_data *data = dev->data;

	memset(info, 0, sizeof(*info));

	k_mutex_lock(&data->lock, K_FOREVER);
	info->beacon_interval = data->bss_params.beacon_interval;
	/* The chip is awake at every beacon */
	info->dtim_period = 1;
	k_mutex_unlock(&data->lock);

	return 0;
}

static int wl1251_supp_get_capa(void *if_priv, struct wpa_driver_capa *capa)
{
	ARG_UNUSED(if_priv);

	memset(capa, 0, sizeof(*capa));

	/* WPA2 with CCMP, the supplicant running the connection */
	capa->key_mgmt = WPA_DRIVER_CAPA_KEY_MGMT_WPA2 | WPA_DRIVER_CAPA_KEY_MGMT_WPA2_PSK;
	capa->enc = WPA_DRIVER_CAPA_ENC_CCMP;
	capa->auth = WPA_DRIVER_AUTH_OPEN;
	capa->flags = WPA_DRIVER_FLAGS_SME | WPA_DRIVER_FLAGS_CONTROL_PORT;
	capa->max_scan_ssids = 1;

	return 0;
}

static int wl1251_supp_get_wiphy(void *if_priv)
{
	const struct device *dev = if_priv;
	struct wl1251_data *data = dev->data;
	struct wl1251_supp *supp;
	struct wpa_supp_event_supported_band *band;
	int ret = 0;

	band = os_zalloc(sizeof(*band));
	if (band == NULL) {
		return -ENOMEM;
	}

	band->band = WIFI_FREQ_BAND_2_4_GHZ;

	for (uint8_t channel = FIRST_CHANNEL; channel <= LAST_CHANNEL; channel++) {
		struct wpa_supp_event_channel *chan = &band->channels[band->wpa_supp_n_channels++];

		chan->ch_valid = 1;
		chan->center_frequency = wifi_utils_chan_to_freq(WIFI_FREQ_BAND_2_4_GHZ, channel);
		chan->wpa_supp_max_power = MAX_TX_POWER;
	}

	ARRAY_FOR_EACH(wl1251_bitrates, i) {
		band->bitrates[band->wpa_supp_n_bitrates++].wpa_supp_bitrate = wl1251_bitrates[i];
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	supp = data->supp;
	if (supp != NULL) {
		supp->cb.get_wiphy_res(supp->if_ctx, band);
		/* No more bands */
		supp->cb.get_wiphy_res(supp->if_ctx, NULL);
	} else {
		ret = -ENODEV;
	}
	k_mutex_unlock(&data->lock);

	os_free(band);

	return ret;
}

static int wl1251_supp_register_frame(void *if_priv, u16 type, const u8 *match, size_t match_len,
				      bool multicast)
{
	ARG_UNUSED(if_priv);
	ARG_UNUSED(type);
	ARG_UNUSED(match);
	ARG_UNUSED(match_len);
	ARG_UNUSED(multicast);

	/* No management frames other than the connection's own reach the host */
	return 0;
}

const struct zep_wpa_supp_dev_ops wl1251_supp_ops = {
	.init = wl1251_supp_init,
	.deinit = wl1251_supp_deinit,
	.scan2 = wl1251_supp_scan2,
	.get_scan_results2 = wl1251_supp_get_scan_results2,
	.authenticate = wl1251_supp_authenticate,
	.associate = wl1251_supp_associate,
	.deauthenticate = wl1251_supp_deauthenticate,
	.set_key = wl1251_supp_set_key,
	.set_supp_port = wl1251_supp_set_supp_port,
	.tx_control_port = wl1251_supp_tx_control_port,
	.signal_poll = wl1251_supp_signal_poll,
	.get_conn_info = wl1251_supp_get_conn_info,
	.get_capa = wl1251_supp_get_capa,
	.get_wiphy = wl1251_supp_get_wiphy,
	.register_frame = wl1251_supp_register_frame,
};
