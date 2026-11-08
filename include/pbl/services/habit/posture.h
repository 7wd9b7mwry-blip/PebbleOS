/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! Back posture from a broadcasting accelerometer tag on the upper back (Minew E8 / accelerometer
//! frame), read by the watch in short scan bursts. Pure and integer-only.
//!
//!  - minew_acc_parse(): the Minew accelerometer frame (service data 0xFFE1, frame 0xA1 v3:
//!    battery %, X / Y / Z as signed 8.8 fixed-point g, MAC).
//!  - posture_detect: the tag's gravity direction against an upright reference that is learned
//!    while walking (no manual calibration); "slouched" when tilted beyond a threshold but less
//!    than a reclining / lying angle; an alert after a sustained slouch, repeated while it lasts.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pbl/services/habit/habit_detect.h>

typedef struct {
  uint8_t battery_pct;
  HabitVec acc_mg;  //!< tag axes, mG
  uint8_t mac[6];   //!< as in the frame (most significant byte first)
} MinewAcc;

//! Parse a Minew accelerometer frame from advertising data; false if there is none.
bool minew_acc_parse(const uint8_t *ad, size_t len, MinewAcc *out);

typedef struct {
  int16_t slouch_cos_x1000;   //!< tilted beyond this (cos x 1000) from upright = slouched
  int16_t recline_cos_x1000;  //!< tilted beyond this = reclining / lying, not a slouch
  uint16_t slouch_s;          //!< sustained this long before the first alert
  uint16_t repeat_s;          //!< then again every this long while it lasts
  uint16_t gap_s;             //!< no reading this long: the slouch timer starts over
  uint8_t ref_n;              //!< walking readings averaged into the upright reference
} PostureConfig;

typedef enum {
  PostureState_Unknown = 0,  //!< no upright reference yet (not walked since wearing it)
  PostureState_Upright,
  PostureState_Slouched,
  PostureState_Reclined,
} PostureState;

typedef struct {
  PostureConfig cfg;
  int32_t ref[3];      //!< upright reference, mG x 16
  uint8_t ref_count;
  bool calibrated;
  PostureState state;
  int16_t last_cos_x1000;
  uint32_t last_reading_s;
  uint32_t slouch_since_s;
  uint32_t last_alert_s;
  bool alerted;
  // per-minute summary
  uint16_t min_readings;
  uint16_t min_slouched;
  int32_t min_cos_sum;
  int16_t min_cos_min;
} PostureDetector;

void posture_default_config(PostureConfig *cfg);
void posture_init(PostureDetector *d, const PostureConfig *cfg);

//! One reading (the tag's acceleration, mG) at `t_s` seconds. `walking`: the wearer is walking
//! now (from the watch), so the reading teaches the upright reference instead of being judged.
//! Returns true when an alert should play now.
bool posture_feed(PostureDetector *d, uint32_t t_s, const HabitVec *acc_mg, bool walking);

//! The reference changes when the tag is re-mounted: forget it (it is learned again when walking).
void posture_forget_reference(PostureDetector *d);

typedef struct {
  uint16_t readings;
  uint16_t slouched;     //!< readings judged slouched
  int16_t mean_cos_x1000;
  int16_t min_cos_x1000;
  bool calibrated;
} PostureMinute;

//! The summary since the last call (then reset).
void posture_take_minute(PostureDetector *d, PostureMinute *out);

// ---- scan scheduling (battery) ------------------------------------------------------------------
//
// Listening for the tag is the costliest radio work, so bursts are frequent only while the tag is
// heard and being worn: when it is not heard for a few bursts (strap off, out of range) or its
// readings have not moved at all for a while (strap on a chair), it is checked once a minute
// instead, and any movement brings the fast rate back.

typedef enum {
  PostureScan_Active = 0,  //!< heard and moving: every period_s
  PostureScan_Lost,        //!< not heard for lost_after bursts: every slow_s
  PostureScan_Parked,      //!< heard but perfectly still for parked_after_s: every slow_s
} PostureScanMode;

typedef struct {
  uint16_t period_s;        //!< burst interval while active
  uint16_t slow_s;          //!< burst interval while lost or parked
  uint8_t lost_after;       //!< bursts in a row without the tag
  uint16_t parked_after_s;  //!< no reading moved more than still_mg from the last for this long
  uint16_t still_mg;
} PostureScanConfig;

typedef struct {
  PostureScanConfig cfg;
  PostureScanMode mode;
  uint8_t missed;
  uint32_t last_burst_s;
  bool scanned;
  bool have_still;
  HabitVec still_ref;
  uint32_t still_since_s;
} PostureScanner;

void posture_scan_default_config(PostureScanConfig *cfg);
void posture_scan_init(PostureScanner *sc, const PostureScanConfig *cfg);

//! Back to the fast rate with the same configuration (a new tag was set).
void posture_scan_reset(PostureScanner *sc);

//! Called on every tick of the (period_s) timer: true when a burst should start now.
bool posture_scan_due(PostureScanner *sc, uint32_t now_s);

//! A burst ended: `acc_mg` is the tag's reading, or NULL if the tag was not heard.
void posture_scan_result(PostureScanner *sc, uint32_t now_s, const HabitVec *acc_mg);
