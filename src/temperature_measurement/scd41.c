/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "scd41.h"

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

/* SCD4x command set (see Sensirion SCD4x datasheet, section 3) */
#define SCD41_CMD_WAKE_UP		0x36F6
#define SCD41_CMD_POWER_DOWN		0x36E0
#define SCD41_CMD_STOP_PERIODIC		0x3F86
#define SCD41_CMD_MEASURE_SINGLE_SHOT	0x219D
#define SCD41_CMD_READ_MEASUREMENT	0xEC05

/* Execution / settling times in ms */
#define SCD41_WAKE_UP_DELAY_MS		30
#define SCD41_POWER_DOWN_DELAY_MS	1
#define SCD41_STOP_PERIODIC_DELAY_MS	500
#define SCD41_SINGLE_SHOT_DELAY_MS	5000
#define SCD41_READ_MEASUREMENT_DELAY_MS	1

#define SCD41_CRC8_POLYNOMIAL		0x31
#define SCD41_CRC8_INIT			0xFF

static uint8_t scd41_crc8(const uint8_t *data, size_t len)
{
	uint8_t crc = SCD41_CRC8_INIT;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (uint8_t bit = 0; bit < 8; bit++) {
			if (crc & 0x80) {
				crc = (uint8_t)((crc << 1) ^ SCD41_CRC8_POLYNOMIAL);
			} else {
				crc = (uint8_t)(crc << 1);
			}
		}
	}

	return crc;
}

static int scd41_send_command(const struct device *i2c_dev, uint8_t addr, uint16_t cmd)
{
	uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };

	return i2c_write(i2c_dev, buf, sizeof(buf), addr);
}

int scd41_wake_up(const struct device *i2c_dev, uint8_t addr)
{
	/* The sensor does not acknowledge the wake_up command, so the NACK
	 * (-EIO) returned here is expected and must be ignored. */
	(void)scd41_send_command(i2c_dev, addr, SCD41_CMD_WAKE_UP);
	k_msleep(SCD41_WAKE_UP_DELAY_MS);

	return 0;
}

int scd41_power_down(const struct device *i2c_dev, uint8_t addr)
{
	int ret = scd41_send_command(i2c_dev, addr, SCD41_CMD_POWER_DOWN);

	k_msleep(SCD41_POWER_DOWN_DELAY_MS);

	return ret;
}

int scd41_measure_single_shot(const struct device *i2c_dev, uint8_t addr)
{
	int ret = scd41_send_command(i2c_dev, addr, SCD41_CMD_MEASURE_SINGLE_SHOT);

	if (ret != 0) {
		return ret;
	}

	/* Single-shot measurement (CO2 + RH + T) takes about 5 s to complete. */
	k_msleep(SCD41_SINGLE_SHOT_DELAY_MS);

	return 0;
}

int scd41_read_measurement(const struct device *i2c_dev, uint8_t addr,
			   uint16_t *co2_ppm, int32_t *temp_millideg_c,
			   uint16_t *humidity_milli_percent)
{
	uint8_t rx[9];
	int ret;

	ret = scd41_send_command(i2c_dev, addr, SCD41_CMD_READ_MEASUREMENT);
	if (ret != 0) {
		return ret;
	}

	k_msleep(SCD41_READ_MEASUREMENT_DELAY_MS);

	ret = i2c_read(i2c_dev, rx, sizeof(rx), addr);
	if (ret != 0) {
		return ret;
	}

	/* Each 16-bit word is followed by its CRC8 byte. */
	for (int i = 0; i < 9; i += 3) {
		if (scd41_crc8(&rx[i], 2) != rx[i + 2]) {
			return -EIO;
		}
	}

	uint16_t co2_raw = (uint16_t)((rx[0] << 8) | rx[1]);
	uint16_t temp_raw = (uint16_t)((rx[3] << 8) | rx[4]);
	uint16_t rh_raw = (uint16_t)((rx[6] << 8) | rx[7]);

	if (co2_ppm) {
		*co2_ppm = co2_raw;
	}

	if (temp_millideg_c) {
		/* T [m°C] = -45000 + 175000 * raw / 2^16 */
		*temp_millideg_c = -45000 + (int32_t)((175000ULL * temp_raw) >> 16);
	}

	if (humidity_milli_percent) {
		/* RH [m%] = 100000 * raw / 2^16 */
		*humidity_milli_percent = (uint16_t)((100000ULL * rh_raw) >> 16);
	}

	return 0;
}
