/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Entropy bring-up: a line of random bytes from the entropy device every
 * second, with the time it took to get them.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kernel.h>

static const struct device *const entropy = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));

int main(void)
{
	uint8_t buf[32];

	printk("N900 entropy: %s\n", device_is_ready(entropy) ? "ready" : "NOT READY");

	while (true) {
		uint32_t start = k_cycle_get_32();
		int ret = entropy_get_entropy(entropy, buf, sizeof(buf));
		uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - start);

		if (ret != 0) {
			printk("error %d\n", ret);
		} else {
			for (size_t i = 0; i < sizeof(buf); i++) {
				printk("%02x", buf[i]);
			}
			printk("  %u us\n", us);
		}
		(void)k_msleep(1000);
	}

	return 0;
}
