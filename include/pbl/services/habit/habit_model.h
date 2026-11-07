/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! Habit classifier: gradient-boosted trees over the habit features (habit_features.h), trained
//! offline (tools/habit/train.py) and generated into habit_model.c. Integer-only.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pbl/services/habit/habit_features.h>

typedef struct {
  uint8_t feature;     //!< HabitFeature, or 0xFF for a leaf
  int32_t threshold;   //!< go left when feature <= threshold
  int16_t left, right; //!< node indices within the tree
  int32_t value;       //!< leaf value, logit x 1024
} HabitModelNode;

typedef struct {
  const HabitModelNode *nodes;
  uint16_t num_nodes;
  const uint16_t *tree_roots;
  uint16_t num_trees;
  int32_t bias;         //!< logit x 1024
  int32_t threshold;    //!< default decision threshold on the score (logit x 1024)
  const char *version;  //!< training run, or "none" for the placeholder
} HabitModel;

extern const HabitModel g_habit_model;

//! True once a trained model is compiled in (the placeholder scores every window as a habit).
bool habit_model_trained(void);

//! The model's score for one window: logit x 1024 (0 = even odds).
int32_t habit_model_score(const int32_t features[HabitFeatCount]);
