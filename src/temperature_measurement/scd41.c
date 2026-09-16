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
#define SCD41_CMD_GET_DATA_READY	0xE4B8

/* Execution / settling times in ms */
#define SCD41_WAKE_UP_DELAY_MS		30
#define SCD41_POWER_DOWN_DELAY_MS	1
#define SCD41_STOP_PERIODIC_DELAY_MS	500
#define SCD41_SINGLE_SHOT_DELAY_MS	5000
#define SCD41_READ_MEASUREMENT_DELAY_MS	1
#define SCD41_DATA_READY_POLL_DELAY_MS	50
#define SCD41_DATA_READY_MAX_RETRIES	20 /* 20 * 50ms = 1s extra margin */

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

int scd41_stop_periodic_measurement(const struct device *i2c_dev, uint8_t addr)
{
	int ret = scd41_send_command(i2c_dev, addr, SCD41_CMD_STOP_PERIODIC);

	k_msleep(SCD41_STOP_PERIODIC_DELAY_MS);

	return ret;
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

/* Returns 1 if a fresh measurement is ready, 0 if not yet, <0 on I2C/CRC error. */
static int scd41_data_ready(const struct device *i2c_dev, uint8_t addr)
{
	uint8_t rx[3];
	int ret = scd41_send_command(i2c_dev, addr, SCD41_CMD_GET_DATA_READY);

	if (ret != 0) {
		return ret;
	}

	k_msleep(SCD41_READ_MEASUREMENT_DELAY_MS);

	ret = i2c_read(i2c_dev, rx, sizeof(rx), addr);
	if (ret != 0) {
		return ret;
	}

	if (scd41_crc8(rx, 2) != rx[2]) {
		return -EIO;
	}

	uint16_t status = (uint16_t)((rx[0] << 8) | rx[1]);

	/* Per datasheet: not ready if the low 11 bits of the status word are 0. */
	return (status & 0x07FF) != 0;
}

/* The fixed SCD41_SINGLE_SHOT_DELAY_MS wait in scd41_measure_single_shot() is
 * the sensor's typical, not guaranteed, completion time. Reading before the
 * measurement is actually ready makes the sensor return its last/stale
 * measurement buffer instead - which still carries a self-consistent, valid
 * CRC (computed over whatever is in the buffer), so the corruption is
 * invisible to the CRC check in scd41_read_measurement_raw() and shows up
 * only as plausible-looking but wrong values (relative humidity near 0%
 * being the one most visibly affected). Poll get_data_ready_status for a bit
 * of extra margin before reading instead of trusting the fixed delay alone. */
static void scd41_wait_data_ready(const struct device *i2c_dev, uint8_t addr)
{
	for (int i = 0; i < SCD41_DATA_READY_MAX_RETRIES; i++) {
		int ready = scd41_data_ready(i2c_dev, addr);
		if (ready != 0) {
			/* Ready (1), or an error (<0) we can't resolve by waiting longer. */
			return;
		}
		k_msleep(SCD41_DATA_READY_POLL_DELAY_MS);
	}
}

int scd41_read_measurement_raw(const struct device *i2c_dev, uint8_t addr,
			       uint16_t *co2_ppm, int32_t *temp_millideg_c,
			       uint16_t *humidity_milli_percent,
			       uint16_t *co2_raw_out, uint16_t *temp_raw_out,
			       uint16_t *rh_raw_out)
{
	uint8_t rx[9];
	int ret;

	scd41_wait_data_ready(i2c_dev, addr);

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

	if (co2_raw_out) {
		*co2_raw_out = co2_raw;
	}
	if (temp_raw_out) {
		*temp_raw_out = temp_raw;
	}
	if (rh_raw_out) {
		*rh_raw_out = rh_raw;
	}

	return 0;
}

int scd41_read_measurement(const struct device *i2c_dev, uint8_t addr,
			   uint16_t *co2_ppm, int32_t *temp_millideg_c,
			   uint16_t *humidity_milli_percent)
{
	return scd41_read_measurement_raw(i2c_dev, addr, co2_ppm, temp_millideg_c, humidity_milli_percent,
					   NULL, NULL, NULL);
}
