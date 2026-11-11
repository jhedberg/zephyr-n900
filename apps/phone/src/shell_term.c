/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Shell backend of the phone demo's terminal: the shell writes into the
 * terminal grid, which takes everything at once, and reads what the UI
 * queues from the keyboard. Log messages come out on it as well.
 */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

#include "phone.h"
#include "term.h"

#define LOG_QUEUE_SIZE 16
#define LOG_TIMEOUT_MS 100

struct term_transport {
	shell_transport_handler_t handler;
	void *context;
};

static struct term_transport term_transport;
static struct k_spinlock rx_lock;
static atomic_t posted;
RING_BUF_DECLARE(rx_ring, 64);

static int transport_init(const struct shell_transport *transport, const void *config,
			  shell_transport_handler_t evt_handler, void *context)
{
	struct term_transport *t = transport->ctx;

	ARG_UNUSED(config);

	t->handler = evt_handler;
	t->context = context;

	return 0;
}

static int transport_uninit(const struct shell_transport *transport)
{
	ARG_UNUSED(transport);

	return 0;
}

static int transport_enable(const struct shell_transport *transport, bool blocking_tx)
{
	ARG_UNUSED(transport);
	ARG_UNUSED(blocking_tx);

	return 0;
}

static int transport_write(const struct shell_transport *transport, const void *data,
			   size_t length, size_t *cnt)
{
	struct term_transport *t = transport->ctx;

	term_write(data, length);
	*cnt = length;

	if (!atomic_set(&posted, 1)) {
		phone_post(PEV_TERM, 0, 0);
	}

	t->handler(SHELL_TRANSPORT_EVT_TX_RDY, t->context);

	return 0;
}

static int transport_read(const struct shell_transport *transport, void *data, size_t length,
			  size_t *cnt)
{
	k_spinlock_key_t key = k_spin_lock(&rx_lock);

	ARG_UNUSED(transport);

	*cnt = ring_buf_get(&rx_ring, data, length);
	k_spin_unlock(&rx_lock, key);

	return 0;
}

static const struct shell_transport_api transport_api = {
	.init = transport_init,
	.uninit = transport_uninit,
	.enable = transport_enable,
	.write = transport_write,
	.read = transport_read,
};

static struct shell_transport transport = {
	.api = &transport_api,
	.ctx = &term_transport,
};

SHELL_DEFINE(shell_term, "n900:~$ ", &transport, LOG_QUEUE_SIZE, LOG_TIMEOUT_MS,
	     SHELL_FLAG_OLF_CRLF);

void term_shell_input(const char *data, size_t len)
{
	k_spinlock_key_t key = k_spin_lock(&rx_lock);
	uint32_t put = ring_buf_put(&rx_ring, data, len);

	k_spin_unlock(&rx_lock, key);

	if ((put > 0U) && (term_transport.handler != NULL)) {
		term_transport.handler(SHELL_TRANSPORT_EVT_RX_RDY, term_transport.context);
	}
}

void term_shell_ack(void)
{
	atomic_clear(&posted);
}

static int shell_term_init(void)
{
	static const struct shell_backend_config_flags cfg_flags =
		SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;

	/* The answers to the shell's queries, such as the cursor position */
	term_init(term_shell_input);

	return shell_init(&shell_term, NULL, cfg_flags, true, LOG_LEVEL_INF);
}

SYS_INIT(shell_term_init, APPLICATION, 0);
