/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! Gyroscope: minimal kernel interface (the Pebble Time 2's LSM6DSO has one; the stock firmware
//! leaves it powered down).
//!
//! The gyroscope runs at the accelerometer's rate, batched in the same FIFO, so it costs nothing
//! while off and is turned on by its kernel client only when needed. Samples are delivered to
//! pbl_gyro_samples_cb() from the IMU driver's work context.

#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief One gyroscope sample, in 0.1 degree per second, board axes (as the accelerometer). */
typedef struct {
  int16_t x, y, z;
} GyroSample;

#if defined(CONFIG_ACCEL_LSM6DSO)
/**
 * @brief Power the gyroscope on or off.
 *
 * Takes effect while the accelerometer streams (the gyroscope is batched alongside it).
 *
 * @return 0, or a negative errno.
 */
int pbl_gyro_set_enabled(bool enable);
#else
static inline int pbl_gyro_set_enabled(bool enable) {
  return -ENODEV;
}
#endif

/**
 * @brief Receives gyroscope samples (implemented by the client).
 *
 * @param samples Samples, oldest first.
 * @param num_samples Number of samples.
 */
void pbl_gyro_samples_cb(const GyroSample *samples, size_t num_samples);
