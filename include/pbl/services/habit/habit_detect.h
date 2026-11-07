/* SPDX-FileCopyrightText: 2026 Daniel Allex */
/* SPDX-License-Identifier: Apache-2.0 */

//! Hand-to-head habit detection (nail biting, skin picking, hair / nose picking) from the wrist IMU.
//!
//! Pure, integer-only and OS-independent: the habit service feeds it samples, the unit tests feed it
//! recorded or synthetic ones. Per sample it tracks
//! - the gravity direction (low-passed accelerometer): where the forearm points, i.e. which part of
//!   the head the hand can be at; matched against per-region prototypes (set by calibration),
//! - gross motion (walking, gesturing): too much of it means the hand is not busy at the head,
//! - fine motion (the small, repetitive movements of picking or biting) from the accelerometer and,
//!   when the gyro runs, from wrist rotation.
//! An episode starts once the hand has stayed at one region with fine motion for `min_dwell_ms`
//! (brief gaps up to `release_ms` allowed) and ends after `release_ms` away.

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  HabitRegion_None = 0,
  HabitRegion_Mouth,   //!< nail biting, lip / cheek biting
  HabitRegion_Nose,
  HabitRegion_Face,    //!< skin picking on the cheeks, chin, forehead
  HabitRegion_Hair,    //!< hair picking / pulling, scalp
  HabitRegionCount,
} HabitRegion;

typedef struct {
  int16_t x, y, z;
} HabitVec;

typedef struct {
  bool enabled;
  HabitVec gravity;       //!< prototype gravity direction (any scale; mG from calibration)
  int16_t cos_min_x1000;  //!< match when cos(angle to the prototype) >= this / 1000
} HabitRegionProto;

typedef struct {
  uint16_t sample_hz;              //!< accelerometer (and gyro) sample rate
  HabitRegionProto regions[HabitRegionCount];
  uint16_t min_dwell_ms;           //!< hand at the region (with fine motion) this long to start
  uint16_t release_ms;             //!< away this long ends the episode / resets the candidate
  uint16_t min_fine_mg;            //!< fine motion needed (picking / biting is never fully still)
  uint16_t max_gross_mg;           //!< more gross motion than this: not a habit (walking, waving)
  uint16_t min_fine_gyro_dps10;    //!< with the gyro: wrist rotation needed (0.1 deg/s units)
  uint16_t min_gravity_mg;         //!< gravity magnitude sanity band (free fall / impacts)
  uint16_t max_gravity_mg;
} HabitConfig;

typedef enum {
  HabitEvent_None = 0,
  HabitEvent_Start,      //!< episode confirmed (after the dwell)
  HabitEvent_Ongoing,    //!< once a second while the episode lasts
  HabitEvent_End,
} HabitEvent;

typedef struct {
  HabitRegion region;
  uint32_t start_ms;       //!< when the hand arrived (the start of the dwell)
  uint32_t duration_ms;    //!< so far (Ongoing) or total (End)
  uint8_t confidence;      //!< 0-100: orientation margin and fine-motion level
} HabitEpisode;

typedef enum {
  HabitState_Idle = 0,
  HabitState_Candidate,
  HabitState_Active,
} HabitState;

typedef struct {
  HabitConfig cfg;
  bool primed;
  // filters, x16 fixed point (mG * 16, (0.1 deg/s) * 16)
  int32_t grav[3];
  int32_t fast[3];
  int32_t gross;
  int32_t fine;
  int32_t gyro_fine;
  bool have_gyro;
  // state machine
  HabitState state;
  HabitRegion region;
  uint32_t region_since_ms;
  uint32_t last_match_ms;
  uint32_t last_report_ms;
  uint32_t confidence_sum;
  uint32_t confidence_n;
} HabitDetector;

//! Defaults: left wrist, generic regions (to be replaced by the wearer's calibration).
void habit_detect_default_config(HabitConfig *cfg);

void habit_detect_init(HabitDetector *d, const HabitConfig *cfg);

//! Feed one sample. `gyro` may be NULL (gyro off); in 0.1 deg/s. `vibrating`: the motor was on
//! during this sample, so motion is ignored (the watch's own vibration is not the wearer's).
//! Returns the event and fills `out` for anything but HabitEvent_None.
HabitEvent habit_detect_feed(HabitDetector *d, uint32_t t_ms, const HabitVec *accel,
                             const HabitVec *gyro, bool vibrating, HabitEpisode *out);

//! The region the current gravity direction matches (or None), for calibration UIs and logging.
HabitRegion habit_detect_current_region(const HabitDetector *d);

//! Current filter outputs, for logging / tuning: gross and fine motion in mG, gyro fine in 0.1 dps.
void habit_detect_levels(const HabitDetector *d, HabitVec *gravity, uint16_t *gross_mg,
                         uint16_t *fine_mg, uint16_t *gyro_fine_dps10);

//! Calibration: store the current gravity direction as the prototype for `region`.
void habit_detect_calibrate_region(HabitDetector *d, HabitRegion region, int16_t cos_min_x1000);

const char *habit_region_name(HabitRegion region);
