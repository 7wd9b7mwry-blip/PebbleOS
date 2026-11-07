/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! Decisions, separate from the sensing and the motor: which vibration to play and when.
//!  - Habit response: a detected episode gets a light alert at once, escalating while it continues.
//!  - Smart vibes: per-minute heart rate / HRV / motion against the wearer's own baselines; a calming
//!    program when the body looks stressed at rest during the day, a sleep program when sleep looks
//!    disturbed at night. Rate-limited, with daily caps.
//! Pure and OS-independent.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pbl/services/habit/habit_detect.h>
#include <pbl/services/habit/vibe_program.h>

typedef struct {
  VibeProgramId program;
  uint32_t max_ms;  //!< play-time budget (0: one pass)
} VibeRequest;

// ---- habit response -------------------------------------------------------------------------

typedef enum {
  HabitResponseMode_Off = 0,  //!< detect and log only
  HabitResponseMode_Vibrate,
} HabitResponseMode;

typedef struct {
  HabitResponseMode mode;
  uint16_t escalate_after_ms;  //!< episode still going this long after an alert: next level
  uint32_t max_alert_ms;       //!< stop alerting an episode after this long (it was a choice)
} HabitResponseConfig;

typedef struct {
  HabitResponseConfig cfg;
  uint8_t level;          //!< 0 = no alert yet this episode, 1-3
  uint32_t last_alert_ms;
  uint32_t episode_start_ms;
  uint32_t alerts_total;
} HabitResponse;

void habit_response_default_config(HabitResponseConfig *cfg);
void habit_response_init(HabitResponse *r, const HabitResponseConfig *cfg);

//! Feed the detector's event; returns true and fills `req` when an alert should play now.
bool habit_response_on_event(HabitResponse *r, uint32_t t_ms, HabitEvent e, const HabitEpisode *ep,
                             VibeRequest *req);

// ---- smart vibes ------------------------------------------------------------------------------

typedef struct {
  uint32_t t_min;      //!< minutes (monotonic, e.g. since epoch)
  uint16_t minute_of_day;
  bool worn;
  bool asleep;
  uint8_t hr_bpm;      //!< 0 = no reading this minute
  uint16_t rmssd_ms;   //!< 0 = no HRV this minute
  uint16_t motion;     //!< activity level this minute (e.g. mean |dynamic accel| in mG)
} SmartVibeInput;

typedef struct {
  bool day_enabled;
  bool sleep_enabled;
  uint16_t still_motion;         //!< motion at or below this counts as at rest
  uint8_t stress_hr_over;        //!< bpm over the resting baseline at rest = stressed
  uint8_t stress_hrv_pct;        //!< RMSSD under this % of baseline = stressed
  uint8_t stress_minutes;        //!< consecutive stressed minutes before acting
  uint16_t day_cooldown_min;
  uint8_t day_max_per_day;
  uint16_t arousal_motion;       //!< motion over this while asleep = an arousal
  uint8_t arousal_hr_over;       //!< and/or bpm over the sleeping baseline
  uint16_t sleep_cooldown_min;
  uint8_t sleep_max_per_night;
  uint32_t calm_ms;
  uint32_t sleep_ms;
} SmartVibeConfig;

typedef struct {
  SmartVibeConfig cfg;
  // baselines, x16 fixed point
  int32_t rest_hr;
  int32_t sleep_hr;
  int32_t rest_rmssd;
  uint16_t rest_n, sleep_n, hrv_n;
  uint8_t stressed_run;
  uint32_t last_day_min, last_sleep_min;
  uint8_t day_count, night_count;
  uint16_t day_of_count;    //!< local day the day counter belongs to
  uint16_t night_of_count;  //!< local night (noon to noon) the night counter belongs to
  bool fired_day, fired_sleep;
} SmartVibes;

void smart_vibes_default_config(SmartVibeConfig *cfg);
void smart_vibes_init(SmartVibes *s, const SmartVibeConfig *cfg);

//! Feed one minute; returns true and fills `req` when a program should start.
bool smart_vibes_on_minute(SmartVibes *s, const SmartVibeInput *in, VibeRequest *req);

//! Baselines learned so far (bpm, ms), 0 if not yet known.
void smart_vibes_baselines(const SmartVibes *s, uint8_t *rest_hr, uint8_t *sleep_hr,
                           uint16_t *rest_rmssd);
