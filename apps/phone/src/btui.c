/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * N900 phone demo: Bluetooth BR/EDR side. Host callbacks run in Bluetooth
 * threads: they update the state below under the lock and post PEV_BT events,
 * and the UI reads snapshots of the state in the LVGL thread.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/classic/classic.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "btui.h"
#include "phone.h"

LOG_MODULE_REGISTER(btui, LOG_LEVEL_INF);

/* Inquiry length in units of 1.28 s */
#define DISCOVERY_LENGTH 8

enum auth_kind {
	AUTH_NONE,
	AUTH_DISPLAY,
	AUTH_PASSKEY_CONFIRM,
	AUTH_PAIRING_CONFIRM,
};

static K_MUTEX_DEFINE(lock);
static struct btui_info info;
static struct btui_device devices[BTUI_MAX_DEVICES];
static size_t device_count;
static struct bt_br_discovery_result results[BTUI_MAX_DEVICES];

/* Outgoing pairing: the connection is secured once it is up */
static struct bt_conn *pair_conn;
/* Pairing request waiting for the user's answer */
static struct bt_conn *auth_conn;
static enum auth_kind auth_kind;

static void post(enum btui_event ev, int32_t value)
{
	phone_post(PEV_BT, ev, value);
}

/* Name from Extended Inquiry Response data, empty if it has none */
static void eir_name(const uint8_t *eir, size_t len, char *name, size_t size)
{
	size_t i = 0;

	name[0] = '\0';
	while ((i + 1U < len) && (eir[i] != 0U) && (i + 1U + eir[i] <= len)) {
		uint8_t field = eir[i];
		uint8_t type = eir[i + 1U];

		if ((type == BT_DATA_NAME_COMPLETE) || (type == BT_DATA_NAME_SHORTENED)) {
			size_t n = MIN((size_t)field - 1U, size - 1U);

			memcpy(name, &eir[i + 2U], n);
			name[n] = '\0';
			if (type == BT_DATA_NAME_COMPLETE) {
				return;
			}
		}
		i += 1U + field;
	}
}

/* Lock held */
static struct btui_device *find_device(const bt_addr_t *addr)
{
	for (size_t i = 0; i < device_count; i++) {
		if (bt_addr_eq(&devices[i].addr, addr)) {
			return &devices[i];
		}
	}

	return NULL;
}

/* Lock held */
static void peer_name(const bt_addr_t *addr, char *name, size_t len)
{
	const struct btui_device *d = find_device(addr);

	if ((d != NULL) && (d->name[0] != '\0')) {
		snprintf(name, len, "%s", d->name);
	} else {
		bt_addr_to_str(addr, name, len);
	}
}

/* Lock held */
static void update_device(const struct bt_br_discovery_result *r)
{
	struct btui_device *d = find_device(&r->addr);
	char name[BTUI_NAME_LEN];

	if (d == NULL) {
		if (device_count == ARRAY_SIZE(devices)) {
			return;
		}
		d = &devices[device_count++];
		memset(d, 0, sizeof(*d));
		bt_addr_copy(&d->addr, &r->addr);
	}

	d->rssi = r->rssi;
	d->cod = sys_get_le24(r->cod);
	eir_name(r->eir, sizeof(r->eir), name, sizeof(name));
	if (name[0] != '\0') {
		memcpy(d->name, name, sizeof(d->name));
	}
}

static void discovery_recv(const struct bt_br_discovery_result *result)
{
	k_mutex_lock(&lock, K_FOREVER);
	update_device(result);
	k_mutex_unlock(&lock);
	post(BTUI_EV_DEVICES, 0);
}

/* At the end of the inquiry, with the names the Host resolved */
static void discovery_timeout(const struct bt_br_discovery_result *r, size_t count)
{
	k_mutex_lock(&lock, K_FOREVER);
	for (size_t i = 0; i < count; i++) {
		update_device(&r[i]);
	}
	info.discovering = false;
	k_mutex_unlock(&lock);
	post(BTUI_EV_DEVICES, 0);
	post(BTUI_EV_DISCOVERY, 0);
}

static struct bt_br_discovery_cb discovery_cb = {
	.recv = discovery_recv,
	.timeout = discovery_timeout,
};

static bool is_br(struct bt_conn *conn, const bt_addr_t **dst)
{
	struct bt_conn_info ci;

	if ((bt_conn_get_info(conn, &ci) != 0) || (ci.type != BT_CONN_TYPE_BR)) {
		return false;
	}
	if (dst != NULL) {
		*dst = ci.br.dst;
	}

	return true;
}

/* Lock held */
static void auth_clear(void)
{
	if (auth_conn != NULL) {
		bt_conn_unref(auth_conn);
		auth_conn = NULL;
	}
	auth_kind = AUTH_NONE;
}

/* Lock held; drops the link of an outgoing pairing once it is done */
static void pair_done(struct bt_conn *conn, bool disconnect)
{
	if ((pair_conn == NULL) || (conn != pair_conn)) {
		return;
	}
	if (disconnect) {
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	bt_conn_unref(pair_conn);
	pair_conn = NULL;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	bool secure = false;

	if (!is_br(conn, NULL)) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (err != 0U) {
		if (conn == pair_conn) {
			pair_done(conn, false);
			k_mutex_unlock(&lock);
			post(BTUI_EV_PAIR_FAILED, BT_SECURITY_ERR_UNSPECIFIED);
			return;
		}
		k_mutex_unlock(&lock);
		return;
	}
	info.connections++;
	secure = (conn == pair_conn);
	k_mutex_unlock(&lock);

	post(BTUI_EV_CONN, 0);
	if (secure && (bt_conn_set_security(conn, BT_SECURITY_L2) != 0)) {
		k_mutex_lock(&lock, K_FOREVER);
		pair_done(conn, true);
		k_mutex_unlock(&lock);
		post(BTUI_EV_PAIR_FAILED, BT_SECURITY_ERR_UNSPECIFIED);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	bool cancelled = false;

	if (!is_br(conn, NULL)) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (info.connections > 0U) {
		info.connections--;
	}
	pair_done(conn, false);
	if (conn == auth_conn) {
		auth_clear();
		cancelled = true;
	}
	k_mutex_unlock(&lock);

	post(BTUI_EV_CONN, 0);
	if (cancelled) {
		post(BTUI_EV_CANCEL, 0);
	}
}

/*
 * An outgoing pairing ends once the link is authenticated and encrypted:
 * pairing_complete comes with Simple Pairing Complete, before the link key
 * and the authentication result, and dropping the link there fails it.
 */
static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	bool ours;

	ARG_UNUSED(level);

	if (!is_br(conn, NULL)) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);
	ours = (conn == pair_conn);
	pair_done(conn, true);
	k_mutex_unlock(&lock);

	post(BTUI_EV_BONDS, 0);
	if (ours && (err != BT_SECURITY_ERR_SUCCESS)) {
		post(BTUI_EV_PAIR_FAILED, err);
	}
}

BT_CONN_CB_DEFINE(btui_conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

static void auth_request(struct bt_conn *conn, enum auth_kind kind, enum btui_event ev,
			 int32_t value)
{
	k_mutex_lock(&lock, K_FOREVER);
	auth_clear();
	auth_conn = bt_conn_ref(conn);
	auth_kind = kind;
	k_mutex_unlock(&lock);
	post(ev, value);
}

static void passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	auth_request(conn, AUTH_DISPLAY, BTUI_EV_DISPLAY, (int32_t)passkey);
}

static void passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	auth_request(conn, AUTH_PASSKEY_CONFIRM, BTUI_EV_CONFIRM, (int32_t)passkey);
}

static void pairing_confirm(struct bt_conn *conn)
{
	auth_request(conn, AUTH_PAIRING_CONFIRM, BTUI_EV_CONFIRM, -1);
}

static void auth_cancel(struct bt_conn *conn)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (conn == auth_conn) {
		auth_clear();
	}
	k_mutex_unlock(&lock);
	post(BTUI_EV_CANCEL, 0);
}

/* Display and yes/no make the phone DisplayYesNo */
static const struct bt_conn_auth_cb auth_cb = {
	.passkey_display = passkey_display,
	.passkey_confirm = passkey_confirm,
	.pairing_confirm = pairing_confirm,
	.cancel = auth_cancel,
};

/* A failed outgoing pairing leaves the link without a use */
static void pairing_end(struct bt_conn *conn, bool failed)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (conn == auth_conn) {
		auth_clear();
	}
	if (failed) {
		pair_done(conn, true);
	}
	k_mutex_unlock(&lock);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	if (!is_br(conn, NULL)) {
		return;
	}
	pairing_end(conn, false);
	post(BTUI_EV_PAIRED, bonded ? 1 : 0);
	post(BTUI_EV_BONDS, 0);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	if (!is_br(conn, NULL)) {
		return;
	}
	pairing_end(conn, true);
	post(BTUI_EV_PAIR_FAILED, reason);
}

static void bond_deleted(uint8_t id, const bt_addr_le_t *peer)
{
	ARG_UNUSED(id);
	ARG_UNUSED(peer);

	post(BTUI_EV_BONDS, 0);
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
	.bond_deleted = bond_deleted,
};

/*
 * Inquiring devices take the name from the Extended Inquiry Response, which
 * the Host leaves to the application, so it is rewritten with every name.
 */
static int write_eir(const char *name)
{
	const struct bt_data eir[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name)),
	};
	int err = bt_br_write_eir(eir, ARRAY_SIZE(eir), true);

	if (err != 0) {
		LOG_ERR("EIR write failed (%d)", err);
	}

	return err;
}

static void ready(int err)
{
	static bool registered;
	bt_addr_le_t addr;
	size_t count = 1;

	if ((err == 0) && !registered) {
		bt_br_discovery_cb_register(&discovery_cb);
		(void)bt_conn_auth_cb_register(&auth_cb);
		(void)bt_conn_auth_info_cb_register(&auth_info_cb);
		registered = true;
	}

	k_mutex_lock(&lock, K_FOREVER);
	info.err = err;
	info.state = (err == 0) ? BTUI_ON : BTUI_FAILED;
	if (err == 0) {
		bt_id_get(&addr, &count);
		if (count > 0U) {
			bt_addr_copy(&info.addr, &addr.a);
		}
		snprintf(info.name, sizeof(info.name), "%s", bt_get_name());
	}
	k_mutex_unlock(&lock);

	if (err != 0) {
		LOG_ERR("Bluetooth init failed (%d)", err);
	} else {
		(void)write_eir(bt_get_name());
		/* Reachable by paired devices, but not discoverable, until asked */
		(void)btui_set_connectable(true);
	}
	post(BTUI_EV_STATE, err);
}

/*
 * The controller has no address of its own, only a placeholder from its
 * firmware, so derive one from the device ID (the OMAP die ID). The ID is
 * folded into the six octets of a unicast address that is marked as locally
 * administered, since it comes from no IEEE-assigned block, and kept out of
 * the LAPs reserved for inquiry access codes. The Host forgets identities in
 * bt_disable(), so this runs before every enable.
 */
static void create_identity(void)
{
	bt_addr_le_t addr = {.type = BT_ADDR_LE_PUBLIC};
	uint8_t id[16];
	ssize_t len;
	uint32_t lap;
	int err;

	len = hwinfo_get_device_id(id, sizeof(id));
	if (len <= 0) {
		LOG_WRN("no device ID (%d), the controller keeps its address", (int)len);
		return;
	}

	for (ssize_t i = 0; i < len; i++) {
		addr.a.val[i % sizeof(addr.a.val)] ^= id[i];
	}

	/* The most significant octet is the last one */
	addr.a.val[5] = (uint8_t)((addr.a.val[5] & ~BIT(0)) | BIT(1));

	lap = sys_get_le24(addr.a.val);
	if ((lap >= 0x9E8B00U) && (lap <= 0x9E8B3FU)) {
		addr.a.val[2] ^= BIT(0);
	}

	err = bt_id_create(&addr, NULL);
	if ((err < 0) && (err != -EALREADY)) {
		LOG_WRN("public identity not created (%d)", err);
	}
}

int btui_enable(void)
{
	int err;

	k_mutex_lock(&lock, K_FOREVER);
	if ((info.state == BTUI_ON) || (info.state == BTUI_STARTING)) {
		k_mutex_unlock(&lock);
		return -EALREADY;
	}
	info.state = BTUI_STARTING;
	k_mutex_unlock(&lock);

	create_identity();
	err = bt_enable(ready);
	if (err != 0) {
		k_mutex_lock(&lock, K_FOREVER);
		info.state = BTUI_FAILED;
		info.err = err;
		k_mutex_unlock(&lock);
	}
	post(BTUI_EV_STATE, err);

	return err;
}

int btui_disable(void)
{
	int err;

	if (info.discovering) {
		(void)bt_br_discovery_stop();
	}

	err = bt_disable();
	if (err != 0) {
		LOG_ERR("Bluetooth disable failed (%d)", err);
		return err;
	}

	k_mutex_lock(&lock, K_FOREVER);
	info.state = BTUI_OFF;
	info.discoverable = false;
	info.connectable = false;
	info.discovering = false;
	info.connections = 0;
	auth_clear();
	if (pair_conn != NULL) {
		bt_conn_unref(pair_conn);
		pair_conn = NULL;
	}
	k_mutex_unlock(&lock);
	post(BTUI_EV_STATE, 0);

	return 0;
}

void btui_get_info(struct btui_info *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	*out = info;
	k_mutex_unlock(&lock);
}

int btui_set_connectable(bool on)
{
	int err = bt_br_set_connectable(on, NULL);

	if ((err != 0) && (err != -EALREADY)) {
		LOG_ERR("connectable %d failed (%d)", on, err);
		return err;
	}

	k_mutex_lock(&lock, K_FOREVER);
	info.connectable = on;
	if (!on) {
		/* Inquiry scan goes with page scan */
		info.discoverable = false;
	}
	k_mutex_unlock(&lock);
	post(BTUI_EV_STATE, 0);

	return 0;
}

int btui_set_discoverable(bool on)
{
	int err;

	if (on && !info.connectable) {
		err = btui_set_connectable(true);
		if (err != 0) {
			return err;
		}
	}

	err = bt_br_set_discoverable(on, false);
	if ((err != 0) && (err != -EALREADY)) {
		LOG_ERR("discoverable %d failed (%d)", on, err);
		return err;
	}

	k_mutex_lock(&lock, K_FOREVER);
	info.discoverable = on;
	k_mutex_unlock(&lock);
	post(BTUI_EV_STATE, 0);

	return 0;
}

int btui_set_name(const char *name)
{
	int err = bt_set_name(name);

	if (err != 0) {
		LOG_ERR("set name failed (%d)", err);
		return err;
	}

	err = write_eir(name);
	if (err != 0) {
		return err;
	}

	k_mutex_lock(&lock, K_FOREVER);
	snprintf(info.name, sizeof(info.name), "%s", bt_get_name());
	k_mutex_unlock(&lock);
	post(BTUI_EV_STATE, 0);

	return 0;
}

int btui_discover(void)
{
	static const struct bt_br_discovery_param param = {
		.length = DISCOVERY_LENGTH,
		.limited = false,
	};
	int err;

	k_mutex_lock(&lock, K_FOREVER);
	device_count = 0;
	k_mutex_unlock(&lock);

	err = bt_br_discovery_start(&param, results, ARRAY_SIZE(results));
	if (err != 0) {
		LOG_ERR("discovery failed (%d)", err);
		return err;
	}

	k_mutex_lock(&lock, K_FOREVER);
	info.discovering = true;
	k_mutex_unlock(&lock);
	post(BTUI_EV_DEVICES, 0);
	post(BTUI_EV_DISCOVERY, 1);

	return 0;
}

struct bond_list {
	struct btui_device *out;
	size_t max;
	size_t count;
};

static void add_bond(const struct bt_br_bond_info *bi, void *user_data)
{
	struct bond_list *list = user_data;
	struct btui_device *d;

	if (list->count == list->max) {
		return;
	}

	d = &list->out[list->count++];
	memset(d, 0, sizeof(*d));
	bt_addr_copy(&d->addr, &bi->addr);
	d->paired = true;
}

size_t btui_get_bonds(struct btui_device *bonds, size_t max)
{
	struct bond_list list = {.out = bonds, .max = max};

	if (info.state != BTUI_ON) {
		return 0;
	}

	bt_br_foreach_bond(add_bond, &list);

	k_mutex_lock(&lock, K_FOREVER);
	for (size_t i = 0; i < list.count; i++) {
		const struct btui_device *d = find_device(&bonds[i].addr);

		if (d != NULL) {
			memcpy(bonds[i].name, d->name, sizeof(bonds[i].name));
			bonds[i].cod = d->cod;
		}
	}
	k_mutex_unlock(&lock);

	return list.count;
}

size_t btui_get_devices(struct btui_device *out, size_t max)
{
	struct btui_device bonds[BTUI_MAX_BONDS];
	size_t bond_count = btui_get_bonds(bonds, ARRAY_SIZE(bonds));
	size_t n;

	k_mutex_lock(&lock, K_FOREVER);
	n = MIN(device_count, max);
	memcpy(out, devices, n * sizeof(*out));
	k_mutex_unlock(&lock);

	for (size_t i = 0; i < n; i++) {
		out[i].paired = false;
		for (size_t j = 0; j < bond_count; j++) {
			if (bt_addr_eq(&out[i].addr, &bonds[j].addr)) {
				out[i].paired = true;
			}
		}
	}

	return n;
}

int btui_pair(const bt_addr_t *addr)
{
	struct bt_conn *conn;
	int err;

	if (info.discovering) {
		(void)bt_br_discovery_stop();
		k_mutex_lock(&lock, K_FOREVER);
		info.discovering = false;
		k_mutex_unlock(&lock);
		post(BTUI_EV_DISCOVERY, 0);
	}

	conn = bt_conn_lookup_addr_br(addr);
	if (conn != NULL) {
		/* Already connected: just raise the security */
		err = bt_conn_set_security(conn, BT_SECURITY_L2);
		bt_conn_unref(conn);
		return err;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (pair_conn != NULL) {
		k_mutex_unlock(&lock);
		return -EBUSY;
	}
	conn = bt_conn_create_br(addr, BT_BR_CONN_PARAM_DEFAULT);
	if (conn == NULL) {
		k_mutex_unlock(&lock);
		return -EIO;
	}
	pair_conn = conn;
	k_mutex_unlock(&lock);

	return 0;
}

int btui_forget(const bt_addr_t *addr)
{
	int err = bt_br_unpair(addr);

	if (err != 0) {
		LOG_ERR("unpair failed (%d)", err);
		return err;
	}
	post(BTUI_EV_BONDS, 0);
	post(BTUI_EV_DEVICES, 0);

	return 0;
}

void btui_pending_peer(char *name, size_t len)
{
	const bt_addr_t *dst;

	k_mutex_lock(&lock, K_FOREVER);
	if ((auth_conn != NULL) && is_br(auth_conn, &dst)) {
		peer_name(dst, name, len);
	} else {
		snprintf(name, len, "a device");
	}
	k_mutex_unlock(&lock);
}

void btui_respond(bool accept)
{
	struct bt_conn *conn;
	enum auth_kind kind;

	k_mutex_lock(&lock, K_FOREVER);
	kind = auth_kind;
	if ((kind == AUTH_DISPLAY) && accept) {
		/* Dismissed: the pairing goes on until the peer finishes it */
		k_mutex_unlock(&lock);
		return;
	}
	conn = auth_conn;
	auth_conn = NULL;
	auth_kind = AUTH_NONE;
	k_mutex_unlock(&lock);

	if (conn == NULL) {
		return;
	}

	if (!accept) {
		(void)bt_conn_auth_cancel(conn);
	} else if (kind == AUTH_PASSKEY_CONFIRM) {
		(void)bt_conn_auth_passkey_confirm(conn);
	} else {
		(void)bt_conn_auth_pairing_confirm(conn);
	}
	bt_conn_unref(conn);
}
