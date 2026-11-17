/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/habit/habit_negatives.h>

#include <string.h>

static int16_t prv_sat(int32_t v) {
  return v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : (int16_t)v;
}

static int32_t prv_abs(int32_t v) {
  return v < 0 ? -v : v;
}

void habit_negatives_init(HabitNegatives *n) {
  memset(n, 0, sizeof(*n));
  n->version = HABIT_NEG_VERSION;
}

void habit_negatives_add(HabitNegatives *n, HabitRegion region, const int32_t features[HABIT_NEG_FEATS]) {
  if (region <= HabitRegion_None || region >= HabitRegionCount) {
    return;
  }
  const uint8_t i = n->next % HABIT_NEG_MAX;
  n->region[i] = (uint8_t)region;
  for (int k = 0; k < HABIT_NEG_FEATS; k++) {
    n->feat[i][k] = prv_sat(features[k]);
  }
  n->next = (uint8_t)((i + 1) % HABIT_NEG_MAX);
  if (n->n < HABIT_NEG_MAX) {
    n->n++;
  }
  const int32_t b = n->bias[region] + HABIT_NEG_BIAS_STEP;
  n->bias[region] = (int16_t)(b > HABIT_NEG_BIAS_MAX ? HABIT_NEG_BIAS_MAX : b);
}

bool habit_negatives_match(const HabitNegatives *n, HabitRegion region, const int32_t features[HABIT_NEG_FEATS]) {
  for (int i = 0; i < n->n && i < HABIT_NEG_MAX; i++) {
    if (n->region[i] != region) {
      continue;
    }
    int64_t dist = 0, size = 0;
    for (int k = 0; k < HABIT_NEG_FEATS; k++) {
      const int32_t f = prv_sat(features[k]);
      dist += prv_abs(f - n->feat[i][k]);
      size += prv_abs(f) + prv_abs(n->feat[i][k]);
    }
    if (size > 0 && dist * 100 < size * HABIT_NEG_MATCH_PCT) {
      return true;
    }
  }
  return false;
}

int32_t habit_negatives_bias(const HabitNegatives *n, HabitRegion region) {
  if (region <= HabitRegion_None || region >= HabitRegionCount) {
    return 0;
  }
  return n->bias[region];
}
