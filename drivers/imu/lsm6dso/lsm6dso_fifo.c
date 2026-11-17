/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/drivers/imu/lsm6dso/lsm6dso_fifo.h>

#include <string.h>

size_t lsm6dso_fifo_split(uint8_t *words, size_t num_words, uint8_t *gyro, size_t gyro_max,
                          size_t *num_gyro) {
  size_t n_accel = 0, n_gyro = 0;
  for (size_t i = 0; i < num_words; i++) {
    uint8_t *w = &words[i * LSM6DSO_FIFO_WORD_BYTES];
    const uint8_t tag = w[0] >> 3;
    if (tag == LSM6DSO_FIFO_TAG_ACCEL_NC) {
      if (n_accel != i) {
        memmove(&words[n_accel * LSM6DSO_FIFO_WORD_BYTES], w, LSM6DSO_FIFO_WORD_BYTES);
      }
      n_accel++;
    } else if (tag == LSM6DSO_FIFO_TAG_GYRO_NC && gyro && n_gyro < gyro_max) {
      memcpy(&gyro[n_gyro * 6], &w[1], 6);
      n_gyro++;
    }
  }
  if (num_gyro) {
    *num_gyro = n_gyro;
  }
  return n_accel;
}
