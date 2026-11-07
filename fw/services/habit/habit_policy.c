/* SPDX-FileCopyrightText: 2026 Daniel Allex */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/habit/habit_policy.h>

#include <string.h>

// ---- habit response -------------------------------------------------------------------------

static const VibeProgramId s_levels[] = {
    VibeProgram_HabitTap, VibeProgram_HabitPulse, VibeProgram_HabitBuzz};

void habit_response_default_config(HabitResponseConfig *cfg) {
  *cfg = (HabitResponseConfig){
      .mode = HabitResponseMode_Vibrate,
      .escalate_after_ms = 4000,
      .max_alert_ms = 45000,
  };
}

void habit_response_init(HabitResponse *r, const HabitResponseConfig *cfg) {
  memset(r, 0, sizeof(*r));
  r->cfg = *cfg;
}

static bool prv_alert(HabitResponse *r, uint32_t t_ms, VibeRequest *req) {
  if (r->level < 3) {
    r->level++;
  }
  r->last_alert_ms = t_ms;
  r->alerts_total++;
  if (req) {
    *req = (VibeRequest){.program = s_levels[r->level - 1], .max_ms = 0};
  }
  return true;
}

bool habit_response_on_event(HabitResponse *r, uint32_t t_ms, HabitEvent e, const HabitEpisode *ep,
                             VibeRequest *req) {
  switch (e) {
    case HabitEvent_Start:
      r->level = 0;
      r->episode_start_ms = ep ? ep->start_ms : t_ms;
      return r->cfg.mode == HabitResponseMode_Vibrate && prv_alert(r, t_ms, req);
    case HabitEvent_Ongoing:
      if (r->cfg.mode != HabitResponseMode_Vibrate || r->level == 0) {
        return false;
      }
      if (t_ms - r->episode_start_ms > r->cfg.max_alert_ms) {
        return false;
      }
      if (t_ms - r->last_alert_ms >= r->cfg.escalate_after_ms) {
        return prv_alert(r, t_ms, req);  // level 3 repeats at the escalation interval
      }
      return false;
    case HabitEvent_End:
      r->level = 0;
      return false;
    default:
      return false;
  }
}

// ---- smart vibes ------------------------------------------------------------------------------

#define FX 16
#define BASELINE_N 60  // ~1 hour of qualifying minutes for the running baselines

void smart_vibes_default_config(SmartVibeConfig *cfg) {
  *cfg = (SmartVibeConfig){
      .day_enabled = true,
      .sleep_enabled = true,
      .still_motion = 30,
      .stress_hr_over = 15,
      .stress_hrv_pct = 60,
      .stress_minutes = 3,
      .day_cooldown_min = 30,
      .day_max_per_day = 8,
      .arousal_motion = 80,
      .arousal_hr_over = 8,
      .sleep_cooldown_min = 20,
      .sleep_max_per_night = 6,
      .calm_ms = 3 * 60 * 1000,
      .sleep_ms = 2 * 60 * 1000,
  };
}

void smart_vibes_init(SmartVibes *s, const SmartVibeConfig *cfg) {
  memset(s, 0, sizeof(*s));
  s->cfg = *cfg;
}

static void prv_baseline(int32_t *b, uint16_t *n, int32_t x) {
  if (*n == 0) {
    *b = x * FX;
  } else {
    const int32_t div = *n < BASELINE_N ? *n + 1 : BASELINE_N;
    *b += (x * FX - *b) / div;
  }
  if (*n < 0xFFFF) {
    (*n)++;
  }
}

bool smart_vibes_on_minute(SmartVibes *s, const SmartVibeInput *in, VibeRequest *req) {
  const SmartVibeConfig *c = &s->cfg;
  // local day, and local night (noon to noon, named by its morning)
  const uint32_t local_midnight = in->t_min - in->minute_of_day;
  const uint16_t day = (uint16_t)(local_midnight / 1440);
  const uint16_t night = (uint16_t)((local_midnight + (in->minute_of_day >= 12 * 60 ? 1440 : 0)) / 1440);
  if (day != s->day_of_count) {
    s->day_of_count = day;
    s->day_count = 0;
  }
  if (night != s->night_of_count) {
    s->night_of_count = night;
    s->night_count = 0;
  }
  if (!in->worn || in->hr_bpm == 0) {
    s->stressed_run = 0;
    return false;
  }

  if (in->asleep) {
    s->stressed_run = 0;
    const bool known = s->sleep_n >= 20;
    const bool arousal = in->motion > c->arousal_motion ||
                         (known && in->hr_bpm * FX > s->sleep_hr + c->arousal_hr_over * FX);
    if (!arousal && in->motion <= c->still_motion) {
      prv_baseline(&s->sleep_hr, &s->sleep_n, in->hr_bpm);
    }
    if (c->sleep_enabled && known && arousal && s->night_count < c->sleep_max_per_night &&
        (!s->fired_sleep || in->t_min - s->last_sleep_min >= c->sleep_cooldown_min)) {
      s->fired_sleep = true;
      s->last_sleep_min = in->t_min;
      s->night_count++;
      if (req) {
        *req = (VibeRequest){.program = VibeProgram_Sleep, .max_ms = c->sleep_ms};
      }
      return true;
    }
    return false;
  }

  // awake: only judge minutes at rest (exercise raises heart rate for good reasons)
  if (in->motion > c->still_motion) {
    s->stressed_run = 0;
    return false;
  }
  const bool hr_known = s->rest_n >= 30;
  const bool hrv_known = s->hrv_n >= 15;
  const bool hr_high = hr_known && in->hr_bpm * FX > s->rest_hr + c->stress_hr_over * FX;
  const bool hrv_low = hrv_known && in->rmssd_ms &&
                       (int32_t)in->rmssd_ms * FX * 100 < s->rest_rmssd * c->stress_hrv_pct;
  const bool stressed = hr_high || hrv_low;
  if (!stressed) {
    prv_baseline(&s->rest_hr, &s->rest_n, in->hr_bpm);
    if (in->rmssd_ms) {
      prv_baseline(&s->rest_rmssd, &s->hrv_n, in->rmssd_ms);
    }
    s->stressed_run = 0;
    return false;
  }
  if (s->stressed_run < 255) {
    s->stressed_run++;
  }
  if (c->day_enabled && s->stressed_run >= c->stress_minutes && s->day_count < c->day_max_per_day &&
      (!s->fired_day || in->t_min - s->last_day_min >= c->day_cooldown_min)) {
    s->fired_day = true;
    s->last_day_min = in->t_min;
    s->day_count++;
    s->stressed_run = 0;
    if (req) {
      *req = (VibeRequest){.program = VibeProgram_Calm, .max_ms = c->calm_ms};
    }
    return true;
  }
  return false;
}

void smart_vibes_baselines(const SmartVibes *s, uint8_t *rest_hr, uint8_t *sleep_hr,
                           uint16_t *rest_rmssd) {
  if (rest_hr) {
    *rest_hr = s->rest_n ? (uint8_t)(s->rest_hr / FX) : 0;
  }
  if (sleep_hr) {
    *sleep_hr = s->sleep_n ? (uint8_t)(s->sleep_hr / FX) : 0;
  }
  if (rest_rmssd) {
    *rest_rmssd = s->hrv_n ? (uint16_t)(s->rest_rmssd / FX) : 0;
  }
}
