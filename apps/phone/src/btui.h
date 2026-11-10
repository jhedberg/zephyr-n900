/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/* N900 phone demo: Bluetooth BR/EDR state for the UI */
#ifndef BTUI_H_
#define BTUI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/addr.h>

#define BTUI_MAX_DEVICES 16
#define BTUI_MAX_BONDS   8
#define BTUI_NAME_LEN    32

enum btui_state {
	BTUI_OFF,
	BTUI_STARTING,
	BTUI_ON,
	BTUI_FAILED,
};

/* Codes of PEV_BT events, posted with phone_post() from Bluetooth threads */
enum btui_event {
	BTUI_EV_STATE,        /* enabled, disabled or failed; value: error */
	BTUI_EV_DEVICES,      /* the discovered devices changed */
	BTUI_EV_DISCOVERY,    /* discovery started (value 1) or ended (value 0) */
	BTUI_EV_BONDS,        /* the paired devices changed */
	BTUI_EV_CONN,         /* the number of connections changed */
	BTUI_EV_CONFIRM,      /* pairing needs a yes/no; value: passkey, or -1 */
	BTUI_EV_DISPLAY,      /* show a passkey to type on the peer; value: passkey */
	BTUI_EV_CANCEL,       /* the pending pairing request went away */
	BTUI_EV_PAIRED,       /* pairing completed; value: bonded */
	BTUI_EV_PAIR_FAILED,  /* value: enum bt_security_err */
};

struct btui_info {
	enum btui_state state;
	int err;
	bt_addr_t addr;
	char name[BTUI_NAME_LEN];
	bool discoverable;
	bool connectable;
	bool discovering;
	uint8_t connections;
};

struct btui_device {
	bt_addr_t addr;
	char name[BTUI_NAME_LEN];
	uint32_t cod;
	int8_t rssi;
	bool paired;
};

/* All functions below are for the LVGL thread */
int btui_enable(void);
int btui_disable(void);
void btui_get_info(struct btui_info *info);

int btui_set_connectable(bool on);
int btui_set_discoverable(bool on);
int btui_set_name(const char *name);

int btui_discover(void);
size_t btui_get_devices(struct btui_device *devices, size_t max);
size_t btui_get_bonds(struct btui_device *bonds, size_t max);

int btui_pair(const bt_addr_t *addr);
int btui_forget(const bt_addr_t *addr);

/* Name of the peer of the pending pairing request, for dialogs */
void btui_pending_peer(char *name, size_t len);
/* Answers the pending CONFIRM, or cancels a pending DISPLAY when accept is false */
void btui_respond(bool accept);

#endif /* BTUI_H_ */
