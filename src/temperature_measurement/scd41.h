#pragma once

#include <zephyr/device.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int scd41_wake_up(const struct device *i2c_dev, uint8_t addr);
int scd41_power_down(const struct device *i2c_dev, uint8_t addr);
int scd41_measure_single_shot(const struct device *i2c_dev, uint8_t addr);
int scd41_read_measurement(const struct device *i2c_dev, uint8_t addr,
                            uint16_t *co2_ppm, int32_t *temp_millideg_c,
                            uint16_t *humidity_milli_percent);

#ifdef __cplusplus
}
#endif