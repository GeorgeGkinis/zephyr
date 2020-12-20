/*
 * Copyright (c) 2020 George Gkinis
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_SENSOR_HX711_H_
#define ZEPHYR_INCLUDE_DRIVERS_SENSOR_HX711_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <device.h>
#include <drivers/sensor.h>

enum hx711_gain {
	HX711_GAIN_128X = 1,
	HX711_GAIN_32X,
	HX711_GAIN_64X,
};

enum hx711_rate {
	HX711_RATE_10HZ,
	HX711_RATE_80HZ,
};

enum hx711_power {
	HX711_POWER_ON,
	HX711_POWER_OFF,
};

/**
 * @brief Zero the HX711.
 *
 * @param dev Pointer to the hx711 device structure
 * @param readings Number of readings to get average offset.
 *        5~10 readings should be enough, although more are allowed.
 * @retval The offset value
 *
 */
int avia_hx711_tare(const struct device *dev, uint8_t readings);

/**
 * @brief Callibrate the HX711.
 *
 * Given a target value of a known weight the slope gets calculated.
 * This is actually unit agnostic.
 * If the target weight is given in grams, lb, Kg or any other weight unit,
 * the slope will be calculated accordingly.
 *
 * @param dev Pointer to the hx711 device structure
 * @param target Target weight in grams.
 *        If target is represented in another unit (lb, oz, Kg) then the
 *        value returned by sensor_channel_get() will represent that unit.
 * @param readings Number of readings to take for calibration.
 *        5~10 readings should be enough, although more are allowed.
 * @retval The slope value
 *
 */
struct sensor_value avia_hx711_calibrate(const struct device *dev,
					 uint32_t target,
					 uint8_t readings);

/**
 * @brief Set the HX711 power.
 *
 * @param dev Pointer to the hx711 device structure
 * @param power one of HX711_POWER_OFF, HX711_POWER_ON
 * @retval The current power state or ENOTSUP if an invalid value pow is given
 *
 */
int avia_hx711_power(const struct device *dev, enum hx711_power power);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_SENSOR_HX711_H_ */