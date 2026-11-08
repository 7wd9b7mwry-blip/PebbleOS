/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <clar.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pbl/services/habit/habit_detect.h>
#include <pbl/services/habit/habit_features.h>
#include <pbl/services/habit/habit_model.h>

#define N HABIT_FEAT_WINDOW
#define PI 3.14159265358979

static HabitVec s_acc[N], s_gyro[N];

//! gravity along `g`, plus `amp` mG at `hz` on all axes, gyro `gamp` 0.1 dps at `hz`
static void prv_window(const double g[3], double amp, double hz, double gamp) {
  for (int i = 0; i < N; i++) {
    const double t = i / (double)HABIT_FEAT_HZ;
    const double m = amp * sin(2 * PI * hz * t);
    s_acc[i] = (HabitVec){(int16_t)(g[0] * 1000 + m), (int16_t)(g[1] * 1000 + 0.7 * m),
                          (int16_t)(g[2] * 1000 - 0.5 * m)};
    s_gyro[i] = (HabitVec){(int16_t)(gamp * sin(2 * PI * hz * t + 1)),
                           (int16_t)(gamp * cos(2 * PI * hz * t)), 0};
  }
}

static const double FACE[3] = {-0.94, 0.0, -0.34};

void test_habit_features__picking_vs_still_vs_waving(void) {
  int32_t pick[HabitFeatCount], still[HabitFeatCount], wave[HabitFeatCount];
  prv_window(FACE, 60, 3, 400);
  habit_features_compute(s_acc, s_gyro, N, pick);
  prv_window(FACE, 1, 3, 5);
  habit_features_compute(s_acc, s_gyro, N, still);
  prv_window(FACE, 400, 1, 3000);
  habit_features_compute(s_acc, s_gyro, N, wave);

  cl_assert(pick[HabitFeat_HpMagMean] > 5 * (still[HabitFeat_HpMagMean] + 1));
  cl_assert(pick[HabitFeat_Repetition] > 300);  // a 3 Hz rhythm repeats within the lag range
  cl_assert(pick[HabitFeat_HpZcr] > still[HabitFeat_HpZcr] / 2);
  cl_assert(pick[HabitFeat_GyroHpMean] > 5 * (still[HabitFeat_GyroHpMean] + 1));
  // waving: much more slow motion and rotation than picking
  cl_assert(wave[HabitFeat_SlowMotion] > 3 * pick[HabitFeat_SlowMotion]);
  cl_assert(wave[HabitFeat_GyroMagMean] > 3 * pick[HabitFeat_GyroMagMean]);
  cl_assert(abs(still[HabitFeat_GravityError]) < 30);
}

void test_habit_features__no_gyro_zeroes_rotation(void) {
  int32_t f[HabitFeatCount];
  prv_window(FACE, 60, 3, 400);
  habit_features_compute(s_acc, NULL, N, f);
  cl_assert_equal_i(f[HabitFeat_GyroMagMean], 0);
  cl_assert_equal_i(f[HabitFeat_GyroToAcc], 0);
  cl_assert(f[HabitFeat_HpMagMean] > 0);
}

void test_habit_features__orientation_invariant_magnitudes(void) {
  // the same motion with the device turned 90 degrees about z: magnitude features unchanged
  int32_t a[HabitFeatCount], b[HabitFeatCount];
  prv_window(FACE, 60, 3, 400);
  habit_features_compute(s_acc, s_gyro, N, a);
  for (int i = 0; i < N; i++) {
    const HabitVec v = s_acc[i], g = s_gyro[i];
    s_acc[i] = (HabitVec){(int16_t)-v.y, v.x, v.z};
    s_gyro[i] = (HabitVec){(int16_t)-g.y, g.x, g.z};
  }
  habit_features_compute(s_acc, s_gyro, N, b);
  const HabitFeature inv[] = {HabitFeat_HpMagMean, HabitFeat_HpMagStd, HabitFeat_HpMagMax,
                              HabitFeat_GyroMagMean, HabitFeat_GyroMagMax, HabitFeat_GravTurn,
                              HabitFeat_GravPath, HabitFeat_Repetition, HabitFeat_GravityError};
  for (size_t k = 0; k < sizeof(inv) / sizeof(inv[0]); k++) {
    cl_assert(abs(a[inv[k]] - b[inv[k]]) <= 2 + abs(a[inv[k]]) / 50);
  }
}

void test_habit_features__names(void) {
  cl_assert_equal_s(habit_feature_name(HabitFeat_Repetition), "repetition");
  cl_assert_equal_s(habit_feature_name(HabitFeatCount), "?");
}

void test_habit_features__detector_scores_windows_near_the_head(void) {
  // the windows near the head are scored once a second and handed out for logging
  HabitConfig cfg;
  habit_detect_default_config(&cfg);
  HabitDetector d;
  habit_detect_init(&d, &cfg);
  int32_t f[HabitFeatCount], score;
  uint32_t t = 1000;
  int windows = 0;
  for (int i = 0; i < 25 * 6; i++, t += 40) {  // 6 s flat on the desk: never scored
    const HabitVec a = {0, 0, -1000};
    habit_detect_feed(&d, t, &a, NULL, false, NULL);
    windows += habit_detect_take_window(&d, f, &score);
  }
  cl_assert_equal_i(windows, 0);
  for (int i = 0; i < 25 * 6; i++, t += 40) {  // 6 s at the face, picking
    const double m = 60 * sin(2 * PI * 4 * i / 25.0);
    const HabitVec a = {(int16_t)(-940 + m), (int16_t)(0.7 * m), (int16_t)(-340 - 0.5 * m)};
    habit_detect_feed(&d, t, &a, NULL, false, NULL);
    windows += habit_detect_take_window(&d, f, &score);
  }
  cl_assert(windows >= 4 && windows <= 7);  // one a second once the hand is at the face
  cl_assert(f[HabitFeat_HpMagMean] > 20);
  cl_assert(!habit_detect_take_window(&d, f, &score));  // handed out once
}

// Real windows from the training data (Kaggle CMI, out-of-fold probabilities 0.988 and 0.013):
// the compiled-in model must keep scoring them the same way
static const int32_t s_eyelash_pull[HabitFeatCount] = {164, 88, 284, 918, 293, 217, 137, 620,
                                                       367, 1, -5, 80, 63, 791, 2224, 21};
static const int32_t s_glasses_on_off[HabitFeatCount] = {9, 9, 44, 119, 26, 41, 26, 136,
                                                         35, 1, -3, 64, 36, 0, 3500, 34};

void test_habit_features__trained_model(void) {
  cl_assert(habit_model_trained());
  cl_assert(g_habit_model.num_trees > 0);
  const int32_t pos = habit_model_score(s_eyelash_pull);
  const int32_t neg = habit_model_score(s_glasses_on_off);
  cl_assert(pos > g_habit_model.threshold);
  cl_assert(neg < g_habit_model.threshold);
  // the C export of these windows (features_cli): 4441 and -3984, logit x 1024
  cl_assert(abs(pos - 4441) <= 2);
  cl_assert(abs(neg - (-3984)) <= 2);
}

void test_habit_features__model_gates_the_detector(void) {
  // a still hand at the face scores low: with the model in use no episode starts even if the
  // fine-motion rule were satisfied
  HabitConfig cfg;
  habit_detect_default_config(&cfg);
  cfg.min_fine_mg = 0;  // let the rule pass everything
  HabitDetector d;
  habit_detect_init(&d, &cfg);
  uint32_t t = 1000;
  int starts = 0;
  for (int i = 0; i < 25 * 10; i++, t += 40) {
    const HabitVec a = {-940, 0, -340};
    starts += habit_detect_feed(&d, t, &a, NULL, false, NULL) == HabitEvent_Start;
  }
  cl_assert_equal_i(starts, 0);
  cl_assert(d.scored && !d.model_ok);
  cfg.use_model = false;  // the same without the model: the rule alone starts an episode
  habit_detect_init(&d, &cfg);
  for (int i = 0; i < 25 * 10; i++, t += 40) {
    const HabitVec a = {-940, 0, -340};
    starts += habit_detect_feed(&d, t, &a, NULL, false, NULL) == HabitEvent_Start;
  }
  cl_assert_equal_i(starts, 1);
}
