#pragma once

#include <zephyr/device.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int scd41_wake_up(const struct device *i2c_dev, uint8_t addr);
int scd41_stop_periodic_measurement(const struct device *i2c_dev, uint8_t addr);
int scd41_power_down(const struct device *i2c_dev, uint8_t addr);
int scd41_measure_single_shot(const struct device *i2c_dev, uint8_t addr);
int scd41_read_measurement(const struct device *i2c_dev, uint8_t addr,
                            uint16_t *co2_ppm, int32_t *temp_millideg_c,
                            uint16_t *humidity_milli_percent);

/* Same as scd41_read_measurement(), but also returns the raw (pre-conversion)
 * 16-bit sensor words for debugging - pass NULL for any value you don't need. */
int scd41_read_measurement_raw(const struct device *i2c_dev, uint8_t addr,
                                uint16_t *co2_ppm, int32_t *temp_millideg_c,
                                uint16_t *humidity_milli_percent,
                                uint16_t *co2_raw_out, uint16_t *temp_raw_out,
                                uint16_t *rh_raw_out);

#ifdef __cplusplus
}
#endif