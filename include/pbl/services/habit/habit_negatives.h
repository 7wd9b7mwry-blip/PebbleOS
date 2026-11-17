/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! False alarms the wearer marked (a button press right after a habit buzz): the detector learns from them on the
//! watch. Each one keeps the window's features (habit_features, 16) and its region; a later window of the same region
//! that close to one of them (relative L1 distance under HABIT_NEG_MATCH_PCT) doesn't start an episode, and each false
//! alarm raises that region's classifier threshold a little (HABIT_NEG_BIAS_STEP, at most HABIT_NEG_BIAS_MAX).
//! The newest HABIT_NEG_MAX are kept (oldest replaced). Pure: the habit service persists it.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pbl/services/habit/habit_detect.h>

#define HABIT_NEG_MAX 8
#define HABIT_NEG_FEATS 16
#define HABIT_NEG_MATCH_PCT 15     //!< relative L1 distance (%) that counts as the same motion
#define HABIT_NEG_BIAS_STEP 64     //!< threshold raise per false alarm (score: logit x 1024)
#define HABIT_NEG_BIAS_MAX 512
#define HABIT_NEG_VERSION 1

typedef struct HabitNegatives {
  uint8_t version;
  uint8_t n;       //!< stored (<= HABIT_NEG_MAX)
  uint8_t next;    //!< where the next one goes
  uint8_t region[HABIT_NEG_MAX];
  int16_t bias[HabitRegionCount];
  int16_t feat[HABIT_NEG_MAX][HABIT_NEG_FEATS];   //!< saturated to int16
} HabitNegatives;

void habit_negatives_init(HabitNegatives *n);

//! A false alarm in `region` with the window's features.
void habit_negatives_add(HabitNegatives *n, HabitRegion region, const int32_t features[HABIT_NEG_FEATS]);

//! Whether these features (in `region`) are that close to a marked false alarm.
bool habit_negatives_match(const HabitNegatives *n, HabitRegion region, const int32_t features[HABIT_NEG_FEATS]);

//! How much higher the classifier's threshold is in `region`.
int32_t habit_negatives_bias(const HabitNegatives *n, HabitRegion region);
