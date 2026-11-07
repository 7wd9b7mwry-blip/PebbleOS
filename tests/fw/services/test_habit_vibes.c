/* SPDX-FileCopyrightText: 2026 Daniel Allex */
/* SPDX-License-Identifier: Apache-2.0 */

#include <clar.h>

#include <string.h>

#include <pbl/services/habit/habit_policy.h>
#include <pbl/services/habit/vibe_program.h>

// ---- vibe programs ----------------------------------------------------------------------------

typedef struct {
  uint32_t total_ms;
  uint32_t on_ms;
  int steps;
  uint8_t max_s, min_on_s;
  bool rose, fell;
} Played;

static Played prv_play(VibeProgramId id, uint32_t max_ms, uint8_t scale, uint8_t floor) {
  VibePlayer p;
  vibe_player_start(&p, vibe_program_get(id), max_ms, scale, floor, 50);
  Played r = {.min_on_s = 255};
  uint16_t ms;
  uint8_t s, prev = 0;
  bool first = true;
  while (vibe_player_next(&p, &ms, &s)) {
    cl_assert(ms > 0 && ms <= 10000);
    cl_assert(s <= 100);
    if (!first) {
      cl_assert(s != prev);  // equal neighbours are merged
      r.rose |= s > prev;
      r.fell |= s < prev;
    }
    r.total_ms += ms;
    r.steps++;
    if (s) {
      r.on_ms += ms;
      r.max_s = s > r.max_s ? s : r.max_s;
      r.min_on_s = s < r.min_on_s ? s : r.min_on_s;
    }
    prev = s;
    first = false;
    cl_assert(r.steps < 100000);
  }
  return r;
}

void test_habit_vibes__calm_swells_smoothly_for_its_budget(void) {
  const Played r = prv_play(VibeProgram_Calm, 60000, 100, 0);
  cl_assert_equal_i(r.total_ms, 60000);
  cl_assert(r.rose && r.fell);
  cl_assert(r.max_s <= 45 && r.max_s >= 40);
  // a 4 s ramp at 50 ms resolution: many small steps, not a buzz
  cl_assert(r.steps > 400);
}

void test_habit_vibes__one_shot_plays_once(void) {
  const Played r = prv_play(VibeProgram_HabitTap, 60000, 100, 0);
  cl_assert_equal_i(r.total_ms, vibe_program_length_ms(vibe_program_get(VibeProgram_HabitTap)));
  cl_assert_equal_i(r.on_ms, 160);
  cl_assert_equal_i(r.steps, 3);
}

void test_habit_vibes__levels_escalate(void) {
  const Played a = prv_play(VibeProgram_HabitTap, 0, 100, 0);
  const Played b = prv_play(VibeProgram_HabitPulse, 0, 100, 0);
  const Played c = prv_play(VibeProgram_HabitBuzz, 0, 100, 0);
  cl_assert(a.max_s < b.max_s && b.max_s < c.max_s);
  cl_assert(a.on_ms < b.on_ms && b.on_ms < c.on_ms);
}

void test_habit_vibes__intensity_and_floor(void) {
  cl_assert_equal_i(vibe_strength_map(0, 100, 20), 0);   // off stays off
  cl_assert_equal_i(vibe_strength_map(1, 100, 20), 20);  // the weakest felt strength
  cl_assert_equal_i(vibe_strength_map(100, 100, 20), 100);
  cl_assert_equal_i(vibe_strength_map(100, 50, 0), 50);
  cl_assert_equal_i(vibe_strength_map(100, 0, 20), 0);   // intensity 0 = silent
  const Played r = prv_play(VibeProgram_Sleep, 30000, 100, 15);
  cl_assert(r.min_on_s >= 15);
}

void test_habit_vibes__every_program_is_playable(void) {
  for (int i = 0; i < VibeProgramCount; i++) {
    const VibeProgram *p = vibe_program_get(i);
    cl_assert(p && p->name && p->num_ramps > 0);
    const Played r = prv_play(i, 20000, 80, 10);
    cl_assert(r.total_ms > 0 && r.on_ms > 0);
  }
  cl_assert(vibe_program_get(VibeProgramCount) == NULL);
}

// ---- habit response ---------------------------------------------------------------------------

void test_habit_vibes__response_escalates_while_the_episode_lasts(void) {
  HabitResponseConfig cfg;
  habit_response_default_config(&cfg);
  HabitResponse r;
  habit_response_init(&r, &cfg);
  HabitEpisode ep = {.region = HabitRegion_Mouth, .start_ms = 0};
  VibeRequest req;
  cl_assert(habit_response_on_event(&r, 3000, HabitEvent_Start, &ep, &req));
  cl_assert_equal_i(req.program, VibeProgram_HabitTap);
  cl_assert(!habit_response_on_event(&r, 5000, HabitEvent_Ongoing, &ep, &req));
  cl_assert(habit_response_on_event(&r, 7000, HabitEvent_Ongoing, &ep, &req));
  cl_assert_equal_i(req.program, VibeProgram_HabitPulse);
  cl_assert(habit_response_on_event(&r, 11000, HabitEvent_Ongoing, &ep, &req));
  cl_assert_equal_i(req.program, VibeProgram_HabitBuzz);
  cl_assert(habit_response_on_event(&r, 15000, HabitEvent_Ongoing, &ep, &req));
  cl_assert_equal_i(req.program, VibeProgram_HabitBuzz);  // stays at the top level
  // gives up after max_alert_ms
  cl_assert(!habit_response_on_event(&r, 50000, HabitEvent_Ongoing, &ep, &req));
  // a new episode starts again from the light tap
  habit_response_on_event(&r, 51000, HabitEvent_End, &ep, NULL);
  ep.start_ms = 60000;
  cl_assert(habit_response_on_event(&r, 63000, HabitEvent_Start, &ep, &req));
  cl_assert_equal_i(req.program, VibeProgram_HabitTap);
}

void test_habit_vibes__response_log_only_mode(void) {
  HabitResponseConfig cfg;
  habit_response_default_config(&cfg);
  cfg.mode = HabitResponseMode_Off;
  HabitResponse r;
  habit_response_init(&r, &cfg);
  HabitEpisode ep = {0};
  VibeRequest req;
  cl_assert(!habit_response_on_event(&r, 3000, HabitEvent_Start, &ep, &req));
  cl_assert(!habit_response_on_event(&r, 9000, HabitEvent_Ongoing, &ep, &req));
}

// ---- smart vibes ------------------------------------------------------------------------------

static SmartVibes s_sv;
static uint32_t s_min;

static int prv_minutes(int n, uint8_t hr, uint16_t rmssd, uint16_t motion, bool asleep,
                       VibeProgramId *last) {
  int fired = 0;
  for (int i = 0; i < n; i++) {
    SmartVibeInput in = {.t_min = s_min, .minute_of_day = (uint16_t)(s_min % 1440), .worn = true,
                         .asleep = asleep, .hr_bpm = hr, .rmssd_ms = rmssd, .motion = motion};
    VibeRequest req;
    if (smart_vibes_on_minute(&s_sv, &in, &req)) {
      fired++;
      if (last) {
        *last = req.program;
      }
    }
    s_min++;
  }
  return fired;
}

void test_habit_vibes__initialize(void) {
  SmartVibeConfig cfg;
  smart_vibes_default_config(&cfg);
  smart_vibes_init(&s_sv, &cfg);
  s_min = 1440 * 20000 + 9 * 60;  // 09:00
}

void test_habit_vibes__stress_at_rest_gets_calm(void) {
  VibeProgramId p = VibeProgramCount;
  cl_assert_equal_i(prv_minutes(60, 62, 45, 10, false, NULL), 0);  // learn the baseline
  uint8_t rest;
  smart_vibes_baselines(&s_sv, &rest, NULL, NULL);
  cl_assert(rest >= 61 && rest <= 63);
  cl_assert_equal_i(prv_minutes(2, 85, 40, 10, false, NULL), 0);  // not yet: needs 3 minutes
  cl_assert_equal_i(prv_minutes(1, 85, 40, 10, false, &p), 1);
  cl_assert_equal_i(p, VibeProgram_Calm);
  // cooldown: still stressed, nothing for 30 minutes, then again
  cl_assert_equal_i(prv_minutes(29, 85, 40, 10, false, NULL), 0);
  cl_assert_equal_i(prv_minutes(3, 85, 40, 10, false, NULL), 1);
}

void test_habit_vibes__exercise_is_not_stress(void) {
  prv_minutes(60, 62, 45, 10, false, NULL);
  cl_assert_equal_i(prv_minutes(30, 140, 10, 400, false, NULL), 0);
}

void test_habit_vibes__low_hrv_alone_counts(void) {
  prv_minutes(60, 62, 50, 10, false, NULL);
  cl_assert_equal_i(prv_minutes(3, 64, 20, 10, false, NULL), 1);
}

void test_habit_vibes__no_baseline_no_action(void) {
  cl_assert_equal_i(prv_minutes(10, 110, 15, 10, false, NULL), 0);
}

void test_habit_vibes__daily_cap(void) {
  prv_minutes(60, 62, 45, 10, false, NULL);
  int fired = 0;
  for (int i = 0; i < 20; i++) {
    fired += prv_minutes(31, 90, 40, 10, false, NULL);
  }
  cl_assert_equal_i(fired, 8);
}

void test_habit_vibes__sleep_arousal_gets_sleep_vibe(void) {
  VibeProgramId p = VibeProgramCount;
  s_min = 1440 * 20000 + 23 * 60;
  cl_assert_equal_i(prv_minutes(60, 52, 60, 5, true, NULL), 0);
  uint8_t sleep_hr;
  smart_vibes_baselines(&s_sv, NULL, &sleep_hr, NULL);
  cl_assert(sleep_hr >= 51 && sleep_hr <= 53);
  // restless: heart rate up 10 with some movement
  cl_assert_equal_i(prv_minutes(1, 62, 40, 50, true, &p), 1);
  cl_assert_equal_i(p, VibeProgram_Sleep);
  cl_assert_equal_i(prv_minutes(19, 62, 40, 50, true, NULL), 0);  // cooldown
  cl_assert_equal_i(prv_minutes(1, 62, 40, 50, true, NULL), 1);
}

void test_habit_vibes__not_worn_does_nothing(void) {
  prv_minutes(60, 62, 45, 10, false, NULL);
  SmartVibeInput in = {.t_min = s_min, .minute_of_day = 600, .worn = false, .hr_bpm = 120,
                       .motion = 0};
  for (int i = 0; i < 10; i++, in.t_min++) {
    cl_assert(!smart_vibes_on_minute(&s_sv, &in, NULL));
  }
}
