/*
 * Copyright The Zephyr Project Contributors
 * Copyright (c) 2026 Silicon Laboratories Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * TI TSC2005 resistive touchscreen controller on SPI. In pen mode the chip
 * converts on its own while the panel is touched and raises its data
 * available line after each set of X, Y, Z1 and Z2 samples; the driver reads
 * them on that edge and reports a release when the samples stop.
 */

#define DT_DRV_COMPAT ti_tsc2005

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_touch.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <string.h>

LOG_MODULE_REGISTER(tsc2005, CONFIG_INPUT_LOG_LEVEL);

/* Command byte */
#define CMD        0x80U
#define CMD_NORMAL 0x00U
#define CMD_STOP   0x01U
#define CMD_12BIT  0x04U

/* Register access byte: address in bits 6:3 */
#define REG_READ 0x01U
#define REG_PND0 0x02U
#define REG(n)   ((n) << 3)
#define REG_X    REG(0x0U)
#define REG_CFR0 REG(0xCU)
#define REG_CFR1 REG(0xDU)
#define REG_CFR2 REG(0xEU)

/* Pen mode, 12-bit, 1 MHz conversion clock, 276 us precharge, 1 ms settling */
#define CFR0_INIT 0xB340U
/* 4 ms between conversion batches */
#define CFR1_INIT 0x0003U
/* Median of 15 then average of 7, on X, Y and Z */
#define CFR2_INIT 0x381CU

#define MAX_12BIT 0xFFFU

/* No samples for this long after the last one means the pen is up */
#define PEN_UP_TIMEOUT K_MSEC(40)

#define RESET_PULSE_US 100U

struct tsc2005_config {
	struct input_touchscreen_common_config common;
	struct spi_dt_spec bus;
	struct gpio_dt_spec int_gpio;
	struct gpio_dt_spec reset_gpio;
	uint16_t x_plate_ohms;
	uint16_t min_x;
	uint16_t max_x;
	uint16_t min_y;
	uint16_t max_y;
	uint16_t max_pressure;
};

struct tsc2005_data {
	const struct device *dev;
	struct gpio_callback int_cb;
	struct k_work sample_work;
	struct k_work_delayable pen_up_work;
	uint16_t last[4];
	bool pressed;
};

INPUT_TOUCH_STRUCT_CHECK(struct tsc2005_config);

static int tsc2005_cmd(const struct device *dev, uint8_t cmd)
{
	const struct tsc2005_config *config = dev->config;
	uint8_t byte = CMD | CMD_12BIT | cmd;
	const struct spi_buf buf = {.buf = &byte, .len = sizeof(byte)};
	const struct spi_buf_set tx = {.buffers = &buf, .count = 1};

	return spi_write_dt(&config->bus, &tx);
}

static int tsc2005_reg_write(const struct device *dev, uint8_t reg, uint16_t val)
{
	const struct tsc2005_config *config = dev->config;
	uint8_t frame[3] = {reg | REG_PND0};
	const struct spi_buf buf = {.buf = frame, .len = sizeof(frame)};
	const struct spi_buf_set tx = {.buffers = &buf, .count = 1};

	sys_put_be16(val, &frame[1]);

	return spi_write_dt(&config->bus, &tx);
}

/* The chip returns one register per transfer */
static int tsc2005_reg_read(const struct device *dev, uint8_t reg, uint16_t *val)
{
	const struct tsc2005_config *config = dev->config;
	uint8_t frame[3] = {reg | REG_READ};
	const struct spi_buf tx_buf = {.buf = frame, .len = sizeof(frame)};
	const struct spi_buf rx_buf = {.buf = frame, .len = sizeof(frame)};
	const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
	const struct spi_buf_set rx = {.buffers = &rx_buf, .count = 1};
	int ret;

	ret = spi_transceive_dt(&config->bus, &tx, &rx);
	if (ret != 0) {
		return ret;
	}

	*val = sys_get_be16(&frame[1]);

	return 0;
}

static uint32_t scale(uint16_t raw, uint16_t min, uint16_t max, uint32_t size)
{
	raw = CLAMP(raw, min, max);

	return ((uint32_t)(raw - min) * (size - 1U)) / (uint32_t)(max - min);
}

static void sample_handler(struct k_work *work)
{
	struct tsc2005_data *data = CONTAINER_OF(work, struct tsc2005_data, sample_work);
	const struct device *dev = data->dev;
	const struct tsc2005_config *config = dev->config;
	uint16_t sample[4];
	uint16_t x;
	uint16_t y;
	uint16_t z1;
	uint16_t z2;
	uint32_t pressure;

	/* X, Y, Z1 and Z2 are consecutive registers */
	for (uint8_t i = 0U; i < ARRAY_SIZE(sample); i++) {
		if (tsc2005_reg_read(dev, REG_X + REG(i), &sample[i]) != 0) {
			return;
		}
	}

	x = sample[0];
	y = sample[1];
	z1 = sample[2];
	z2 = sample[3];

	LOG_DBG("x %u y %u z1 %u z2 %u", x, y, z1, z2);

	if ((x > MAX_12BIT) || (y > MAX_12BIT) || (z1 == 0U) || (z2 > MAX_12BIT) || (z1 >= z2)) {
		return;
	}

	/*
	 * At pen down the registers can still hold the samples from before the
	 * last pen up, which would press wherever the previous touch was.
	 */
	if (!data->pressed && (memcmp(sample, data->last, sizeof(sample)) == 0)) {
		LOG_DBG("stale sample");
		return;
	}
	memcpy(data->last, sample, sizeof(sample));

	/* Touch resistance, equation 1 of the datasheet */
	pressure = (uint32_t)x * (z2 - z1) / z1;
	pressure = pressure * config->x_plate_ohms / 4096U;
	if (pressure > config->max_pressure) {
		return;
	}

	input_touchscreen_report_pos(
		dev, scale(x, config->min_x, config->max_x, config->common.screen_width),
		scale(y, config->min_y, config->max_y, config->common.screen_height), K_FOREVER);
	input_report_key(dev, INPUT_BTN_TOUCH, 1, true, K_FOREVER);
	data->pressed = true;

	k_work_reschedule(&data->pen_up_work, PEN_UP_TIMEOUT);
}

static void pen_up_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct tsc2005_data *data = CONTAINER_OF(dwork, struct tsc2005_data, pen_up_work);

	if (data->pressed) {
		data->pressed = false;
		input_report_key(data->dev, INPUT_BTN_TOUCH, 0, true, K_FOREVER);
	}
}

static void int_handler(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	struct tsc2005_data *data = CONTAINER_OF(cb, struct tsc2005_data, int_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	k_work_submit(&data->sample_work);
}

static int tsc2005_init(const struct device *dev)
{
	const struct tsc2005_config *config = dev->config;
	struct tsc2005_data *data = dev->data;
	const uint16_t cfr[][2] = {
		{REG_CFR0, CFR0_INIT},
		{REG_CFR1, CFR1_INIT},
		{REG_CFR2, CFR2_INIT},
	};
	int ret;

	if (!spi_is_ready_dt(&config->bus) || !gpio_is_ready_dt(&config->int_gpio)) {
		return -ENODEV;
	}

	if (config->reset_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&config->reset_gpio)) {
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret != 0) {
			return ret;
		}
		k_busy_wait(RESET_PULSE_US);
		ret = gpio_pin_set_dt(&config->reset_gpio, 0);
		if (ret != 0) {
			return ret;
		}
		k_busy_wait(RESET_PULSE_US);
	}

	data->dev = dev;
	k_work_init(&data->sample_work, sample_handler);
	k_work_init_delayable(&data->pen_up_work, pen_up_handler);

	ret = gpio_pin_configure_dt(&config->int_gpio, GPIO_INPUT);
	if (ret != 0) {
		return ret;
	}

	gpio_init_callback(&data->int_cb, int_handler, BIT(config->int_gpio.pin));
	ret = gpio_add_callback_dt(&config->int_gpio, &data->int_cb);
	if (ret != 0) {
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&config->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		return ret;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(cfr); i++) {
		ret = tsc2005_reg_write(dev, cfr[i][0], cfr[i][1]);
		if (ret != 0) {
			LOG_ERR("configuration failed (%d)", ret);
			return ret;
		}
	}

	return tsc2005_cmd(dev, CMD_NORMAL);
}

#define TSC2005_INIT(inst)                                                                         \
	BUILD_ASSERT(DT_INST_PROP(inst, min_x) < DT_INST_PROP(inst, max_x), "min-x >= max-x");     \
	BUILD_ASSERT(DT_INST_PROP(inst, min_y) < DT_INST_PROP(inst, max_y), "min-y >= max-y");     \
	BUILD_ASSERT(DT_INST_PROP(inst, screen_width) > 0, "screen-width is required");            \
	BUILD_ASSERT(DT_INST_PROP(inst, screen_height) > 0, "screen-height is required");          \
	static const struct tsc2005_config tsc2005_config_##inst = {                               \
		.common = INPUT_TOUCH_DT_INST_COMMON_CONFIG_INIT(inst),                            \
		.bus = SPI_DT_SPEC_INST_GET(inst, SPI_OP_MODE_CONTROLLER | SPI_TRANSFER_MSB |      \
							  SPI_WORD_SET(8)),                        \
		.int_gpio = GPIO_DT_SPEC_INST_GET(inst, int_gpios),                                \
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}),                    \
		.x_plate_ohms = DT_INST_PROP(inst, ti_x_plate_ohms),                               \
		.min_x = DT_INST_PROP(inst, min_x),                                                \
		.max_x = DT_INST_PROP(inst, max_x),                                                \
		.min_y = DT_INST_PROP(inst, min_y),                                                \
		.max_y = DT_INST_PROP(inst, max_y),                                                \
		.max_pressure = DT_INST_PROP(inst, max_pressure),                                  \
	};                                                                                         \
	static struct tsc2005_data tsc2005_data_##inst;                                            \
	DEVICE_DT_INST_DEFINE(inst, tsc2005_init, NULL, &tsc2005_data_##inst,                      \
			      &tsc2005_config_##inst, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY,     \
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(TSC2005_INIT)
