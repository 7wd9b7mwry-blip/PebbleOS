/* SPDX-FileCopyrightText: 2026 Daniel Allex */
/* SPDX-License-Identifier: Apache-2.0 */

#include <clar.h>

#include <math.h>
#include <string.h>

#include <pbl/services/habit/habit_detect.h>

// Synthetic wrist IMU at 25 Hz: a forearm orientation (unit gravity direction * 1 g), plus a
// sinusoidal motion component. Orientations in the Pebble frame (left wrist).
#define HZ 25
#define DT_MS (1000 / HZ)

typedef struct {
  float x, y, z;
} V3;

static const V3 ARM_DOWN = {-1.0f, 0.0f, 0.0f};    // hanging, hand down
static const V3 ARM_DESK = {0.0f, 0.0f, 1.0f};     // flat, screen up (typing)
static const V3 HAND_FACE = {0.94f, 0.0f, 0.34f};  // forearm raised ~70 deg
static const V3 HAND_HAIR = {0.5f, -0.85f, 0.15f}; // elbow lifted, wrist rotated

static HabitDetector s_d;
static uint32_t s_t;
static int s_starts, s_ends, s_ongoing;
static HabitEpisode s_last_start, s_last_end;

static V3 prv_norm(V3 v) {
  const float n = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
  return (V3){v.x / n, v.y / n, v.z / n};
}

//! Feed `ms` of samples: orientation `o`, motion amplitude `amp_mg` at `freq_hz` on all axes,
//! gyro amplitude `gyro_dps` (or <0 for no gyro), `vibrating` flag.
static void prv_run(V3 o, uint32_t ms, float amp_mg, float freq_hz, float gyro_dps, bool vibrating) {
  o = prv_norm(o);
  for (uint32_t i = 0; i < ms / DT_MS; i++) {
    const float ph = 2.0f * 3.14159265f * freq_hz * (s_t / 1000.0f);
    const float m = amp_mg * sinf(ph);
    HabitVec a = {(int16_t)(o.x * 1000 + m), (int16_t)(o.y * 1000 + 0.7f * m),
                  (int16_t)(o.z * 1000 - 0.5f * m)};
    HabitVec g = {(int16_t)(gyro_dps * 10 * sinf(ph)), (int16_t)(gyro_dps * 10 * cosf(ph)), 0};
    HabitEpisode ep;
    const HabitEvent e = habit_detect_feed(&s_d, s_t, &a, gyro_dps >= 0 ? &g : NULL, vibrating, &ep);
    if (e == HabitEvent_Start) {
      s_starts++;
      s_last_start = ep;
    } else if (e == HabitEvent_End) {
      s_ends++;
      s_last_end = ep;
    } else if (e == HabitEvent_Ongoing) {
      s_ongoing++;
    }
    s_t += DT_MS;
  }
}

//! Move from `a` to `b` over `ms` with moderate motion (a deliberate arm movement)
static void prv_move(V3 a, V3 b, uint32_t ms) {
  const uint32_t n = ms / DT_MS;
  for (uint32_t i = 0; i < n; i++) {
    const float f = (float)i / n;
    prv_run((V3){a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f}, DT_MS, 0, 1,
            -1, false);
  }
}

void test_habit_detect__initialize(void) {
  HabitConfig cfg;
  habit_detect_default_config(&cfg);
  habit_detect_init(&s_d, &cfg);
  s_t = 100000;
  s_starts = s_ends = s_ongoing = 0;
  memset(&s_last_start, 0, sizeof(s_last_start));
  memset(&s_last_end, 0, sizeof(s_last_end));
  prv_run(ARM_DESK, 3000, 3, 2, -1, false);  // settle the filters
}

void test_habit_detect__picking_at_face_is_detected(void) {
  prv_move(ARM_DESK, HAND_FACE, 600);
  const uint32_t arrived = s_t;
  prv_run(HAND_FACE, 8000, 60, 4, -1, false);  // small 4 Hz movements: picking / biting
  cl_assert_equal_i(s_starts, 1);
  cl_assert_equal_i(s_last_start.region, HabitRegion_Face);
  // confirmed after the 3 s dwell (plus filter settling), not before
  cl_assert(s_last_start.duration_ms >= 3000);
  cl_assert(s_last_start.start_ms >= arrived - 600 && s_last_start.start_ms <= arrived + 1500);
  cl_assert(s_ongoing >= 3);
  cl_assert_equal_i(s_ends, 0);
  prv_move(HAND_FACE, ARM_DESK, 600);
  prv_run(ARM_DESK, 3000, 3, 2, -1, false);
  cl_assert_equal_i(s_ends, 1);
  cl_assert(s_last_end.duration_ms >= 7000 && s_last_end.duration_ms <= 10000);
  cl_assert(s_last_end.confidence > 30);
}

void test_habit_detect__still_hand_at_face_is_not(void) {
  // resting the chin on the hand: right orientation, no fine motion
  prv_move(ARM_DESK, HAND_FACE, 600);
  prv_run(HAND_FACE, 10000, 2, 4, -1, false);
  cl_assert_equal_i(s_starts, 0);
}

void test_habit_detect__walking_is_not(void) {
  prv_move(ARM_DESK, ARM_DOWN, 600);
  prv_run(ARM_DOWN, 10000, 300, 2, -1, false);
  cl_assert_equal_i(s_starts, 0);
}

void test_habit_detect__big_motion_at_face_is_not(void) {
  // e.g. brushing hair vigorously, waving the arm around overhead
  prv_move(ARM_DESK, HAND_FACE, 600);
  prv_run(HAND_FACE, 10000, 400, 3, -1, false);
  cl_assert_equal_i(s_starts, 0);
}

void test_habit_detect__brief_touches_are_not(void) {
  // eating / drinking: hand to mouth for ~1.5 s, down for ~3 s, repeated
  for (int i = 0; i < 6; i++) {
    prv_move(ARM_DESK, HAND_FACE, 400);
    prv_run(HAND_FACE, 1500, 60, 4, -1, false);
    prv_move(HAND_FACE, ARM_DESK, 400);
    prv_run(ARM_DESK, 3000, 3, 2, -1, false);
  }
  cl_assert_equal_i(s_starts, 0);
}

void test_habit_detect__short_pause_keeps_the_episode(void) {
  prv_move(ARM_DESK, HAND_FACE, 600);
  prv_run(HAND_FACE, 6000, 60, 4, -1, false);
  cl_assert_equal_i(s_starts, 1);
  // drops the hand briefly (under release_ms) and goes back
  prv_move(HAND_FACE, ARM_DESK, 300);
  prv_move(ARM_DESK, HAND_FACE, 300);
  prv_run(HAND_FACE, 4000, 60, 4, -1, false);
  cl_assert_equal_i(s_starts, 1);
  cl_assert_equal_i(s_ends, 0);
  // and a pause in the motion itself (hand stays) does not end it either
  prv_run(HAND_FACE, 3000, 2, 4, -1, false);
  cl_assert_equal_i(s_ends, 0);
}

void test_habit_detect__own_vibration_is_ignored(void) {
  // a strong vibration while the hand is still at the face must neither start an episode ...
  prv_move(ARM_DESK, HAND_FACE, 600);
  prv_run(HAND_FACE, 6000, 300, 120, -1, true);
  cl_assert_equal_i(s_starts, 0);
  // ... nor end or disturb one in progress
  prv_run(HAND_FACE, 6000, 60, 4, -1, false);
  cl_assert_equal_i(s_starts, 1);
  prv_run(HAND_FACE, 2000, 500, 120, -1, true);
  cl_assert_equal_i(s_ends, 0);
  prv_run(HAND_FACE, 2000, 60, 4, -1, false);
  cl_assert_equal_i(s_ends, 0);
}

void test_habit_detect__gyro_counts_as_fine_motion(void) {
  // rubbing / twisting with almost no linear motion: only the gyro sees it
  prv_move(ARM_DESK, HAND_FACE, 600);
  prv_run(HAND_FACE, 8000, 2, 4, -1, false);
  cl_assert_equal_i(s_starts, 0);
  prv_run(HAND_FACE, 8000, 2, 4, 40, false);  // 40 deg/s wrist rotation
  cl_assert_equal_i(s_starts, 1);
}

void test_habit_detect__calibrated_regions(void) {
  // calibrate "hair" at its own orientation, then picking there reports hair, not face
  prv_move(ARM_DESK, HAND_HAIR, 600);
  prv_run(HAND_HAIR, 2000, 3, 4, -1, false);
  habit_detect_calibrate_region(&s_d, HabitRegion_Hair, 940);  // ~20 deg
  cl_assert_equal_i(habit_detect_current_region(&s_d), HabitRegion_Hair);
  prv_run(HAND_HAIR, 6000, 60, 4, -1, false);
  cl_assert_equal_i(s_starts, 1);
  cl_assert_equal_i(s_last_start.region, HabitRegion_Hair);
  prv_move(HAND_HAIR, HAND_FACE, 600);
  cl_assert_equal_i(habit_detect_current_region(&s_d), HabitRegion_Face);
}

void test_habit_detect__arm_down_matches_nothing(void) {
  prv_move(ARM_DESK, ARM_DOWN, 600);
  prv_run(ARM_DOWN, 2000, 3, 2, -1, false);
  cl_assert_equal_i(habit_detect_current_region(&s_d), HabitRegion_None);
  cl_assert_equal_s(habit_region_name(HabitRegion_Hair), "hair");
}
