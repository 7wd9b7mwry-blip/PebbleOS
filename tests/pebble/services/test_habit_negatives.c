/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <clar.h>

#include <pbl/services/habit/habit_negatives.h>

static HabitNegatives s_n;
static int32_t s_f[16];

void test_habit_negatives__initialize(void) {
  habit_negatives_init(&s_n);
  for (int k = 0; k < 16; k++) {
    s_f[k] = 1000 + 100 * k;
  }
}

void test_habit_negatives__nothing_marked_matches_nothing(void) {
  cl_assert(!habit_negatives_match(&s_n, HabitRegion_Mouth, s_f));
  cl_assert_equal_i(habit_negatives_bias(&s_n, HabitRegion_Mouth), 0);
}

void test_habit_negatives__a_close_window_in_the_same_region_matches(void) {
  habit_negatives_add(&s_n, HabitRegion_Mouth, s_f);
  int32_t near[16], far[16];
  for (int k = 0; k < 16; k++) {
    near[k] = s_f[k] + 50;      // ~4% off
    far[k] = s_f[k] * 2;        // ~33% off
  }
  cl_assert(habit_negatives_match(&s_n, HabitRegion_Mouth, near));
  cl_assert(!habit_negatives_match(&s_n, HabitRegion_Mouth, far));
  cl_assert(!habit_negatives_match(&s_n, HabitRegion_Nose, near));   // another region
}

void test_habit_negatives__each_false_alarm_raises_the_region_threshold_up_to_a_cap(void) {
  for (int i = 0; i < 20; i++) {
    habit_negatives_add(&s_n, HabitRegion_Face, s_f);
  }
  cl_assert_equal_i(habit_negatives_bias(&s_n, HabitRegion_Face), HABIT_NEG_BIAS_MAX);
  cl_assert_equal_i(habit_negatives_bias(&s_n, HabitRegion_Mouth), 0);
  cl_assert_equal_i(s_n.n, HABIT_NEG_MAX);
}

void test_habit_negatives__the_oldest_is_replaced(void) {
  habit_negatives_add(&s_n, HabitRegion_Mouth, s_f);
  int32_t other[16];
  for (int k = 0; k < 16; k++) {
    other[k] = -5000 - 300 * k;
  }
  for (int i = 0; i < HABIT_NEG_MAX; i++) {
    habit_negatives_add(&s_n, HabitRegion_Hair, other);
  }
  cl_assert(!habit_negatives_match(&s_n, HabitRegion_Mouth, s_f));
  cl_assert(habit_negatives_match(&s_n, HabitRegion_Hair, other));
}

void test_habit_negatives__ignores_no_region(void) {
  habit_negatives_add(&s_n, HabitRegion_None, s_f);
  cl_assert_equal_i(s_n.n, 0);
}
