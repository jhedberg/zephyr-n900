/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Hardware random number generator of the OMAP3 in the Nokia N900. The N900
 * is a high-security device, where the normal world has no access to the RNG
 * module: random numbers come from a service of the secure world instead,
 * four bytes per call.
 */

#define DT_DRV_COMPAT nokia_n900_rom_rng

#include <errno.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <soc.h>

LOG_MODULE_REGISTER(entropy_omap3_rom, CONFIG_ENTROPY_LOG_LEVEL);

/* The RNG module has an interface clock only */
#define CM_ICLKEN2_CORE 0x48004A14U
#define CM_IDLEST2_CORE 0x48004A24U
#define CORE_EN_RNG     BIT(2)

/* Bounded so that a module that never becomes ready cannot hang the boot */
#define IDLEST_POLL_LIMIT 100000U

/* The RNG service and its operations */
#define SERVICE_HWRNG        29U
#define RNG_GEN_PRNG_HW_INIT 0x02U
#define RNG_GEN_HW           0x08U

/* Data cache line of the Cortex-A8 */
#define CACHE_LINE_SIZE 64U

/*
 * The secure side reads the parameters from memory and writes the random
 * word there, by physical address and not necessarily through the data cache
 * of the normal world. They have a cache line of their own, which leaves the
 * cache around every call.
 */
static struct {
	/* One more than the number of parameters, which the N900 expects */
	uint32_t count;
	uint32_t ptr;
	uint32_t len;
	uint32_t op;
	uint32_t unused;
	uint32_t word;
} __aligned(CACHE_LINE_SIZE) rng;

BUILD_ASSERT(sizeof(rng) == CACHE_LINE_SIZE);

static int rom_rng_call(uint32_t op, uint32_t *word)
{
	unsigned int key = irq_lock();
	uint32_t ret;

	/* Memory is mapped at its physical address */
	rng.count = 3U + 1U;
	rng.ptr = (word != NULL) ? (uint32_t)&rng.word : 0U;
	rng.len = (word != NULL) ? sizeof(rng.word) : 0U;
	rng.op = op;
	rng.unused = 0U;

	(void)sys_cache_data_flush_and_invd_range(&rng, sizeof(rng));
	ret = omap3_secure_call(SERVICE_HWRNG, 0U, 0U, (uint32_t)&rng);
	(void)sys_cache_data_flush_and_invd_range(&rng, sizeof(rng));

	if (word != NULL) {
		*word = rng.word;
		rng.word = 0U;
	}

	irq_unlock(key);

	return (ret == 0U) ? 0 : -EIO;
}

static int rom_rng_get_entropy(const struct device *dev, uint8_t *buffer, uint16_t length)
{
	ARG_UNUSED(dev);

	while (length > 0U) {
		size_t len = MIN(length, sizeof(uint32_t));
		uint32_t word;
		int ret;

		ret = rom_rng_call(RNG_GEN_HW, &word);
		if (ret < 0) {
			return ret;
		}

		(void)memcpy(buffer, &word, len);
		buffer += len;
		length -= len;
	}

	return 0;
}

/* The service never waits for data, so ENTROPY_BUSYWAIT makes no difference */
static int rom_rng_get_entropy_isr(const struct device *dev, uint8_t *buffer, uint16_t length,
				   uint32_t flags)
{
	int ret;

	ARG_UNUSED(flags);

	ret = rom_rng_get_entropy(dev, buffer, length);

	return (ret < 0) ? ret : length;
}

static int rom_rng_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	sys_set_bits(CM_ICLKEN2_CORE, CORE_EN_RNG);
	for (uint32_t i = 0U; i < IDLEST_POLL_LIMIT; i++) {
		if ((sys_read32(CM_IDLEST2_CORE) & CORE_EN_RNG) == 0U) {
			break;
		}
	}

	if (rom_rng_call(RNG_GEN_PRNG_HW_INIT, NULL) < 0) {
		LOG_ERR("RNG initialization failed");
		return -EIO;
	}

	return 0;
}

static DEVICE_API(entropy, rom_rng_api) = {
	.get_entropy = rom_rng_get_entropy,
	.get_entropy_isr = rom_rng_get_entropy_isr,
};

DEVICE_DT_INST_DEFINE(0, rom_rng_init, NULL, NULL, NULL, PRE_KERNEL_1, CONFIG_ENTROPY_INIT_PRIORITY,
		      &rom_rng_api);
