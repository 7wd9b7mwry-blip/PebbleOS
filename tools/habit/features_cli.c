/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! Host tool: the firmware's habit features for training data, so the model is trained on
//! exactly what the watch computes. Optionally scores each window with the compiled-in model.
//!
//!   cc -O2 -I include -o habit_features tools/habit/features_cli.c
//!      fw/services/habit/habit_features.c fw/services/habit/habit_model.c   (one line)
//!   habit_features < samples.csv > features.csv
//!
//! Input: "seq,ax,ay,az,gx,gy,gz" per line, HABIT_FEAT_HZ (10 Hz), mG and 0.1 deg/s, a sequence's samples in
//! order (gx empty: no gyroscope). Output per 3 s window (1 s hop): "seq,end,score,<features>",
//! end = index of the window's last sample within its sequence.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pbl/services/habit/habit_features.h>
#include <pbl/services/habit/habit_model.h>

#define MAX_SEQ 20000

static HabitVec s_acc[MAX_SEQ], s_gyro[MAX_SEQ];
static size_t s_n;
static int s_has_gyro;
static char s_seq[128];

static void prv_flush(void) {
  for (size_t end = HABIT_FEAT_WINDOW; end <= s_n; end += HABIT_FEAT_HOP) {
    int32_t f[HabitFeatCount];
    const size_t start = end - HABIT_FEAT_WINDOW;
    habit_features_compute(&s_acc[start], s_has_gyro ? &s_gyro[start] : NULL, HABIT_FEAT_WINDOW,
                           f);
    printf("%s,%zu,%ld", s_seq, end - 1, (long)habit_model_score(f));
    for (int i = 0; i < HabitFeatCount; i++) {
      printf(",%ld", (long)f[i]);
    }
    printf("\n");
  }
  s_n = 0;
}

int main(void) {
  printf("seq,end,score");
  for (int i = 0; i < HabitFeatCount; i++) {
    printf(",%s", habit_feature_name(i));
  }
  printf("\n");
  char line[512];
  while (fgets(line, sizeof(line), stdin)) {
    char *save = NULL;
    const char *seq = strtok_r(line, ",\r\n", &save);
    if (!seq || strcmp(seq, "seq") == 0) {
      continue;
    }
    if (strcmp(seq, s_seq) != 0) {
      prv_flush();
      snprintf(s_seq, sizeof(s_seq), "%s", seq);
    }
    long v[6] = {0};
    int got = 0;
    char *tok;
    while (got < 6 && (tok = strtok_r(NULL, ",\r\n", &save))) {
      v[got++] = strtol(tok, NULL, 10);
    }
    if (got < 3 || s_n >= MAX_SEQ) {
      continue;
    }
    s_has_gyro = got >= 6;
    s_acc[s_n] = (HabitVec){(int16_t)v[0], (int16_t)v[1], (int16_t)v[2]};
    s_gyro[s_n] = (HabitVec){(int16_t)v[3], (int16_t)v[4], (int16_t)v[5]};
    s_n++;
  }
  prv_flush();
  return 0;
}
