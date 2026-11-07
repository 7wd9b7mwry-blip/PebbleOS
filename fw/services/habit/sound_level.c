/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/habit/sound_level.h>

#include <string.h>

// 100 * log10(32768^2) = 903.09: dB relative to full scale
#define FULL_SCALE_LOG10_X100 903

void sound_level_reset(SoundLevelAcc *acc) {
  memset(acc, 0, sizeof(*acc));
}

void sound_level_add(SoundLevelAcc *acc, const int16_t *samples, size_t count) {
  if (count == 0) {
    return;
  }
  int64_t sum = 0;
  for (size_t i = 0; i < count; i++) {
    sum += samples[i];
  }
  const int32_t mean = (int32_t)(sum / (int64_t)count);
  for (size_t i = 0; i < count; i++) {
    const int32_t v = samples[i] - mean;
    acc->sum_sq += (uint64_t)((int64_t)v * v);
    const uint32_t a = (uint32_t)(v < 0 ? -v : v);
    if (a > acc->peak) {
      acc->peak = a;
    }
  }
  acc->n += (uint32_t)count;
}

int32_t sound_level_log10_x100(uint64_t x) {
  if (x <= 1) {
    return 0;
  }
  // log2 in Q16: integer part from the top bit, fraction by repeated squaring
  int msb = 63;
  while (!(x >> msb)) {
    msb--;
  }
  uint64_t y = x;
  // normalise to [1, 2) in Q30
  uint64_t m = (msb >= 30) ? (y >> (msb - 30)) : (y << (30 - msb));
  int64_t log2_q16 = (int64_t)msb << 16;
  for (int bit = 15; bit >= 0; bit--) {
    m = (m * m) >> 30;
    if (m >= (2ULL << 30)) {
      m >>= 1;
      log2_q16 += 1 << bit;
    }
  }
  // log10(x) = log2(x) * log10(2); 100 * 0.30103 = 30.103
  return (int32_t)((log2_q16 * 30103 + 65536 * 500) / (65536 * 1000));  // rounded
}

static int16_t prv_db10(uint64_t power) {
  if (power == 0) {
    return SOUND_LEVEL_FLOOR_DB10;
  }
  const int32_t db10 = sound_level_log10_x100(power) - FULL_SCALE_LOG10_X100;
  return (int16_t)(db10 < SOUND_LEVEL_FLOOR_DB10 ? SOUND_LEVEL_FLOOR_DB10 : db10);
}

int16_t sound_level_rms_db10(const SoundLevelAcc *acc) {
  return acc->n ? prv_db10(acc->sum_sq / acc->n) : SOUND_LEVEL_FLOOR_DB10;
}

int16_t sound_level_peak_db10(const SoundLevelAcc *acc) {
  return prv_db10((uint64_t)acc->peak * acc->peak);
}
