/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <clar.h>

#include <string.h>

#include <pbl/drivers/imu/lsm6dso/lsm6dso_fifo.h>

static void prv_word(uint8_t *w, uint8_t tag, uint8_t fill) {
  w[0] = (uint8_t)(tag << 3) | 0x2;  // TAG_CNT / parity bits in 2:0 are ignored
  for (int i = 1; i < LSM6DSO_FIFO_WORD_BYTES; i++) {
    w[i] = (uint8_t)(fill + i);
  }
}

void test_lsm6dso_fifo__accel_only_is_unchanged(void) {
  uint8_t words[4 * LSM6DSO_FIFO_WORD_BYTES], copy[sizeof(words)];
  for (int i = 0; i < 4; i++) {
    prv_word(&words[i * LSM6DSO_FIFO_WORD_BYTES], LSM6DSO_FIFO_TAG_ACCEL_NC, (uint8_t)(i * 10));
  }
  memcpy(copy, words, sizeof(words));
  size_t ng = 99;
  cl_assert_equal_i(lsm6dso_fifo_split(words, 4, NULL, 0, &ng), 4);
  cl_assert_equal_i(ng, 0);
  cl_assert(memcmp(words, copy, sizeof(words)) == 0);
}

void test_lsm6dso_fifo__interleaved_words_are_split(void) {
  // A G A G T(timestamp) A  -> accel A0 A1 A2 in order, gyro G0 G1
  const uint8_t tags[] = {LSM6DSO_FIFO_TAG_ACCEL_NC, LSM6DSO_FIFO_TAG_GYRO_NC,
                          LSM6DSO_FIFO_TAG_ACCEL_NC, LSM6DSO_FIFO_TAG_GYRO_NC, 0x04,
                          LSM6DSO_FIFO_TAG_ACCEL_NC};
  const uint8_t fills[] = {10, 100, 20, 110, 200, 30};
  uint8_t words[6 * LSM6DSO_FIFO_WORD_BYTES];
  for (int i = 0; i < 6; i++) {
    prv_word(&words[i * LSM6DSO_FIFO_WORD_BYTES], tags[i], fills[i]);
  }
  uint8_t gyro[4 * 6];
  size_t ng = 0;
  const size_t na = lsm6dso_fifo_split(words, 6, gyro, 4, &ng);
  cl_assert_equal_i(na, 3);
  cl_assert_equal_i(ng, 2);
  for (int i = 0; i < 3; i++) {
    const uint8_t *w = &words[i * LSM6DSO_FIFO_WORD_BYTES];
    cl_assert_equal_i(w[0] >> 3, LSM6DSO_FIFO_TAG_ACCEL_NC);
    cl_assert_equal_i(w[1], (uint8_t)((i + 1) * 10 + 1));
    cl_assert_equal_i(w[6], (uint8_t)((i + 1) * 10 + 6));
  }
  cl_assert_equal_i(gyro[0], 101);
  cl_assert_equal_i(gyro[5], 106);
  cl_assert_equal_i(gyro[6], 111);
  cl_assert_equal_i(gyro[11], 116);
}

void test_lsm6dso_fifo__gyro_capacity_is_respected(void) {
  uint8_t words[5 * LSM6DSO_FIFO_WORD_BYTES];
  for (int i = 0; i < 5; i++) {
    prv_word(&words[i * LSM6DSO_FIFO_WORD_BYTES], LSM6DSO_FIFO_TAG_GYRO_NC, (uint8_t)i);
  }
  uint8_t gyro[2 * 6];
  size_t ng = 0;
  cl_assert_equal_i(lsm6dso_fifo_split(words, 5, gyro, 2, &ng), 0);
  cl_assert_equal_i(ng, 2);
}
