/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Wi-Fi bring-up: the network and Wi-Fi shells on the WL1251.
 *
 * A phone on the bench has no reset button, and one that hangs wants its
 * battery pulled. So that most mistakes can be found out from the shell
 * instead: a thread that faults is ended and reported while the rest lives
 * on, a thread that keeps the processor to itself is suspended and reported,
 * and what cannot be survived reboots into the bootloader.
 */

#include <zephyr/fatal.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(wifi_bringup, LOG_LEVEL_INF);

/* Without the lowest priority thread running for this long, something hogs */
#define HOG_SECONDS 5

static uint32_t heartbeat;
static uint32_t heartbeat_seen;
static uint8_t stalled;

static char fault_thread[32];
static unsigned int fault_reason;
static uint32_t fault_pc;
static uint32_t fault_lr;
static char hog_thread[32];

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	const char *name = k_thread_name_get(k_current_get());

	/*
	 * Nothing to return to from a panic or a fault in an interrupt
	 * handler. The exception this runs in counts as one level itself.
	 */
	if (reason == K_ERR_KERNEL_PANIC || arch_curr_cpu()->nested > 1U) {
		sys_reboot(SYS_REBOOT_COLD);
	}

	strncpy(fault_thread, name != NULL ? name : "?", sizeof(fault_thread) - 1);
	fault_reason = reason;
	if (esf != NULL) {
		fault_pc = esf->basic.pc;
		fault_lr = esf->basic.lr;
	}

	/* Returning ends the faulting thread only */
}

static void watchdog_expired(struct k_timer *timer)
{
	const char *name;

	ARG_UNUSED(timer);

	if (heartbeat != heartbeat_seen) {
		heartbeat_seen = heartbeat;
		stalled = 0;
		return;
	}

	if (++stalled < HOG_SECONDS) {
		return;
	}
	stalled = 0;

	/* A second hog, or one that cannot be stopped */
	if (hog_thread[0] != '\0') {
		sys_reboot(SYS_REBOOT_COLD);
	}

	/* The thread this interrupt came in on is the one that does not yield */
	name = k_thread_name_get(k_current_get());
	strncpy(hog_thread, name != NULL ? name : "?", sizeof(hog_thread) - 1);
	k_thread_suspend(k_current_get());
}

static K_TIMER_DEFINE(watchdog, watchdog_expired, NULL);

static void heartbeat_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		heartbeat++;
		k_msleep(200);
	}
}

K_THREAD_DEFINE(heartbeat_tid, 512, heartbeat_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

static int cmd_health(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (fault_thread[0] == '\0' && hog_thread[0] == '\0') {
		shell_print(sh, "No faults");
	}
	if (fault_thread[0] != '\0') {
		shell_print(sh, "Thread %s faulted: reason %u, pc 0x%08x, lr 0x%08x", fault_thread,
			    fault_reason, fault_pc, fault_lr);
	}
	if (hog_thread[0] != '\0') {
		shell_print(sh, "Thread %s did not yield and is suspended", hog_thread);
	}

	return 0;
}

SHELL_CMD_REGISTER(health, NULL, "Faults since boot", cmd_health);

/* Early, to cover the rest of the start-up too */
static int watchdog_start(void)
{
	k_timer_start(&watchdog, K_SECONDS(1), K_SECONDS(1));

	return 0;
}

SYS_INIT(watchdog_start, POST_KERNEL, 0);

int main(void)
{
	return 0;
}
