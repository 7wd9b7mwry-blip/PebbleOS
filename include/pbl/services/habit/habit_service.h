/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! Habit service: always on, independent of the foreground app or watchface.
//!  - Detects hand-to-head habits from the accelerometer (habit_detect) and responds with
//!    escalating vibrations (habit_policy).
//!  - Smart vibes: once a minute, heart rate, HRV, motion and the sleep state against the
//!    wearer's baselines; plays calming / sleep programs (vibe_program), streamed to the motor.
//!  - Logs episodes, alerts, programs and a per-minute summary (heart rate, RMSSD, motion,
//!    ambient light, sleep state) to data logging (tag DlsSystemTagHabit) for the phone to forward.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pbl/kernel/compiler.h>
#include <pbl/services/habit/habit_detect.h>
#include <pbl/services/habit/vibe_program.h>

//! Data logging tag of the habit records (outside the range of the system tags, 78-87)
#define DlsSystemTagHabit 120
//! Data logging tag of the classifier windows near the head (HabitWindowRecord)
#define DlsSystemTagHabitWindow 121

typedef enum {
  HabitLogKind_EpisodeStart = 1,  //!< a: region, b: confidence, e: dwell ms
  HabitLogKind_EpisodeEnd = 2,    //!< a: region, b: confidence, e: duration ms
  HabitLogKind_Alert = 3,         //!< a: program, b: alert level
  HabitLogKind_SmartVibe = 4,     //!< a: program, e: budget ms
  HabitLogKind_Minute = 5,        //!< a: bpm, b: RMSSD ms, c: motion mG, d: lux, e: sleep state
  HabitLogKind_Label = 6,         //!< a: label (wearer marked an episode / false alarm)
  HabitLogKind_Sound = 7,         //!< b: RMS, c: peak (tenths of dBFS + 1000), e: samples
} HabitLogKind;

//! One logged record, 16 bytes
typedef struct PBL_PACKED {
  uint32_t utc;
  uint8_t kind;
  uint8_t a;
  uint16_t b;
  uint16_t c;
  uint16_t d;
  uint32_t e;
} HabitLogRecord;

//! One scored classifier window, 76 bytes: for training a personal model from the wearer's own
//! labelled episodes (labels are HabitLogKind_Label records of the other session)
typedef struct PBL_PACKED {
  uint32_t utc;
  uint8_t region;      //!< HabitRegion the hand was at
  uint8_t state;       //!< 0 idle, 1 candidate, 2 in an episode
  uint8_t gyro;        //!< the gyroscope ran
  uint8_t reserved;
  int32_t score;       //!< classifier score, logit x 1024
  int32_t features[16];
} HabitWindowRecord;

void habit_service_init(void);

//! Runlevel hook: start / stop the sensing.
void habit_service_set_enabled(bool enabled);

//! Play a program now (console, settings, a future app): for at most `max_ms` (0: one pass).
bool habit_service_play(VibeProgramId program, uint32_t max_ms);
void habit_service_stop(void);

//! Calibration: the hand is at `region` now; store the current forearm direction.
void habit_service_calibrate(HabitRegion region);

//! The wearer labels the last few seconds (1: "that was a habit", 2: "false alarm").
void habit_service_label(uint8_t label);

//! Settings, persisted in flash (with the calibration and the learned baselines).
void habit_service_set_vibrate(bool vibrate);
void habit_service_set_alerts_while_asleep(bool on);
//! Presence beacon: while connected to the phone, advertise a BTHome beacon (battery) every
//! second, so room sensors (ESPHome Bluetooth proxies, Home Assistant) can locate the watch.
void habit_service_set_beacon(bool on);
//! Sound level: one second a minute from the microphone, as a level only (no audio is kept).
void habit_service_set_sound(bool on);
//! Forget the calibrated regions (back to the generic "hand at the head" default).
void habit_service_reset_calibration(void);

typedef struct {
  bool running;
  HabitRegion region_now;
  HabitVec gravity;
  uint16_t gross_mg, fine_mg;
  uint32_t episodes, alerts, smart_vibes;
  uint8_t rest_hr, sleep_hr;
  uint16_t rest_rmssd;
  bool playing;
  VibeProgramId program;
  bool vibrate;
  bool alerts_while_asleep;
  uint8_t regions_calibrated;  //!< bit per HabitRegion
  bool beacon;
  bool beacon_airing;
  bool sound;
  int16_t sound_rms_db10;   //!< last minute's RMS level, tenths of dBFS
  int16_t sound_peak_db10;
} HabitServiceStatus;

void habit_service_get_status(HabitServiceStatus *status);
