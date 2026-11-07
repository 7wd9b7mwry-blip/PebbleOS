/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/habit/habit_model.h>

#include <string.h>

#include "habit_model_data.h"

bool habit_model_trained(void) {
  return g_habit_model.num_trees > 0;
}

int32_t habit_model_score(const int32_t features[HabitFeatCount]) {
  const HabitModel *m = &g_habit_model;
  if (m->num_trees == 0) {
    return INT32_MAX / 2;  // placeholder: every window passes, the rule-based detector decides
  }
  int64_t sum = m->bias;
  for (uint16_t t = 0; t < m->num_trees; t++) {
    const HabitModelNode *tree = &m->nodes[m->tree_roots[t]];
    int16_t i = 0;
    for (int depth = 0; depth < 64 && tree[i].feature != 0xFF; depth++) {
      const HabitModelNode *n = &tree[i];
      i = (features[n->feature] <= n->threshold) ? n->left : n->right;
    }
    sum += tree[i].value;
  }
  return (int32_t)(sum > INT32_MAX / 2 ? INT32_MAX / 2 : (sum < -INT32_MAX / 2 ? -INT32_MAX / 2 : sum));
}
