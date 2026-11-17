/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <clar.h>

#include <math.h>
#include <stdlib.h>

#include <pbl/services/habit/sound_level.h>

static SoundLevelAcc s_acc;

static void prv_sine(double amplitude, double hz, int dc, int frames) {
  int16_t buf[320];
  static int t;
  for (int f = 0; f < frames; f++) {
    for (int i = 0; i < 320; i++, t++) {
      buf[i] = (int16_t)(dc + amplitude * sin(2 * 3.14159265358979 * hz * t / 16000.0));
    }
    sound_level_add(&s_acc, buf, 320);
  }
}

void test_sound_level__initialize(void) {
  sound_level_reset(&s_acc);
}

void test_sound_level__log10(void) {
  cl_assert_equal_i(sound_level_log10_x100(1), 0);
  cl_assert_equal_i(sound_level_log10_x100(10), 100);
  cl_assert_equal_i(sound_level_log10_x100(1000000), 600);
  // 100 * log10(2) = 30.1, log10(32768^2) * 100 = 903.1
  cl_assert(abs(sound_level_log10_x100(2) - 30) <= 1);
  cl_assert(abs(sound_level_log10_x100(32768ULL * 32768ULL) - 903) <= 1);
  // within 0.1 across the range
  for (uint64_t x = 3; x < (1ULL << 40); x = x * 7 + 1) {
    const int32_t want = (int32_t)lround(100.0 * log10((double)x));
    cl_assert(abs(sound_level_log10_x100(x) - want) <= 1);
  }
}

void test_sound_level__full_scale_sine(void) {
  prv_sine(32767, 1000, 0, 50);
  cl_assert(abs(sound_level_rms_db10(&s_acc) - (-30)) <= 2);  // -3.0 dBFS
  cl_assert(abs(sound_level_peak_db10(&s_acc) - 0) <= 2);
}

void test_sound_level__minus_40_db(void) {
  prv_sine(327.67, 440, 0, 50);  // -40 dBFS peak, -43 dBFS RMS
  cl_assert(abs(sound_level_rms_db10(&s_acc) - (-430)) <= 3);
  cl_assert(abs(sound_level_peak_db10(&s_acc) - (-400)) <= 3);
}

void test_sound_level__dc_offset_is_removed(void) {
  prv_sine(327.67, 440, 2000, 50);
  cl_assert(abs(sound_level_rms_db10(&s_acc) - (-430)) <= 3);
}

void test_sound_level__silence_is_the_floor(void) {
  cl_assert_equal_i(sound_level_rms_db10(&s_acc), SOUND_LEVEL_FLOOR_DB10);
  prv_sine(0, 440, 1500, 10);
  cl_assert_equal_i(sound_level_rms_db10(&s_acc), SOUND_LEVEL_FLOOR_DB10);
  cl_assert_equal_i(sound_level_peak_db10(&s_acc), SOUND_LEVEL_FLOOR_DB10);
}
