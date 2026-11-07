/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/habit/vibe_program.h>

#include <stddef.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static const VibeRamp s_calm[] = {
    {4000, 8, 45}, {1000, 45, 45}, {5000, 45, 8}, {500, 8, 8},
};
static const VibeRamp s_sleep[] = {
    {6000, 4, 25}, {1000, 25, 25}, {7000, 25, 4}, {1000, 4, 4},
};
static const VibeRamp s_focus[] = {
    {600, 15, 40}, {1400, 40, 15},
};
static const VibeRamp s_energy[] = {
    {250, 70, 70}, {150, 0, 0}, {250, 70, 70}, {600, 0, 0},
};
static const VibeRamp s_habit_tap[] = {
    {80, 60, 60}, {140, 0, 0}, {80, 60, 60},
};
static const VibeRamp s_habit_pulse[] = {
    {120, 85, 85}, {120, 0, 0}, {120, 85, 85}, {120, 0, 0}, {120, 85, 85},
};
static const VibeRamp s_habit_buzz[] = {
    {600, 100, 100}, {250, 0, 0}, {600, 100, 100},
};

static const VibeProgram s_programs[VibeProgramCount] = {
    [VibeProgram_Calm] = {"calm", s_calm, ARRAY_LEN(s_calm), true},
    [VibeProgram_Sleep] = {"sleep", s_sleep, ARRAY_LEN(s_sleep), true},
    [VibeProgram_Focus] = {"focus", s_focus, ARRAY_LEN(s_focus), true},
    [VibeProgram_Energy] = {"energy", s_energy, ARRAY_LEN(s_energy), true},
    [VibeProgram_HabitTap] = {"habit_tap", s_habit_tap, ARRAY_LEN(s_habit_tap), false},
    [VibeProgram_HabitPulse] = {"habit_pulse", s_habit_pulse, ARRAY_LEN(s_habit_pulse), false},
    [VibeProgram_HabitBuzz] = {"habit_buzz", s_habit_buzz, ARRAY_LEN(s_habit_buzz), false},
};

const VibeProgram *vibe_program_get(VibeProgramId id) {
  return (id < VibeProgramCount) ? &s_programs[id] : NULL;
}

uint32_t vibe_program_length_ms(const VibeProgram *program) {
  uint32_t ms = 0;
  for (uint8_t i = 0; i < program->num_ramps; i++) {
    ms += program->ramps[i].ms;
  }
  return ms;
}

uint8_t vibe_strength_map(uint8_t s, uint8_t scale_pct, uint8_t floor_pct) {
  if (s == 0 || scale_pct == 0) {
    return 0;
  }
  s = s > 100 ? 100 : s;
  const uint32_t scaled = (uint32_t)s * scale_pct / 100;
  const uint32_t out = floor_pct + scaled * (100 - floor_pct) / 100;
  return (uint8_t)(out > 100 ? 100 : (out < 1 ? 1 : out));
}

void vibe_player_start(VibePlayer *p, const VibeProgram *program, uint32_t max_ms,
                       uint8_t scale_pct, uint8_t floor_pct, uint16_t step_ms) {
  *p = (VibePlayer){
      .program = program,
      .remaining_ms = (program && (max_ms == 0 || !program->loops))
                          ? vibe_program_length_ms(program)
                          : max_ms,
      .scale_pct = scale_pct > 100 ? 100 : scale_pct,
      .floor_pct = floor_pct > 99 ? 99 : floor_pct,
      .step_ms = step_ms ? step_ms : 50,
  };
  if (program && !program->loops && max_ms && max_ms < p->remaining_ms) {
    p->remaining_ms = max_ms;
  }
}

//! Strength at the current position, and how long until the next change of step
static uint8_t prv_peek(const VibePlayer *p, uint16_t *len) {
  const VibeRamp *r = &p->program->ramps[p->ramp];
  const uint16_t left = r->ms - p->pos_ms;
  if (r->from == r->to) {
    *len = left;
    return r->from;
  }
  *len = left < p->step_ms ? left : p->step_ms;
  // strength at the middle of the step
  const int32_t mid = p->pos_ms + *len / 2;
  return (uint8_t)(r->from + ((int32_t)r->to - r->from) * mid / r->ms);
}

static void prv_advance(VibePlayer *p, uint16_t len) {
  p->pos_ms += len;
  p->remaining_ms = p->remaining_ms > len ? p->remaining_ms - len : 0;
  if (p->pos_ms >= p->program->ramps[p->ramp].ms) {
    p->pos_ms = 0;
    p->ramp++;
    if (p->ramp >= p->program->num_ramps) {
      p->ramp = 0;
      if (!p->program->loops) {
        p->remaining_ms = 0;
      }
    }
  }
}

bool vibe_player_next(VibePlayer *p, uint16_t *ms, uint8_t *strength) {
  if (!p->program || p->remaining_ms == 0) {
    return false;
  }
  uint16_t len;
  const uint8_t s = vibe_strength_map(prv_peek(p, &len), p->scale_pct, p->floor_pct);
  uint32_t total = len < p->remaining_ms ? len : p->remaining_ms;
  prv_advance(p, (uint16_t)total);
  // merge following steps of the same motor strength (cap a step at 10 s, the service's maximum)
  while (p->remaining_ms > 0 && total < 10000) {
    const VibePlayer save = *p;
    uint16_t l2;
    const uint8_t s2 = vibe_strength_map(prv_peek(p, &l2), p->scale_pct, p->floor_pct);
    if (s2 != s) {
      *p = save;
      break;
    }
    if (l2 > p->remaining_ms) {
      l2 = (uint16_t)p->remaining_ms;
    }
    if (total + l2 > 10000) {
      break;
    }
    prv_advance(p, l2);
    total += l2;
  }
  *ms = (uint16_t)total;
  *strength = s;
  return true;
}
