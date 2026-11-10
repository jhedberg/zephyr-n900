/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Bluetooth HCI driver for the controllers of Nokia phones that speak H4+ over
 * a UART, such as the BCM2048 of the N900. H4+ is H4 with:
 * - every frame, packet indicator included, padded to an even length;
 * - a negotiation packet that sets the UART speed and identifies the chip, and
 *   an alive packet that checks the link, both exchanged before any HCI traffic;
 * - a radio packet type that carries HCI events;
 * - wakeup lines in both directions: the host raises BT wakeup while it
 *   transmits, upon which the controller raises CTS, and RTS follows the
 *   controller's host wakeup line. A controller that has gone to sleep with
 *   RTS held high does not send anything any more.
 *
 * The BCM2048's ROM negotiates and answers HCI_Reset, but nothing else until
 * Nokia's firmware script (bcmfw.bin from Maemo) has been sent to it as HCI
 * commands. The script may not be redistributed, so its path is given at build
 * time with CONFIG_BT_NOKIA_H4P_FIRMWARE.
 */

#define DT_DRV_COMPAT nokia_h4p_bluetooth

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/drivers/bluetooth/hci_lockstep.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/sys_io.h>

LOG_MODULE_REGISTER(bt_nokia_h4p, CONFIG_BT_HCI_DRIVER_LOG_LEVEL);

/* The speed of every H4+ controller out of reset */
#define INIT_BAUD 120000U

#define PKT_NEG   0x06
#define PKT_ALIVE 0x07
#define PKT_RADIO 0x08

#define NEG_REQ   0x00
#define NEG_ACK   0x20
#define NEG_PROTO 0x4c
/* dlen, then ack, baud, 2, proto, sysclk, 2, manufacturer, version */
#define NEG_REPLY_LEN 13U

#define ALIVE_REQ  0x55
#define ALIVE_RESP 0xcc

/* Vendor commands of the BCM2048 */
#define BCM_OP_WRITE_BD_ADDR 0xfc01
#define BCM_OP_SET_UART_BAUD 0xfc18

/*
 * No UART API reads CTS, which the controller drops while it sleeps, and this
 * UART has no automatic CTS flow control in the ns16550 driver. Until it does,
 * the modem status register is read through the UART driver's mapping.
 */
#define UART_MSR 6U
#define MSR_CTS  BIT(4)

#define CTS_TIMEOUT_MS   200U
#define SETUP_TIMEOUT_MS 2000U
/* Longest wait for the rest of a frame once its first byte has arrived */
#define FRAME_TIMEOUT_MS 1000U

#define RX_RING_SIZE 2048U
/* Packet indicator, then the largest event or ACL packet the host takes */
#define RX_FRAME_SIZE (1U + MAX(BT_HCI_EVT_HDR_SIZE + 255U, \
				BT_HCI_ACL_HDR_SIZE + CONFIG_BT_BUF_ACL_RX_SIZE))

#if defined(NOKIA_H4P_FIRMWARE)
static const uint8_t firmware[] = {
#include "nokia_h4p_firmware.inc"
};
#endif

struct h4p_config {
	/* bt_hci_driver_config must be first */
	struct bt_hci_driver_config common;
	const struct device *uart;
	uint8_t uart_reg_shift;
	uint32_t baud;
	uint32_t sysclk_hz;
	struct gpio_dt_spec reset;
	struct gpio_dt_spec bt_wakeup;
	struct gpio_dt_spec host_wakeup;
	const struct pinctrl_dev_config *pcfg;
	k_thread_stack_t *rx_stack;
	size_t rx_stack_size;
	struct k_thread *rx_thread;
	struct ring_buf *rx_ring;
};

struct h4p_data {
	/* bt_hci_driver_data must be first */
	struct bt_hci_driver_data common;
	const struct device *dev;

	struct k_spinlock rx_lock;
	struct k_sem rx_sem;
	/* A byte read while looking for padding that starts the next frame */
	int pushback;
	uint8_t frame[RX_FRAME_SIZE];

	/* Negotiation and alive replies, handed from the RX thread to open() */
	struct k_sem setup_sem;
	uint8_t setup_type;
	uint8_t setup_reply[NEG_REPLY_LEN];

	struct k_mutex tx_lock;
	struct gpio_callback host_wakeup_cb;
	/* Sleep handshake: BT wakeup per transmission, RTS following host wakeup */
	bool sleep_handshake;

	struct bt_hci_lockstep lockstep;
};

static bool cts(const struct device *dev)
{
	const struct h4p_config *cfg = dev->config;
	mm_reg_t base = DEVICE_MMIO_GET(cfg->uart);

	return (sys_read8(base + (UART_MSR << cfg->uart_reg_shift)) & MSR_CTS) != 0U;
}

static int wait_cts(const struct device *dev, uint32_t timeout_ms)
{
	k_timepoint_t end = sys_timepoint_calc(K_MSEC(timeout_ms));

	while (!cts(dev)) {
		if (sys_timepoint_expired(end)) {
			return -ETIMEDOUT;
		}
		k_busy_wait(50);
	}

	return 0;
}

static void uart_isr(const struct device *uart, void *user_data)
{
	const struct device *dev = user_data;
	const struct h4p_config *cfg = dev->config;
	struct h4p_data *h4p = dev->data;
	uint8_t buf[64];
	int n;

	/* rx_ready reports the status cached by irq_update: drain the FIFO */
	uart_irq_update(uart);
	if (uart_irq_rx_ready(uart) <= 0) {
		return;
	}

	n = uart_fifo_read(uart, buf, sizeof(buf));
	while (n > 0) {
		k_spinlock_key_t key = k_spin_lock(&h4p->rx_lock);
		uint32_t put = ring_buf_put(cfg->rx_ring, buf, n);

		k_spin_unlock(&h4p->rx_lock, key);
		if (put < (uint32_t)n) {
			LOG_ERR("RX ring full, %u bytes lost", (uint32_t)n - put);
		}
		n = uart_fifo_read(uart, buf, sizeof(buf));
	}
	k_sem_give(&h4p->rx_sem);
}

static void rx_flush(const struct device *dev)
{
	const struct h4p_config *cfg = dev->config;
	struct h4p_data *h4p = dev->data;
	k_spinlock_key_t key = k_spin_lock(&h4p->rx_lock);

	ring_buf_reset(cfg->rx_ring);
	h4p->pushback = -1;
	k_spin_unlock(&h4p->rx_lock, key);
}

static int rx_byte(const struct device *dev, uint8_t *b, k_timepoint_t end)
{
	const struct h4p_config *cfg = dev->config;
	struct h4p_data *h4p = dev->data;

	if (h4p->pushback >= 0) {
		*b = (uint8_t)h4p->pushback;
		h4p->pushback = -1;
		return 0;
	}

	while (true) {
		k_spinlock_key_t key = k_spin_lock(&h4p->rx_lock);
		uint32_t got = ring_buf_get(cfg->rx_ring, b, 1);

		k_spin_unlock(&h4p->rx_lock, key);
		if (got == 1U) {
			return 0;
		}
		if (k_sem_take(&h4p->rx_sem, sys_timepoint_timeout(end)) != 0) {
			return -ETIMEDOUT;
		}
	}
}

/* Reads len bytes into buf, or skips them when buf is NULL */
static int rx_bytes(const struct device *dev, uint8_t *buf, size_t len, k_timepoint_t end)
{
	uint8_t b;

	for (size_t i = 0; i < len; i++) {
		int err = rx_byte(dev, (buf != NULL) ? &buf[i] : &b, end);

		if (err != 0) {
			return err;
		}
	}

	return 0;
}

/*
 * Reads one frame into h4p->frame, packet indicator first. Returns its length,
 * 0 for a frame that was dropped, or a negative error.
 */
static int rx_frame(const struct device *dev)
{
	struct h4p_data *h4p = dev->data;
	uint8_t *f = h4p->frame;
	k_timepoint_t end;
	size_t hdr_len;
	size_t len;
	size_t total;
	bool dropped;
	uint8_t pad;
	int err;

	/* Bytes that cannot start a frame, such as one the BCM2048 sends out of reset */
	do {
		err = rx_byte(dev, &f[0], sys_timepoint_calc(K_FOREVER));
		if (err != 0) {
			return err;
		}
		if ((f[0] == BT_HCI_H4_ACL) || (f[0] == BT_HCI_H4_SCO) ||
		    (f[0] == BT_HCI_H4_EVT) || (f[0] == PKT_NEG) || (f[0] == PKT_ALIVE) ||
		    (f[0] == PKT_RADIO)) {
			break;
		}
		LOG_DBG("skipping byte 0x%02x", f[0]);
	} while (true);

	switch (f[0]) {
	case PKT_NEG:
	case PKT_ALIVE:
		hdr_len = 1;
		break;
	case BT_HCI_H4_EVT:
	case PKT_RADIO:
		hdr_len = BT_HCI_EVT_HDR_SIZE;
		break;
	case BT_HCI_H4_SCO:
		hdr_len = BT_HCI_SCO_HDR_SIZE;
		break;
	default:
		hdr_len = BT_HCI_ACL_HDR_SIZE;
		break;
	}

	end = sys_timepoint_calc(K_MSEC(FRAME_TIMEOUT_MS));
	err = rx_bytes(dev, &f[1], hdr_len, end);
	if (err != 0) {
		LOG_ERR("type 0x%02x: header timed out", f[0]);
		return err;
	}

	switch (f[0]) {
	case BT_HCI_H4_ACL:
		len = sys_get_le16(&f[3]);
		break;
	default:
		/* The last header byte is the length for all the others */
		len = f[hdr_len];
		break;
	}

	total = 1U + hdr_len + len;
	dropped = total > sizeof(h4p->frame);
	if (dropped) {
		LOG_ERR("type 0x%02x: %zu bytes too long, dropped", f[0], len);
	}
	err = rx_bytes(dev, dropped ? NULL : &f[1 + hdr_len], len, end);
	if (err != 0) {
		LOG_ERR("type 0x%02x: payload timed out", f[0]);
		return err;
	}

	/* An odd frame is followed by a pad byte; no frame starts with 0 */
	if ((total % 2U) != 0U) {
		if ((rx_byte(dev, &pad, sys_timepoint_calc(K_MSEC(20))) == 0) && (pad != 0U)) {
			h4p->pushback = pad;
		}
	}

	return dropped ? 0 : (int)total;
}

static void deliver(const struct device *dev, const uint8_t *pkt, size_t len)
{
	struct h4p_data *h4p = dev->data;
	struct net_buf *buf;

	switch (pkt[0]) {
	case PKT_NEG:
	case PKT_ALIVE:
		if (k_sem_count_get(&h4p->setup_sem) == 0U) {
			h4p->setup_type = pkt[0];
			memcpy(h4p->setup_reply, &pkt[1], MIN(len - 1U, sizeof(h4p->setup_reply)));
			k_sem_give(&h4p->setup_sem);
		}
		return;
	case PKT_RADIO:
	case BT_HCI_H4_EVT:
		/* Radio packets carry events, as Linux's hci_nokia treats them */
		if (bt_hci_lockstep_feed(&h4p->lockstep, pkt, len)) {
			return;
		}
		buf = bt_buf_get_evt(pkt[1], false, K_FOREVER);
		break;
	case BT_HCI_H4_ACL:
		buf = bt_buf_get_rx(BT_BUF_ACL_IN, K_FOREVER);
		break;
	default:
		LOG_DBG("dropping packet type 0x%02x", pkt[0]);
		return;
	}

	if (len - 1U > net_buf_tailroom(buf)) {
		LOG_ERR("packet type 0x%02x of %zu bytes does not fit", pkt[0], len);
		net_buf_unref(buf);
		return;
	}

	/* The buffer starts with the packet indicator already */
	net_buf_add_mem(buf, &pkt[1], len - 1U);
	bt_hci_recv(dev, buf);
}

static void rx_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	struct h4p_data *h4p = dev->data;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		int len = rx_frame(dev);

		if (len > 0) {
			deliver(dev, h4p->frame, len);
		}
	}
}

/* Writes one frame, packet indicator first, padded to an even length */
static int tx_frame(const struct device *dev, const uint8_t *pkt, size_t len)
{
	const struct h4p_config *cfg = dev->config;
	struct h4p_data *h4p = dev->data;
	int err = 0;

	k_mutex_lock(&h4p->tx_lock, K_FOREVER);

	if (h4p->sleep_handshake) {
		gpio_pin_set_dt(&cfg->bt_wakeup, 1);
	}

	for (size_t i = 0; i < len + (len % 2U); i++) {
		err = wait_cts(dev, CTS_TIMEOUT_MS);
		if (err != 0) {
			LOG_ERR("CTS stayed low at byte %zu of %zu (host wakeup %d)", i, len,
				gpio_pin_get_dt(&cfg->host_wakeup));
			break;
		}
		uart_poll_out(cfg->uart, (i < len) ? pkt[i] : 0U);
	}

	if (h4p->sleep_handshake) {
		/* poll_out returns once a byte is queued: let the last two out */
		k_busy_wait((2U * 10U * USEC_PER_SEC) / cfg->baud + 1U);
		gpio_pin_set_dt(&cfg->bt_wakeup, 0);
	}

	k_mutex_unlock(&h4p->tx_lock);

	return err;
}

static int h4p_send_raw(const struct device *dev, const uint8_t *pkt, size_t len)
{
	return tx_frame(dev, pkt, len);
}

static int h4p_send(const struct device *dev, struct net_buf *buf)
{
	int err;

	err = tx_frame(dev, buf->data, buf->len);
	net_buf_unref(buf);

	return err;
}

static void host_wakeup_changed(const struct device *port, struct gpio_callback *cb,
				gpio_port_pins_t pins)
{
	struct h4p_data *h4p = CONTAINER_OF(cb, struct h4p_data, host_wakeup_cb);
	const struct h4p_config *cfg = h4p->dev->config;

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	/* The controller raises host wakeup while it has something to send */
	if (h4p->sleep_handshake) {
		uart_line_ctrl_set(cfg->uart, UART_LINE_CTRL_RTS,
				   gpio_pin_get_dt(&cfg->host_wakeup));
	}
}

static int set_speed(const struct device *dev, uint32_t baud)
{
	const struct h4p_config *cfg = dev->config;
	int err;

	/* RTS low while switching, so that the controller does not send meanwhile */
	uart_line_ctrl_set(cfg->uart, UART_LINE_CTRL_RTS, 0);
	uart_line_ctrl_set(cfg->uart, UART_LINE_CTRL_BAUD_RATE, baud);
	err = wait_cts(dev, CTS_TIMEOUT_MS);
	uart_line_ctrl_set(cfg->uart, UART_LINE_CTRL_RTS, 1);
	if (err != 0) {
		LOG_ERR("no CTS at %u baud", baud);
	}

	return err;
}

static int setup_exchange(const struct device *dev, const uint8_t *pkt, size_t len,
			  uint8_t type)
{
	struct h4p_data *h4p = dev->data;
	int err;

	k_sem_reset(&h4p->setup_sem);
	err = tx_frame(dev, pkt, len);
	if (err != 0) {
		return err;
	}

	if ((k_sem_take(&h4p->setup_sem, K_MSEC(SETUP_TIMEOUT_MS)) != 0) ||
	    (h4p->setup_type != type)) {
		LOG_ERR("no reply to packet type 0x%02x", type);
		return -ETIMEDOUT;
	}

	return 0;
}

static int negotiate(const struct device *dev)
{
	const struct h4p_config *cfg = dev->config;
	struct h4p_data *h4p = dev->data;
	uint8_t neg[12] = {PKT_NEG, 10, NEG_REQ};
	int err;

	/* The speed goes as a divisor of ten times the controller's clock */
	sys_put_le16((uint16_t)DIV_ROUND_CLOSEST(cfg->sysclk_hz * 10ULL, cfg->baud), &neg[3]);
	neg[7] = NEG_PROTO;
	sys_put_le16((uint16_t)(cfg->sysclk_hz / 1000U), &neg[8]);

	err = setup_exchange(dev, neg, sizeof(neg), PKT_NEG);
	if (err != 0) {
		return err;
	}

	if ((h4p->setup_reply[0] != NEG_REPLY_LEN - 1U) || (h4p->setup_reply[1] != NEG_ACK)) {
		LOG_ERR("negotiation refused (ack 0x%02x)", h4p->setup_reply[1]);
		return -EIO;
	}

	LOG_INF("manufacturer 0x%02x version 0x%02x, %u baud", h4p->setup_reply[11],
		h4p->setup_reply[12], cfg->baud);

	/* Also at an unchanged speed: the controller waits for the RTS cycle */
	return set_speed(dev, cfg->baud);
}

static int check_alive(const struct device *dev)
{
	struct h4p_data *h4p = dev->data;
	static const uint8_t alive[] = {PKT_ALIVE, 2, ALIVE_REQ, 0};
	int err;

	err = setup_exchange(dev, alive, sizeof(alive), PKT_ALIVE);
	if (err != 0) {
		return err;
	}

	if (h4p->setup_reply[1] != ALIVE_RESP) {
		LOG_ERR("bad alive reply 0x%02x", h4p->setup_reply[1]);
		return -EIO;
	}

	return 0;
}

BT_HCI_PKT_CMD_DEFINE_STATIC(vnd_cmd, UINT8_MAX);

/*
 * Sends the firmware script as Linux's hci_nokia does: records of a le16
 * length (of type and payload), a type and a payload, of which only the HCI
 * commands are sent. The last command moves the controller to 3692307 baud,
 * which the UART cannot produce from its clock in 16x mode, so it is left out
 * and the negotiated speed stays.
 */
static int send_firmware(const struct device *dev)
{
#if defined(NOKIA_H4P_FIRMWARE)
	struct h4p_data *h4p = dev->data;
	size_t off = 0;

	while (sizeof(firmware) - off >= 4U) {
		uint16_t size = sys_get_le16(&firmware[off]);
		const uint8_t *rec = &firmware[off + 2U];
		uint16_t opcode;
		int err;

		if ((size == 0U) || (off + 2U + size > sizeof(firmware))) {
			LOG_ERR("bad firmware record at %zu", off);
			return -EINVAL;
		}

		off += 2U + size;
		/* Records are padded to an even length, like H4+ frames */
		if ((rec[0] != BT_HCI_H4_CMD) || (size < 4U) || (rec[3] > size - 4U)) {
			continue;
		}

		opcode = sys_get_le16(&rec[1]);
		if (opcode == BCM_OP_SET_UART_BAUD) {
			continue;
		}

		bt_hci_pkt_reset_cmd(&vnd_cmd);
		net_buf_simple_add_mem(&vnd_cmd, &rec[4], rec[3]);
		err = bt_hci_lockstep_cmd_send_sync(&h4p->lockstep, opcode, &vnd_cmd, NULL);
		if (err != 0) {
			return err;
		}
	}

	return 0;
#else
	LOG_WRN("no firmware built in (CONFIG_BT_NOKIA_H4P_FIRMWARE): a BCM2048 will not work");
	return 0;
#endif
}

static int set_public_addr(const struct device *dev)
{
	struct h4p_data *h4p = dev->data;
	const bt_addr_t *addr = bt_hci_get_public_addr(dev);

	if (bt_addr_eq(addr, BT_ADDR_ANY)) {
		return 0;
	}

	bt_hci_pkt_reset_cmd(&vnd_cmd);
	net_buf_simple_add_mem(&vnd_cmd, addr->val, sizeof(addr->val));

	return bt_hci_lockstep_cmd_send_sync(&h4p->lockstep, BCM_OP_WRITE_BD_ADDR, &vnd_cmd,
					     NULL);
}

/*
 * Lets the controller sleep between transmissions: BT wakeup goes high only
 * while transmitting, and RTS follows host wakeup. The BCM2048 firmware script
 * enables its sleep mode partway, after which the controller holds CTS low
 * while it sleeps, so this starts before the script rather than after it.
 */
static void start_sleep_handshake(const struct device *dev)
{
	const struct h4p_config *cfg = dev->config;
	struct h4p_data *h4p = dev->data;

	h4p->sleep_handshake = true;
	gpio_pin_set_dt(&cfg->bt_wakeup, 0);
	uart_line_ctrl_set(cfg->uart, UART_LINE_CTRL_RTS, gpio_pin_get_dt(&cfg->host_wakeup));
}

/* Holds the controller in reset, with reception off */
static void power_down(const struct device *dev)
{
	const struct h4p_config *cfg = dev->config;
	struct h4p_data *h4p = dev->data;

	h4p->sleep_handshake = false;
	uart_irq_rx_disable(cfg->uart);
	gpio_pin_set_dt(&cfg->reset, 1);
	gpio_pin_set_dt(&cfg->bt_wakeup, 0);
}

static int h4p_open(const struct device *dev)
{
	const struct h4p_config *cfg = dev->config;
	struct h4p_data *h4p = dev->data;
	int err;

	/* Reset, with BT wakeup high until the controller is set up */
	power_down(dev);
	gpio_pin_set_dt(&cfg->bt_wakeup, 1);
	k_msleep(100);

	uart_line_ctrl_set(cfg->uart, UART_LINE_CTRL_RTS, 0);
	uart_line_ctrl_set(cfg->uart, UART_LINE_CTRL_BAUD_RATE, INIT_BAUD);
	rx_flush(dev);
	uart_irq_rx_enable(cfg->uart);

	gpio_pin_set_dt(&cfg->reset, 0);
	err = wait_cts(dev, CTS_TIMEOUT_MS);
	uart_line_ctrl_set(cfg->uart, UART_LINE_CTRL_RTS, 1);
	if (err != 0) {
		LOG_ERR("no CTS out of reset");
		goto fail;
	}

	/* What the reset controller sent or allowed before does not count */
	bt_hci_lockstep_reset(&h4p->lockstep);

	err = negotiate(dev);
	if (err == 0) {
		err = check_alive(dev);
	}
	if (err != 0) {
		goto fail;
	}

	start_sleep_handshake(dev);
	err = send_firmware(dev);
	if (err == 0) {
		err = set_public_addr(dev);
	}
	if (err != 0) {
		goto fail;
	}

	return 0;

fail:
	power_down(dev);
	return (err == -ETIMEDOUT) ? -EIO : err;
}

static int h4p_close(const struct device *dev)
{
	power_down(dev);

	return 0;
}

static DEVICE_API(bt_hci, h4p_driver_api) = {
	.open = h4p_open,
	.send = h4p_send,
	.close = h4p_close,
};

static int h4p_init(const struct device *dev)
{
	const struct h4p_config *cfg = dev->config;
	struct h4p_data *h4p = dev->data;
	int err;

	if (!device_is_ready(cfg->uart) || !gpio_is_ready_dt(&cfg->reset) ||
	    !gpio_is_ready_dt(&cfg->bt_wakeup) || !gpio_is_ready_dt(&cfg->host_wakeup)) {
		return -ENODEV;
	}

	err = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (err != 0) {
		return err;
	}

	h4p->dev = dev;
	h4p->pushback = -1;
	k_sem_init(&h4p->rx_sem, 0, 1);
	k_sem_init(&h4p->setup_sem, 0, 1);
	k_mutex_init(&h4p->tx_lock);
	bt_hci_lockstep_init(&h4p->lockstep, dev, h4p_send_raw);

	/* The controller stays in reset until the transport is opened */
	err = gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_ACTIVE);
	if (err == 0) {
		err = gpio_pin_configure_dt(&cfg->bt_wakeup, GPIO_OUTPUT_INACTIVE);
	}
	if (err == 0) {
		err = gpio_pin_configure_dt(&cfg->host_wakeup, GPIO_INPUT);
	}
	if (err != 0) {
		return err;
	}

	gpio_init_callback(&h4p->host_wakeup_cb, host_wakeup_changed, BIT(cfg->host_wakeup.pin));
	err = gpio_add_callback_dt(&cfg->host_wakeup, &h4p->host_wakeup_cb);
	if (err == 0) {
		err = gpio_pin_interrupt_configure_dt(&cfg->host_wakeup, GPIO_INT_EDGE_BOTH);
	}
	if (err != 0) {
		return err;
	}

	uart_irq_rx_disable(cfg->uart);
	uart_irq_tx_disable(cfg->uart);
	uart_irq_callback_user_data_set(cfg->uart, uart_isr, (void *)dev);

	k_thread_create(cfg->rx_thread, cfg->rx_stack, cfg->rx_stack_size, rx_thread,
			(void *)dev, NULL, NULL, K_PRIO_COOP(CONFIG_BT_RX_PRIO), 0, K_NO_WAIT);
	k_thread_name_set(cfg->rx_thread, "bt_nokia_h4p_rx");

	return 0;
}

#define H4P_DEVICE_INIT(inst)                                                                      \
	PINCTRL_DT_INST_DEFINE(inst);                                                              \
	static K_KERNEL_STACK_DEFINE(rx_stack_##inst, CONFIG_BT_NOKIA_H4P_RX_STACK_SIZE);         \
	static struct k_thread rx_thread_##inst;                                                   \
	RING_BUF_DECLARE(h4p_rx_ring_##inst, RX_RING_SIZE);                                        \
	static const struct h4p_config h4p_config_##inst = {                                       \
		.common = BT_DT_HCI_DRIVER_CONFIG_INST_GET(inst),                                  \
		.uart = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                       \
		.uart_reg_shift = DT_PROP_OR(DT_INST_PARENT(inst), reg_shift, 0),                  \
		.baud = DT_PROP(DT_INST_PARENT(inst), current_speed),                              \
		.sysclk_hz = DT_PROP(DT_INST_CLOCKS_CTLR(inst), clock_frequency),                  \
		.reset = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),                                 \
		.bt_wakeup = GPIO_DT_SPEC_INST_GET(inst, bluetooth_wakeup_gpios),                  \
		.host_wakeup = GPIO_DT_SPEC_INST_GET(inst, host_wakeup_gpios),                     \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                                      \
		.rx_stack = rx_stack_##inst,                                                       \
		.rx_stack_size = K_KERNEL_STACK_SIZEOF(rx_stack_##inst),                           \
		.rx_thread = &rx_thread_##inst,                                                    \
		.rx_ring = &h4p_rx_ring_##inst,                                                    \
	};                                                                                         \
	static struct h4p_data h4p_data_##inst;                                                    \
	DEVICE_DT_INST_DEFINE(inst, h4p_init, NULL, &h4p_data_##inst, &h4p_config_##inst,          \
			      POST_KERNEL, CONFIG_BT_HCI_INIT_PRIORITY, &h4p_driver_api);

DT_INST_FOREACH_STATUS_OKAY(H4P_DEVICE_INIT)
