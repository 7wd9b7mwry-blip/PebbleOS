/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/habit/habit_features.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SMOOTH 9      // moving average length (0.36 s): removes gravity and slow motion
#define EDGE 10       // samples averaged at each end for the forearm turn (0.4 s)
#define PATH_STEP 5   // 0.2 s steps for the forearm path
#define LAG_MIN 3
#define LAG_MAX 12

static int64_t prv_isqrt(int64_t v) {
  if (v <= 0) {
    return 0;
  }
  int64_t x = v, y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + v / x) / 2;
  }
  return x;
}

static int32_t prv_mag(int32_t x, int32_t y, int32_t z) {
  return (int32_t)prv_isqrt((int64_t)x * x + (int64_t)y * y + (int64_t)z * z);
}

//! 1000 - cos(angle) * 1000 between two vectors (0 = same direction, 2000 = opposite)
static int32_t prv_turn(const int32_t a[3], const int32_t b[3]) {
  const int64_t na = prv_isqrt((int64_t)a[0] * a[0] + (int64_t)a[1] * a[1] + (int64_t)a[2] * a[2]);
  const int64_t nb = prv_isqrt((int64_t)b[0] * b[0] + (int64_t)b[1] * b[1] + (int64_t)b[2] * b[2]);
  if (na == 0 || nb == 0) {
    return 0;
  }
  const int64_t dot = (int64_t)a[0] * b[0] + (int64_t)a[1] * b[1] + (int64_t)a[2] * b[2];
  return (int32_t)(1000 - (dot * 1000) / (na * nb));
}

static int32_t prv_axis(const HabitVec *v, int axis) {
  return axis == 0 ? v->x : (axis == 1 ? v->y : v->z);
}

//! centred moving average of `src` axis at i (window SMOOTH, clipped at the edges), x1
static int32_t prv_smooth(const HabitVec *src, size_t n, size_t i, int axis) {
  const int half = SMOOTH / 2;
  int32_t sum = 0, cnt = 0;
  for (int k = -half; k <= half; k++) {
    const int j = (int)i + k;
    if (j >= 0 && j < (int)n) {
      sum += prv_axis(&src[j], axis);
      cnt++;
    }
  }
  return sum / cnt;
}

static int32_t prv_std(const int32_t *x, size_t n, int32_t mean) {
  int64_t acc = 0;
  for (size_t i = 0; i < n; i++) {
    const int64_t d = x[i] - mean;
    acc += d * d;
  }
  return (int32_t)prv_isqrt(acc / (int64_t)n);
}

void habit_features_compute(const HabitVec *acc, const HabitVec *gyro, size_t n,
                            int32_t out[HabitFeatCount]) {
  memset(out, 0, sizeof(int32_t) * HabitFeatCount);
  if (n < LAG_MAX + 2 || n > HABIT_FEAT_WINDOW) {
    return;
  }
  int32_t hp_mag[HABIT_FEAT_WINDOW];
  int32_t hp[3][HABIT_FEAT_WINDOW];
  int32_t lp_prev[3] = {0};
  int64_t sum_hp = 0, slow = 0, jerk = 0, amag = 0;
  int32_t hp_max = 0;

  for (size_t i = 0; i < n; i++) {
    int32_t lp[3];
    for (int a = 0; a < 3; a++) {
      lp[a] = prv_smooth(acc, n, i, a);
      hp[a][i] = prv_axis(&acc[i], a) - lp[a];
      if (i > 0) {
        slow += abs(lp[a] - lp_prev[a]) * 10;
        jerk += abs(prv_axis(&acc[i], a) - prv_axis(&acc[i - 1], a));
      }
      lp_prev[a] = lp[a];
    }
    hp_mag[i] = prv_mag(hp[0][i], hp[1][i], hp[2][i]);
    sum_hp += hp_mag[i];
    hp_max = hp_mag[i] > hp_max ? hp_mag[i] : hp_max;
    amag += prv_mag(acc[i].x, acc[i].y, acc[i].z);
  }
  const int32_t hp_mean = (int32_t)(sum_hp / (int64_t)n);
  out[HabitFeat_HpMagMean] = hp_mean;
  out[HabitFeat_HpMagStd] = prv_std(hp_mag, n, hp_mean);
  out[HabitFeat_HpMagMax] = hp_max;
  out[HabitFeat_SlowMotion] = (int32_t)(slow / (int64_t)(n - 1));
  out[HabitFeat_Jerk] = (int32_t)(jerk / (int64_t)(n - 1));
  out[HabitFeat_GravityError] = abs((int32_t)(amag / (int64_t)n) - 1000);

  // forearm turn: smoothed direction at the ends, and along the way
  int32_t first[3] = {0}, last[3] = {0};
  for (size_t i = 0; i < EDGE; i++) {
    for (int a = 0; a < 3; a++) {
      first[a] += prv_axis(&acc[i], a);
      last[a] += prv_axis(&acc[n - EDGE + i], a);
    }
  }
  out[HabitFeat_GravTurn] = prv_turn(first, last);
  int32_t path = 0;
  for (size_t i = PATH_STEP; i < n; i += PATH_STEP) {
    int32_t p[3], q[3];
    for (int a = 0; a < 3; a++) {
      p[a] = prv_smooth(acc, n, i - PATH_STEP, a);
      q[a] = prv_smooth(acc, n, i, a);
    }
    path += prv_turn(p, q);
  }
  out[HabitFeat_GravPath] = path;

  // zero crossings of the high-passed acceleration
  int32_t zcr = 0;
  for (int a = 0; a < 3; a++) {
    for (size_t i = 1; i < n; i++) {
      zcr += ((hp[a][i - 1] < 0) != (hp[a][i] < 0)) ? 1 : 0;
    }
  }
  out[HabitFeat_HpZcr] = zcr;

  // repetition: normalised autocorrelation of the high-passed magnitude
  int64_t var = 0;
  for (size_t i = 0; i < n; i++) {
    const int64_t d = hp_mag[i] - hp_mean;
    var += d * d;
  }
  int32_t rep = 0;
  if (var > 0) {
    for (int lag = LAG_MIN; lag <= LAG_MAX; lag++) {
      int64_t c = 0;
      for (size_t i = lag; i < n; i++) {
        c += (int64_t)(hp_mag[i] - hp_mean) * (hp_mag[i - lag] - hp_mean);
      }
      // scale for the shorter overlap
      const int32_t r = (int32_t)((c * 1000 * (int64_t)n) / (var * (int64_t)(n - lag)));
      rep = r > rep ? r : rep;
    }
  }
  out[HabitFeat_Repetition] = rep;

  if (!gyro) {
    return;
  }
  int32_t g_mag[HABIT_FEAT_WINDOW];
  int64_t gsum = 0, ghp = 0;
  int32_t gmax = 0;
  int64_t gmean_axis[3] = {0};
  for (size_t i = 0; i < n; i++) {
    g_mag[i] = prv_mag(gyro[i].x, gyro[i].y, gyro[i].z);
    gsum += g_mag[i];
    gmax = g_mag[i] > gmax ? g_mag[i] : gmax;
    for (int a = 0; a < 3; a++) {
      ghp += abs(prv_axis(&gyro[i], a) - prv_smooth(gyro, n, i, a));
      gmean_axis[a] += prv_axis(&gyro[i], a);
    }
  }
  const int32_t gmean = (int32_t)(gsum / (int64_t)n);
  out[HabitFeat_GyroMagMean] = gmean;
  out[HabitFeat_GyroMagStd] = prv_std(g_mag, n, gmean);
  out[HabitFeat_GyroMagMax] = gmax;
  out[HabitFeat_GyroHpMean] = (int32_t)(ghp / (int64_t)n);
  int32_t gz = 0;
  for (int a = 0; a < 3; a++) {
    const int32_t m = (int32_t)(gmean_axis[a] / (int64_t)n);
    for (size_t i = 1; i < n; i++) {
      gz += ((prv_axis(&gyro[i - 1], a) < m) != (prv_axis(&gyro[i], a) < m)) ? 1 : 0;
    }
  }
  out[HabitFeat_GyroZcr] = gz;
  out[HabitFeat_GyroToAcc] = (out[HabitFeat_GyroHpMean] * 1000) / (hp_mean + 1);
}

const char *habit_feature_name(HabitFeature f) {
  static const char *const names[HabitFeatCount] = {
      "hp_mag_mean", "hp_mag_std", "hp_mag_max", "slow_motion", "jerk", "gyro_mag_mean",
      "gyro_mag_std", "gyro_mag_max", "gyro_hp_mean", "grav_turn", "grav_path", "hp_zcr",
      "gyro_zcr", "repetition", "gyro_to_acc", "gravity_error",
  };
  return (f < HabitFeatCount) ? names[f] : "?";
}
