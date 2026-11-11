/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_WIFI_WL1251_WL1251_H_
#define ZEPHYR_DRIVERS_WIFI_WL1251_WL1251_H_

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/util.h>

/*
 * The interface of the chip and its firmware: registers, commands and the
 * layouts of what goes back and forth. TI has not published it; the numbers
 * are those of Linux's wl1251 driver.
 */

/*
 * Chip address space. The host sees two windows of it through a 17-bit SPI
 * address: a memory window at 0 and a register window right after it.
 */
#define WL1251_REG_BASE       0x00300000U
#define WL1251_REG_SIZE_BOOT  0x00008800U
#define WL1251_REG_SIZE_RUN   0x0000b000U
#define WL1251_MEM_START_BOOT 0x00000000U
#define WL1251_MEM_SIZE_BOOT  0x00016800U
#define WL1251_MEM_START_RUN  0x00028000U
#define WL1251_MEM_SIZE_RUN   0x00014000U

/* Outside of both windows, at fixed SPI addresses */
#define WL1251_SPI_PARTITION 0x1ffc0U
#define WL1251_SPI_ELP_CTRL  0x1fffcU

#define WL1251_ELP_WAKE_UP    BIT(0)
#define WL1251_ELP_WLAN_READY BIT(1)

#define WL1251_REG(offset) (WL1251_REG_BASE + (offset))

#define WL1251_REG_SOFT_RESET    WL1251_REG(0x0000)
#define WL1251_REG_INTR_TRIG     WL1251_REG(0x0474)
#define WL1251_REG_INTR_TRIG_H   WL1251_REG(0x0478)
#define WL1251_REG_INTR_MASK     WL1251_REG(0x0494)
#define WL1251_REG_INTR_CLEAR    WL1251_REG(0x04a4)
#define WL1251_REG_INTR_ACK      WL1251_REG(0x04a8)
#define WL1251_REG_INTR_NO_CLEAR WL1251_REG(0x04b0)
#define WL1251_REG_ECPU_CTRL     WL1251_REG(0x0804)
#define WL1251_REG_HI_CFG        WL1251_REG(0x0808)
#define WL1251_REG_SPARE_A2      WL1251_REG(0x0998)
#define WL1251_REG_ENABLE        WL1251_REG(0x5450)
#define WL1251_REG_SCR_PAD0      WL1251_REG(0x5608)
#define WL1251_REG_SCR_PAD1      WL1251_REG(0x560c)
#define WL1251_REG_SCR_PAD2      WL1251_REG(0x5610)
#define WL1251_REG_SCR_PAD3      WL1251_REG(0x5614)
#define WL1251_REG_SCR_PAD4      WL1251_REG(0x5618)
#define WL1251_REG_SCR_PAD6      WL1251_REG(0x5630)
#define WL1251_REG_CHIP_ID       WL1251_REG(0x5674)
#define WL1251_REG_ELP_CFG_MODE  WL1251_REG(0x5804)
#define WL1251_REG_ELP_CMD       WL1251_REG(0x5808)
#define WL1251_REG_PLL_CAL_TIME  WL1251_REG(0x5810)
#define WL1251_REG_CLK_REQ_TIME  WL1251_REG(0x5814)
#define WL1251_REG_CLK_BUF_TIME  WL1251_REG(0x5818)
#define WL1251_REG_PLL_SYNC_CNT  WL1251_REG(0x5820)
/* Baseband PLL and RF front end */
#define WL1251_REG_BB_PLL_DIV    WL1251_REG(0x5840)
#define WL1251_REG_BB_PLL_FRAC   WL1251_REG(0x5844)
#define WL1251_REG_BB_PLL_SIGMA  WL1251_REG(0x5848)
#define WL1251_REG_BB_PLL_LOOP   WL1251_REG(0x5854)
#define WL1251_REG_BB_PLL_STOP   WL1251_REG(0x5858)
#define WL1251_REG_RF_AFE_PLL    WL1251_REG(0x58cc)
#define WL1251_REG_RF_AFE_5      WL1251_REG(0x58d4)
#define WL1251_REG_RF_AFE_REFDIV WL1251_REG(0x58f0)
#define WL1251_REG_RF_AFE_13     WL1251_REG(0x58f4)
#define WL1251_REG_RF_AFE_FILTER WL1251_REG(0x58f8)
#define WL1251_REG_RF_AFE_CTRL_2 WL1251_REG(0x5948)

/* The firmware leaves its mailbox addresses in the scratch pads */
#define WL1251_REG_CMD_MBOX_PTR   WL1251_REG_SCR_PAD0
#define WL1251_REG_EVENT_MBOX_PTR WL1251_REG_SCR_PAD1
/* Where the firmware finds the NVS tables: the length of the image */
#define WL1251_REG_NVS_ADDR       WL1251_REG_SCR_PAD4

#define WL1251_SOFT_RESET     BIT(0)
#define WL1251_ECPU_HALT      0x00000101U
/* UART, RS232, clock request and host interrupt enable */
#define WL1251_HI_CFG_DEFAULT 0x0000003cU

#define WL1251_CHIP_ID_PG11 0x07020101U
#define WL1251_CHIP_ID_PG12 0x07030101U

/* Interrupts from the firmware to the host */
#define WL1251_INTR_RX0_DATA      BIT(0)
#define WL1251_INTR_TX_RESULT     BIT(1)
#define WL1251_INTR_RX1_DATA      BIT(3)
#define WL1251_INTR_EVENT_A       BIT(4)
#define WL1251_INTR_EVENT_B       BIT(5)
#define WL1251_INTR_CMD_COMPLETE  BIT(9)
#define WL1251_INTR_INIT_COMPLETE BIT(14)

/* Triggers from the host to the firmware */
#define WL1251_TRIG_CMD       BIT(0)
#define WL1251_TRIG_EVENT_ACK BIT(1)
#define WL1251_TRIG_TX_PROC0  BIT(2)
#define WL1251_TRIG_RX_PROC0  BIT(3)
/* In the high trigger register */
#define WL1251_TRIG_RX_PROC1  BIT(17)
#define WL1251_TRIG_TX_PROC1  BIT(18)

/* Commands */
#define WL1251_CMD_INTERROGATE     1
#define WL1251_CMD_CONFIGURE       2
#define WL1251_CMD_ENABLE_RX       3
#define WL1251_CMD_ENABLE_TX       4
#define WL1251_CMD_SCAN            8
#define WL1251_CMD_STOP_SCAN       9
#define WL1251_CMD_VBM             10
#define WL1251_CMD_START_JOIN      11
#define WL1251_CMD_SET_KEYS        12
#define WL1251_CMD_BEACON          19
#define WL1251_CMD_PROBE_RESP      20
#define WL1251_CMD_NULL_DATA       21
#define WL1251_CMD_PROBE_REQ       22
#define WL1251_CMD_PS_POLL         30
#define WL1251_CMD_QOS_NULL_DATA   31
#define WL1251_CMD_DISCONNECT      36
#define WL1251_CMD_SET_PS_MODE     37
#define WL1251_CMD_TRIGGER_SCAN_TO 47

#define WL1251_CMD_STATUS_SUCCESS 1

/* Information elements, read with INTERROGATE and written with CONFIGURE */
#define WL1251_IE_WAKE_UP_CONDITIONS 0x0002
#define WL1251_IE_MEM_CFG            0x0003
#define WL1251_IE_SLOT               0x0004
#define WL1251_IE_AC_CFG             0x0007
#define WL1251_IE_MEM_MAP            0x0008
#define WL1251_IE_AID                0x000a
#define WL1251_IE_FW_REV             0x000d
#define WL1251_IE_RX_CFG             0x0010
#define WL1251_IE_TX_QUEUE_CFG       0x0011
#define WL1251_IE_FEATURE_CFG        0x0015
#define WL1251_IE_TID_CFG            0x001a
#define WL1251_IE_BEACON_FILTER_OPT  0x001f
#define WL1251_IE_PD_THRESHOLD       0x0023
#define WL1251_IE_DATA_PATH_PARAMS   0x0024
#define WL1251_IE_CCA_THRESHOLD      0x0025
#define WL1251_IE_EVENT_MBOX_MASK    0x0026
#define WL1251_IE_CONN_MONIT_PARAMS  0x002d
#define WL1251_IE_BCN_DTIM_OPTIONS   0x0031
#define WL1251_IE_SG_ENABLE          0x0032
#define WL1251_IE_SG_CFG             0x0033
#define WL1251_IE_BEACON_FILTER_TBL  0x0038
#define WL1251_IE_CTS_PROTECTION     0x003e
#define WL1251_IE_SLEEP_AUTH         0x003f
#define WL1251_IE_PREAMBLE_TYPE      0x0040
#define WL1251_IE_FW_GEN_FRAME_RATES 0x0042
#define WL1251_IE_SERVICE_PERIOD_TMO 0x0045
#define WL1251_IE_STATION_ID         0x1001
#define WL1251_IE_RX_MSDU_LIFE_TIME  0x1004
#define WL1251_IE_CUR_TX_PWR         0x100d
#define WL1251_IE_DEFAULT_KEY        0x1010
#define WL1251_IE_RTS_THRESHOLD      0x1013
#define WL1251_IE_GROUP_ADDRESS_TBL  0x1014

/* Events */
#define WL1251_EVENT_SCAN_COMPLETE BIT(3)
#define WL1251_EVENT_PS_REPORT     BIT(6)
#define WL1251_EVENT_SYNC_TIMEOUT  BIT(7)
#define WL1251_EVENT_JOIN_COMPLETE BIT(13)
#define WL1251_EVENT_BSS_LOSE      BIT(15)
#define WL1251_EVENT_REGAINED_BSS  BIT(22)

/* Receive configuration: which frames the firmware takes off the air */
#define WL1251_RX_CFG_ALL_GOOD      BIT(3)
#define WL1251_RX_CFG_UNICAST       BIT(4)
#define WL1251_RX_CFG_BSSID         BIT(5)
#define WL1251_RX_CFG_MULTICAST     BIT(6)
#define WL1251_RX_CFG_TSF           BIT(16)
/* Receive filter: which of those it passes to the host */
#define WL1251_RX_FILTER_PROBE_RESP BIT(2)
#define WL1251_RX_FILTER_MGMT       BIT(4)
#define WL1251_RX_FILTER_DATA       BIT(6)
#define WL1251_RX_FILTER_CTRL       BIT(7)
#define WL1251_RX_FILTER_BEACON     BIT(9)
#define WL1251_RX_FILTER_AUTH       BIT(10)
#define WL1251_RX_FILTER_ASSOC      BIT(11)

/* Rates as the firmware numbers them, in transmit descriptors and masks */
#define WL1251_RATE_1M   BIT(0)
#define WL1251_RATE_2M   BIT(1)
#define WL1251_RATE_5_5M BIT(2)
#define WL1251_RATE_6M   BIT(3)
#define WL1251_RATE_9M   BIT(4)
#define WL1251_RATE_11M  BIT(5)
#define WL1251_RATE_12M  BIT(6)
#define WL1251_RATE_18M  BIT(7)
#define WL1251_RATE_24M  BIT(9)
#define WL1251_RATE_36M  BIT(10)
#define WL1251_RATE_48M  BIT(11)
#define WL1251_RATE_54M  BIT(12)

#define WL1251_BSS_TYPE_STA 2

#define WL1251_SCAN_MAX_CHANNELS 16
#define WL1251_MAX_TEMPLATE_SIZE 300
#define WL1251_TX_RESULTS        16
#define WL1251_TX_RING_CHUNKS    2
#define WL1251_RING_CHUNK_SIZE   1600
#define WL1251_TX_BLOCK_SIZE     252
#define WL1251_RX_PLCP_LEN       8
/* Between the receive descriptor and the frame */
#define WL1251_RX_FRAME_OFFSET   20
#define WL1251_MAX_FRAME_LEN     2346

#define WL1251_KEY_ADD          1
#define WL1251_KEY_REMOVE       2
#define WL1251_KEY_AES_GROUP    4
#define WL1251_KEY_AES_PAIRWISE 5

/* CCMP */
#define WL1251_KEY_SIZE          16
/* Key indexes 0 to 3, and where the pairwise key is kept */
#define WL1251_PAIRWISE_KEY_SLOT 4

#define WL1251_QUEUE_BE 0
#define WL1251_QUEUE_VO 3

struct wl1251_cmd_hdr {
	uint16_t id;
	uint16_t status;
} __packed;

struct wl1251_ie_hdr {
	struct wl1251_cmd_hdr cmd;
	uint16_t id;
	/* Of what follows this header */
	uint16_t len;
} __packed;

struct wl1251_ie_fw_rev {
	struct wl1251_ie_hdr hdr;
	char fw_version[20];
	uint32_t hw_version;
} __packed;

struct wl1251_ie_u8 {
	struct wl1251_ie_hdr hdr;
	uint8_t value;
	uint8_t pad[3];
} __packed;

struct wl1251_ie_u16 {
	struct wl1251_ie_hdr hdr;
	uint16_t value;
	uint8_t pad[2];
} __packed;

struct wl1251_ie_u32 {
	struct wl1251_ie_hdr hdr;
	uint32_t value;
} __packed;

struct wl1251_ie_feature_cfg {
	struct wl1251_ie_hdr hdr;
	uint32_t options;
	uint32_t data_flow_options;
} __packed;

struct wl1251_ie_mem_cfg {
	struct wl1251_ie_hdr hdr;
	uint16_t num_stations;
	uint16_t reserved_1;
	uint8_t rx_mem_block_num;
	uint8_t reserved_2;
	uint8_t num_tx_queues;
	uint8_t host_if_options;
	uint8_t tx_min_mem_block_num;
	uint8_t num_ssid_profiles;
	uint16_t debug_buffer_size;
	struct {
		uint8_t num_descs;
		uint8_t pad;
		uint8_t type;
		uint8_t priority;
		uint32_t dma_address;
	} __packed rx_queue;
	struct {
		uint8_t num_descs;
		uint8_t pad[2];
		uint8_t attributes;
	} __packed tx_queue[5];
} __packed;

struct wl1251_ie_mem_map {
	struct wl1251_ie_hdr hdr;
	/* Start and end of the firmware's memory areas */
	uint32_t areas[16];
	uint32_t num_tx_mem_blocks;
	uint32_t num_rx_mem_blocks;
} __packed;

struct wl1251_ie_data_path {
	struct wl1251_ie_hdr hdr;
	uint16_t rx_chunk_size;
	uint16_t tx_chunk_size;
	uint8_t rx_chunk_num;
	uint8_t tx_chunk_num;
	uint8_t tx_complete_threshold;
	uint8_t tx_complete_ring_depth;
	uint32_t tx_complete_timeout;
} __packed;

struct wl1251_ie_data_path_resp {
	struct wl1251_ie_hdr hdr;
	uint16_t rx_chunk_size;
	uint16_t tx_chunk_size;
	uint8_t rx_chunk_num;
	uint8_t tx_chunk_num;
	uint8_t pad[2];
	uint32_t rx_ring_addr;
	uint32_t tx_ring_addr;
	uint32_t rx_control_addr;
	uint32_t tx_control_addr;
	uint32_t tx_complete_addr;
} __packed;

struct wl1251_ie_rx_cfg {
	struct wl1251_ie_hdr hdr;
	uint32_t config;
	uint32_t filter;
} __packed;

struct wl1251_ie_tx_queue_cfg {
	struct wl1251_ie_hdr hdr;
	uint8_t qid;
	uint8_t pad[3];
	uint16_t high_threshold;
	uint16_t low_threshold;
} __packed;

struct wl1251_ie_ac_cfg {
	struct wl1251_ie_hdr hdr;
	uint8_t ac;
	uint8_t cw_min;
	uint16_t cw_max;
	uint8_t aifsn;
	uint8_t reserved;
	uint16_t txop_limit;
} __packed;

struct wl1251_ie_tid_cfg {
	struct wl1251_ie_hdr hdr;
	uint8_t queue;
	uint8_t channel_type;
	uint8_t tsid;
	uint8_t ps_scheme;
	uint8_t ack_policy;
	uint8_t pad[3];
	uint32_t apsd_conf[2];
} __packed;

struct wl1251_ie_slot {
	struct wl1251_ie_hdr hdr;
	uint8_t wone_index;
	uint8_t slot_time;
	uint8_t reserved[6];
} __packed;

struct wl1251_ie_group_addr_tbl {
	struct wl1251_ie_hdr hdr;
	uint8_t enabled;
	uint8_t num_groups;
	uint8_t pad[2];
	uint8_t mac_table[8 * 6];
} __packed;

struct wl1251_ie_rx_timeout {
	struct wl1251_ie_hdr hdr;
	uint16_t ps_poll_timeout;
	uint16_t upsd_timeout;
} __packed;

struct wl1251_ie_beacon_filter_opt {
	struct wl1251_ie_hdr hdr;
	uint8_t enable;
	uint8_t max_num_beacons;
	uint8_t pad[2];
} __packed;

struct wl1251_ie_beacon_filter_tbl {
	struct wl1251_ie_hdr hdr;
	uint8_t num_ie;
	uint8_t pad[3];
	uint8_t table[32 * 2 + 6 * 6];
} __packed;

struct wl1251_ie_conn_monit {
	struct wl1251_ie_hdr hdr;
	uint32_t synch_fail_threshold;
	uint32_t bss_lose_timeout;
} __packed;

/* Bluetooth coexistence ("soft gemini") parameters */
struct wl1251_ie_sg_cfg {
	struct wl1251_ie_hdr hdr;
	uint32_t min_rate;
	uint16_t bt_hp_max_time;
	uint16_t wlan_hp_max_time;
	uint16_t sense_disable_timer;
	uint16_t rx_time_bt_hp;
	uint16_t tx_time_bt_hp;
	uint16_t rx_time_bt_hp_fast;
	uint16_t tx_time_bt_hp_fast;
	uint16_t wlan_cycle_fast;
	uint16_t bt_anti_starvation_period;
	uint16_t next_bt_lp_packet;
	uint16_t wake_up_beacon;
	uint16_t hp_dm_max_guard_time;
	uint16_t next_wlan_packet;
	uint8_t antenna_type;
	uint8_t signal_type;
	uint8_t afh_leverage_on;
	uint8_t quiet_cycle_num;
	uint8_t max_cts;
	uint8_t wlan_packets_num;
	uint8_t bt_packets_num;
	uint8_t missed_rx_avalanche;
	uint8_t wlan_elp_hp;
	uint8_t bt_anti_starvation_cycles;
	uint8_t ack_mode_dual_ant;
	uint8_t pa_sd_enable;
	uint8_t pta_auto_mode_enable;
	uint8_t bt_hp_respected_num;
} __packed;

struct wl1251_ie_cca_threshold {
	struct wl1251_ie_hdr hdr;
	uint16_t rx_cca_threshold;
	uint8_t tx_energy_detection;
	uint8_t pad;
} __packed;

struct wl1251_ie_bcn_dtim {
	struct wl1251_ie_hdr hdr;
	uint16_t beacon_rx_timeout;
	uint16_t broadcast_timeout;
	uint8_t rx_broadcast_in_ps;
	uint8_t ps_poll_threshold;
	uint8_t pad[2];
} __packed;

struct wl1251_ie_event_mask {
	struct wl1251_ie_hdr hdr;
	uint32_t event_mask;
	uint32_t high_event_mask;
} __packed;

struct wl1251_ie_station_id {
	struct wl1251_ie_hdr hdr;
	/* Last octet first */
	uint8_t mac[6];
	uint8_t pad[2];
} __packed;

struct wl1251_ie_frame_rates {
	struct wl1251_ie_hdr hdr;
	uint8_t ctrl_rate;
	uint8_t ctrl_mod;
	uint8_t mgmt_rate;
	uint8_t mgmt_mod;
} __packed;

struct wl1251_cmd_path {
	struct wl1251_cmd_hdr hdr;
	uint8_t channel;
	uint8_t pad[3];
} __packed;

struct wl1251_cmd_template {
	struct wl1251_cmd_hdr hdr;
	uint16_t size;
	uint8_t data[WL1251_MAX_TEMPLATE_SIZE + 2];
} __packed;

struct wl1251_cmd_vbm {
	struct wl1251_cmd_hdr hdr;
	uint16_t len;
	uint8_t pad[2];
	/* A TIM element */
	uint8_t id;
	uint8_t length;
	uint8_t dtim_count;
	uint8_t dtim_period;
	uint8_t bitmap_ctrl;
	uint8_t bitmap[251];
} __packed;

struct wl1251_cmd_scan {
	struct wl1251_cmd_hdr hdr;
	uint32_t rx_config;
	uint32_t rx_filter;
	uint16_t options;
	uint8_t num_channels;
	uint8_t num_probe_requests;
	uint16_t tx_rate;
	uint8_t tid_trigger;
	uint8_t ssid_len;
	uint8_t ssid[32];
	struct {
		/* In TU */
		uint32_t min_duration;
		uint32_t max_duration;
		uint8_t bssid[6];
		uint8_t early_termination;
		uint8_t tx_power_att;
		uint8_t channel;
		uint8_t pad[3];
	} __packed channels[WL1251_SCAN_MAX_CHANNELS];
} __packed;

#define WL1251_SCAN_OPT_PASSIVE       BIT(0)
#define WL1251_SCAN_OPT_PRIORITY_HIGH BIT(3)

struct wl1251_cmd_trigger_scan_to {
	struct wl1251_cmd_hdr hdr;
	uint32_t timeout;
} __packed;

struct wl1251_cmd_join {
	struct wl1251_cmd_hdr hdr;
	/* Last octet first */
	uint8_t bssid[6];
	uint16_t beacon_interval;
	uint32_t rx_config;
	uint32_t rx_filter;
	uint16_t basic_rate_set;
	uint8_t dtim_interval;
	uint8_t obsolete_1[2];
	uint8_t bss_type;
	uint8_t channel;
	uint8_t ssid_len;
	uint8_t ssid[32];
	uint8_t ctrl;
	uint8_t obsolete_2[2];
	uint8_t reserved;
} __packed;

#define WL1251_JOIN_CTRL_TX_FLUSH BIT(7)

struct wl1251_cmd_set_keys {
	struct wl1251_cmd_hdr hdr;
	uint8_t addr[6];
	uint16_t action;
	uint16_t reserved_1;
	uint8_t key_size;
	uint8_t key_type;
	uint8_t ssid_profile;
	uint8_t id;
	uint8_t reserved_2[6];
	uint8_t key[32];
	uint16_t ac_seq_num16[4];
	uint32_t ac_seq_num32[4];
} __packed;

struct wl1251_event_mbox {
	uint32_t events_vector;
	uint32_t events_mask;
	uint32_t reserved_1[2];
	int8_t average_rssi_level;
	uint8_t ps_status;
	uint8_t channel_switch_status;
	uint8_t scheduled_scan_status;
	uint16_t scheduled_scan_channels;
	uint16_t health_report;
	uint16_t bad_fft_counter;
	uint8_t bt_pta_sense_info;
	uint8_t bt_pta_protective_info;
	uint32_t reserved_2;
	uint32_t debug_report[2];
	uint32_t fcs_err_counter;
	uint8_t debug_event[16];
	uint8_t average_snr_level;
	uint8_t pad[19];
} __packed;

struct wl1251_rx_desc {
	/* In microseconds */
	uint32_t timestamp;
	/* Of the frame, plus WL1251_RX_PLCP_LEN */
	uint16_t length;
	uint16_t flags;
	uint8_t type;
	uint8_t rate;
	uint8_t mod_pre;
	uint8_t channel;
	uint8_t band;
	int8_t rssi;
	uint8_t rcpi;
	uint8_t snr;
} __packed;

#define WL1251_RX_FLAG_VALID_FCS    BIT(0)
#define WL1251_RX_FLAG_ENCRYPTED    GENMASK(9, 8)
#define WL1251_RX_FLAG_MIC_FAIL     BIT(13)
#define WL1251_RX_FLAG_DECRYPT_FAIL BIT(14)

struct wl1251_tx_desc {
	uint16_t length;
	uint16_t rate;
	/* In TU */
	uint32_t expiry_time;
	uint8_t xmit_queue;
	uint8_t id;
	uint16_t control;
	uint16_t frag_threshold;
	uint8_t num_mem_blocks;
	uint8_t reserved;
} __packed;

#define WL1251_TX_CTRL_QOS         BIT(6)
#define WL1251_TX_CTRL_TX_COMPLETE BIT(7)

struct wl1251_tx_result {
	uint8_t done_1;
	uint8_t id;
	uint16_t medium_usage;
	uint32_t medium_delay;
	uint32_t fw_handling_time;
	uint8_t lsb_seq_num;
	uint8_t ack_failures;
	uint16_t rate;
	uint16_t reserved;
	uint8_t status;
	uint8_t done_2;
} __packed;

/* IEEE 802.11 frame control, as the first two octets of a frame */
#define WL1251_FC_TYPE_MASK    0x000cU
#define WL1251_FC_TYPE_MGMT    0x0000U
#define WL1251_FC_TYPE_DATA    0x0008U
#define WL1251_FC_STYPE_MASK   0x00f0U
#define WL1251_FC_ASSOC_REQ    0x0000U
#define WL1251_FC_ASSOC_RESP   0x0010U
#define WL1251_FC_REASSOC_RESP 0x0030U
#define WL1251_FC_PROBE_REQ    0x0040U
#define WL1251_FC_PROBE_RESP   0x0050U
#define WL1251_FC_BEACON       0x0080U
#define WL1251_FC_DISASSOC     0x00a0U
#define WL1251_FC_AUTH         0x00b0U
#define WL1251_FC_DEAUTH       0x00c0U
#define WL1251_FC_DATA         0x0008U
#define WL1251_FC_NULL_DATA    0x0048U
#define WL1251_FC_PS_POLL      0x00a4U
/* Bits of the subtype of data frames */
#define WL1251_FC_STYPE_NULL   0x0040U
#define WL1251_FC_STYPE_QOS    0x0080U
#define WL1251_FC_TO_DS        0x0100U
#define WL1251_FC_FROM_DS      0x0200U
#define WL1251_FC_PROTECTED    0x4000U

#define WL1251_CAPA_ESS        BIT(0)
#define WL1251_CAPA_PRIVACY    BIT(4)
#define WL1251_CAPA_SHORT_PRE  BIT(5)
#define WL1251_CAPA_SHORT_SLOT BIT(10)

#define WL1251_IE_ID_SSID      0
#define WL1251_IE_ID_RATES     1
#define WL1251_IE_ID_DS_PARAMS 3
#define WL1251_IE_ID_RSN       48
#define WL1251_IE_ID_EXT_RATES 50
#define WL1251_IE_ID_VENDOR    221

struct wl1251_80211_hdr {
	uint16_t frame_control;
	uint16_t duration;
	uint8_t addr1[6];
	uint8_t addr2[6];
	uint8_t addr3[6];
	uint16_t seq_ctrl;
} __packed;

/* Fixed fields of beacons and probe responses, after the header */
struct wl1251_80211_beacon {
	uint8_t timestamp[8];
	uint16_t beacon_interval;
	uint16_t capability;
} __packed;

/* A network seen in a scan */
struct wl1251_bss {
	uint8_t bssid[6];
	uint8_t channel;
	int8_t rssi;
	uint16_t beacon_interval;
	uint16_t capability;
	uint16_t ies_len;
	bool valid;
	/* From a probe response rather than a beacon */
	bool probe_resp;
	uint8_t ies[CONFIG_WIFI_WL1251_BSS_IES_SIZE];
};

struct wl1251_config {
	struct spi_dt_spec bus;
	struct gpio_dt_spec power;
	struct gpio_dt_spec irq;
	struct net_eth_mac_config mac;
	k_thread_stack_t *stack;
};

/* What joining a network takes to know about it */
struct wl1251_bss_params {
	uint8_t bssid[6];
	uint8_t channel;
	uint16_t beacon_interval;
	uint16_t capability;
	/* WL1251_RATE_* the access point takes */
	uint16_t rates;
};

struct wl1251_scan_req {
	/* Network to probe for, or none for all that answer */
	const uint8_t *ssid;
	uint8_t ssid_len;
	bool passive;
	/* Channels to scan, or none for all */
	const uint8_t *channels;
	uint8_t num_channels;
};

enum wl1251_state {
	WL1251_STATE_IDLE,
	WL1251_STATE_AUTHENTICATING,
	WL1251_STATE_AUTHENTICATED,
	WL1251_STATE_ASSOCIATING,
	WL1251_STATE_ASSOCIATED,
};

struct wl1251_data {
	const struct device *dev;
	struct net_if *iface;
	uint8_t mac[6];

	/* Serializes everything that talks to the chip */
	struct k_mutex lock;
	/* Held from composing a frame in the transmit buffer until it is out */
	struct k_mutex tx_lock;
	bool running;

	struct gpio_callback irq_cb;
	struct k_sem work_sem;
	struct k_thread thread;

	/* Address windows */
	uint32_t mem_start;
	uint32_t mem_size;
	uint32_t reg_start;

	uint32_t chip_id;
	char fw_version[21];
	uint32_t cmd_mbox;
	uint32_t event_mbox[2];
	uint32_t intr_mask;
	struct wl1251_ie_data_path_resp path;
	uint32_t tx_blocks;

	/* Receive: the firmware alternates between two buffers */
	uint32_t rx_counter;
	uint32_t rx_handled;
	uint8_t rx_buffer;

	/* Transmit */
	uint8_t tx_in_count;
	uint8_t tx_next_result;
	/* Frame IDs in flight, and those of them sent at the adapted rate */
	uint16_t tx_busy;
	uint16_t tx_adapt;
	struct k_sem tx_sem;
	struct k_sem join_sem;
	uint16_t tx_seq;
	/* CCMP packet number of the pairwise key */
	uint64_t tx_pn;
	/* Rate of data frames, adapted to how they get through */
	uint8_t tx_rate;
	uint8_t tx_good;
	uint8_t tx_bad;

	/* Station state */
	enum wl1251_state state;
	struct wl1251_bss_params bss_params;
	uint16_t aid;
	int8_t rssi;
	bool joined;
	/* Data frames pass, not only the key handshake */
	bool authorized;
	/* A pairwise key is set: data frames are encrypted */
	bool pairwise_key;
	uint8_t pairwise_key_idx;
	uint8_t group_keys;
	uint8_t keys[WL1251_PAIRWISE_KEY_SLOT + 1][WL1251_KEY_SIZE];
	uint32_t rx_filter;
	/* Elements of the last association request */
	uint8_t assoc_ies[256];
	uint16_t assoc_ies_len;

	bool scanning;
	bool scan_from_supp;
	scan_result_cb_t scan_cb;
	struct wl1251_bss bss[CONFIG_WIFI_WL1251_MAX_BSS];

	/* Scratch buffers, used with the lock held */
	union {
		uint32_t align;
		uint8_t buf[600];
	} cmd;
	uint8_t frame[WL1251_RING_CHUNK_SIZE];
	uint8_t tx_frame[WL1251_RING_CHUNK_SIZE];

	/* Of the wpa_supplicant glue */
	void *supp;
};

/* Chip access; all of these need the lock */
int wl1251_spi_reset_wake(const struct device *dev);
int wl1251_raw_read(const struct device *dev, uint32_t addr, void *buf, size_t len);
int wl1251_raw_write(const struct device *dev, uint32_t addr, const void *buf, size_t len);
int wl1251_set_partition(const struct device *dev, uint32_t mem_start, uint32_t mem_size,
			 uint32_t reg_start, uint32_t reg_size);
int wl1251_mem_read(const struct device *dev, uint32_t addr, void *buf, size_t len);
int wl1251_mem_write(const struct device *dev, uint32_t addr, const void *buf, size_t len);
uint32_t wl1251_mem_read32(const struct device *dev, uint32_t addr);
void wl1251_mem_write32(const struct device *dev, uint32_t addr, uint32_t val);
uint32_t wl1251_reg_read32(const struct device *dev, uint32_t addr);
void wl1251_reg_write32(const struct device *dev, uint32_t addr, uint32_t val);

/* A zeroed command of the given size in the scratch buffer */
void *wl1251_cmd_buf(const struct device *dev, size_t len);
int wl1251_cmd_send(const struct device *dev, uint16_t id, void *cmd, size_t len);
/* What the firmware made of the last command */
int wl1251_cmd_status(const struct device *dev, uint16_t id);
int wl1251_cmd_interrogate(const struct device *dev, uint16_t id, void *ie, size_t len);
int wl1251_cmd_configure(const struct device *dev, uint16_t id, void *ie, size_t len);
int wl1251_cmd_template(const struct device *dev, uint16_t id, const void *frame, size_t len);
int wl1251_cfg_u8(const struct device *dev, uint16_t id, uint8_t value);
int wl1251_cfg_u16(const struct device *dev, uint16_t id, uint16_t value);
int wl1251_cfg_u32(const struct device *dev, uint16_t id, uint32_t value);

int wl1251_boot(const struct device *dev);
void wl1251_power_off(const struct device *dev);

/*
 * Station management, for whoever decides which network to join: these take
 * the lock themselves.
 */
int wl1251_scan_start(const struct device *dev, const struct wl1251_scan_req *req,
		      scan_result_cb_t cb);
int wl1251_authenticate(const struct device *dev, const struct wl1251_bss_params *bss,
			uint16_t algorithm);
int wl1251_associate(const struct device *dev, const uint8_t *ssid, size_t ssid_len,
		     const uint8_t *ies, size_t ies_len);
int wl1251_deauthenticate(const struct device *dev, uint16_t reason);
int wl1251_set_key(const struct device *dev, bool pairwise, uint8_t idx, const uint8_t *key,
		   size_t key_len);
int wl1251_remove_key(const struct device *dev, bool pairwise, uint8_t idx);
void wl1251_set_authorized(const struct device *dev, bool authorized);
/* Of data frames, as the rate adaptation has it now */
uint32_t wl1251_tx_rate_kbps(const struct device *dev);
int wl1251_send_eapol(const struct device *dev, const uint8_t *dst, uint16_t type,
		      const uint8_t *buf, size_t len, bool encrypt);

const uint8_t *wl1251_find_ie(const uint8_t *ies, size_t len, uint8_t id);
/* The WL1251_RATE_* of a Supported Rates element, or 0 without one */
uint16_t wl1251_ie_rates(const uint8_t *ie);

/*
 * What wpa_supplicant needs to hear of. Called in the driver thread with the
 * lock held.
 */
struct zep_wpa_supp_dev_ops;
extern const struct zep_wpa_supp_dev_ops wl1251_supp_ops;
void wl1251_supp_scan_done(const struct device *dev, bool aborted);
void wl1251_supp_mgmt_rx(const struct device *dev, const uint8_t *frame, size_t len);
void wl1251_supp_link_lost(const struct device *dev);

#endif /* ZEPHYR_DRIVERS_WIFI_WL1251_WL1251_H_ */
