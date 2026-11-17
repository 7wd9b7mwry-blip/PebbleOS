/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
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
  bool use_model;                  //!< episodes also need the classifier (habit_model) to agree
  int32_t model_threshold;         //!< score (logit x 1024) a window needs; the model's default
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

//! Motion window kept for the classifier: the last 3 s as 100 ms bins (HABIT_FEAT_WINDOW at
//! HABIT_FEAT_HZ)
#define HABIT_DETECT_WINDOW 30

typedef struct {
  HabitConfig cfg;
  bool primed;
  // classifier window (ring) and its latest result
  HabitVec win_acc[HABIT_DETECT_WINDOW];
  HabitVec win_gyro[HABIT_DETECT_WINDOW];
  HabitVec last_gyro;
  int32_t bin_acc[3], bin_gyro[3];
  uint8_t bin_n;
  uint32_t bin_id;        //!< t_ms / 100 of the bin being filled
  uint8_t win_head;
  uint8_t win_count;
  uint8_t since_score;
  bool scored;            //!< a window was scored since the hand came to the region
  bool model_ok;          //!< the latest window passed the classifier
  int32_t model_score;
  int32_t features[16];   //!< the latest window's features (HabitFeatCount)
  bool features_fresh;    //!< set when a window was scored; cleared by habit_detect_take_window
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

//! Feed one gyroscope sample (0.1 deg/s) when the gyro runs separately from the accelerometer
//! (at the same rate); `vibrating` as for habit_detect_feed().
void habit_detect_feed_gyro(HabitDetector *d, const HabitVec *gyro, bool vibrating);

//! The latest scored window (features and score), once: true if one was scored since the last
//! call. For logging the windows near the head for personal training.
bool habit_detect_take_window(HabitDetector *d, int32_t features[16], int32_t *score);

//! The gyro was turned off: forget its level until it runs again.
void habit_detect_gyro_off(HabitDetector *d);

//! True when the hand is at, or on its way into, an episode (where the gyro helps).
bool habit_detect_wants_gyro(const HabitDetector *d);

//! The region the current gravity direction matches (or None), for calibration UIs and logging.
HabitRegion habit_detect_current_region(const HabitDetector *d);

//! Current filter outputs, for logging / tuning: gross and fine motion in mG, gyro fine in 0.1 dps.
void habit_detect_levels(const HabitDetector *d, HabitVec *gravity, uint16_t *gross_mg,
                         uint16_t *fine_mg, uint16_t *gyro_fine_dps10);

//! Calibration: store the current gravity direction as the prototype for `region`.
void habit_detect_calibrate_region(HabitDetector *d, HabitRegion region, int16_t cos_min_x1000);

const char *habit_region_name(HabitRegion region);
