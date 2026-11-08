/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <clar.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pbl/services/habit/posture.h>

// ---- Minew accelerometer frame -----------------------------------------------------------------

// flags; service data 0xFFE1: A1 03, battery 100 %, X -0.0117 g, Y 0.0234 g, Z 1.016 g, MAC
static const uint8_t s_frame[] = {
    0x02, 0x01, 0x06,
    0x12, 0x16, 0xE1, 0xFF, 0xA1, 0x03, 0x64,
    0xFF, 0xFD, 0x00, 0x06, 0x01, 0x04,
    0xAC, 0x23, 0x3F, 0x11, 0x22, 0x33,
};

void test_posture__parse_minew_acc_frame(void) {
  MinewAcc m;
  cl_assert(minew_acc_parse(s_frame, sizeof(s_frame), &m));
  cl_assert_equal_i(m.battery_pct, 100);
  cl_assert_equal_i(m.acc_mg.x, -11);   // -3/256 g
  cl_assert_equal_i(m.acc_mg.y, 23);    // 6/256 g
  cl_assert_equal_i(m.acc_mg.z, 1015);  // 260/256 g
  cl_assert_equal_i(m.mac[0], 0xAC);
  cl_assert_equal_i(m.mac[5], 0x33);
}

void test_posture__other_frames_are_ignored(void) {
  MinewAcc m;
  uint8_t ibeacon[] = {0x02, 0x01, 0x06, 0x1A, 0xFF, 0x4C, 0x00, 0x02, 0x15, 1, 2, 3, 4, 5, 6, 7,
                       8, 9, 10, 11, 12, 13, 14, 15, 16, 0, 1, 0, 2, 0xC5};
  cl_assert(!minew_acc_parse(ibeacon, sizeof(ibeacon), &m));
  uint8_t info[sizeof(s_frame)];
  memcpy(info, s_frame, sizeof(info));
  info[7] = 0xA2;  // another Minew frame type
  cl_assert(!minew_acc_parse(info, sizeof(info), &m));
  // truncated / malformed
  cl_assert(!minew_acc_parse(s_frame, 12, &m));
  uint8_t bad[] = {0x05, 0x16};
  cl_assert(!minew_acc_parse(bad, sizeof(bad), &m));
}

// ---- posture -----------------------------------------------------------------------------------

static PostureDetector s_d;
static uint32_t s_t;

// upright: gravity along -y of the tag (as mounted); tilt by `deg` forward about x
static HabitVec prv_tilt(double deg) {
  const double r = deg * 3.14159265 / 180;
  return (HabitVec){(int16_t)(0), (int16_t)(-1000 * cos(r)), (int16_t)(1000 * sin(r))};
}

//! readings every `step_s` for `secs`; returns the number of alerts
static int prv_run(double deg, uint32_t secs, uint32_t step_s, bool walking) {
  int alerts = 0;
  for (uint32_t t = 0; t < secs; t += step_s) {
    HabitVec a = prv_tilt(deg);
    if (walking) {  // walking jiggles the readings
      a.x += (int16_t)((rand() % 81) - 40);
      a.z += (int16_t)((rand() % 81) - 40);
    }
    alerts += posture_feed(&s_d, s_t, &a, walking);
    s_t += step_s;
  }
  return alerts;
}

void test_posture__initialize(void) {
  PostureConfig cfg;
  posture_default_config(&cfg);
  posture_init(&s_d, &cfg);
  s_t = 1000;
  srand(7);
}

void test_posture__no_judging_before_walking(void) {
  cl_assert_equal_i(prv_run(30, 600, 10, false), 0);
  cl_assert(!s_d.calibrated);
  cl_assert_equal_i(s_d.state, PostureState_Unknown);
}

void test_posture__sustained_slouch_alerts_and_repeats(void) {
  cl_assert_equal_i(prv_run(0, 120, 10, true), 0);  // walk: learns upright
  cl_assert(s_d.calibrated);
  cl_assert_equal_i(prv_run(5, 300, 10, false), 0);  // sitting upright (5 deg): fine
  cl_assert_equal_i(s_d.state, PostureState_Upright);
  // slouched 25 deg: the first alert after 60 s, then every 5 min
  cl_assert_equal_i(prv_run(25, 50, 10, false), 0);
  cl_assert_equal_i(prv_run(25, 20, 10, false), 1);
  cl_assert_equal_i(prv_run(25, 290, 10, false), 0);
  cl_assert_equal_i(prv_run(25, 20, 10, false), 1);
  // sitting up resets it
  cl_assert_equal_i(prv_run(3, 30, 10, false), 0);
  cl_assert_equal_i(prv_run(25, 50, 10, false), 0);
}

void test_posture__reclining_is_not_slouching(void) {
  prv_run(0, 120, 10, true);
  cl_assert_equal_i(prv_run(75, 900, 10, false), 0);  // lying back / on the couch
  cl_assert_equal_i(s_d.state, PostureState_Reclined);
}

void test_posture__brief_leans_and_gaps(void) {
  prv_run(0, 120, 10, true);
  for (int i = 0; i < 10; i++) {  // leaning in for 40 s, then back: never an alert
    cl_assert_equal_i(prv_run(25, 40, 10, false), 0);
    cl_assert_equal_i(prv_run(2, 20, 10, false), 0);
  }
  // readings missing for 2 min in the middle of a slouch: the timer starts over
  cl_assert_equal_i(prv_run(25, 50, 10, false), 0);
  s_t += 120;
  cl_assert_equal_i(prv_run(25, 50, 10, false), 0);
}

void test_posture__minute_summary_and_forget(void) {
  prv_run(0, 120, 10, true);
  prv_run(5, 30, 10, false);
  prv_run(25, 30, 10, false);
  PostureMinute m;
  posture_take_minute(&s_d, &m);
  cl_assert_equal_i(m.readings, 6);
  cl_assert_equal_i(m.slouched, 3);
  cl_assert(m.calibrated);
  cl_assert(m.min_cos_x1000 < 920 && m.min_cos_x1000 > 890);  // cos 25 deg = 906
  posture_take_minute(&s_d, &m);
  cl_assert_equal_i(m.readings, 0);
  posture_forget_reference(&s_d);
  cl_assert(!s_d.calibrated);
}

// ---- scan scheduling ---------------------------------------------------------------------------

static PostureScanner s_sc;

//! ticks every 10 s for `secs`; `reading` NULL: the tag is not heard. Returns the bursts started.
static int prv_ticks(uint32_t secs, const HabitVec *reading, bool jiggle) {
  int bursts = 0;
  for (uint32_t t = 0; t < secs; t += 10) {
    if (posture_scan_due(&s_sc, s_t)) {
      bursts++;
      HabitVec a;
      if (reading) {
        a = *reading;
        if (jiggle) {
          a.x += (int16_t)((rand() % 121) - 60);
        }
      }
      posture_scan_result(&s_sc, s_t, reading ? &a : NULL);
    }
    s_t += 10;
  }
  return bursts;
}

static void prv_scan_init(void) {
  PostureScanConfig cfg;
  posture_scan_default_config(&cfg);
  posture_scan_init(&s_sc, &cfg);
}

void test_posture__worn_tag_is_read_every_10_s(void) {
  prv_scan_init();
  const HabitVec a = prv_tilt(5);
  cl_assert_equal_i(prv_ticks(1800, &a, true), 180);  // 30 min, moving a little
  cl_assert_equal_i(s_sc.mode, PostureScan_Active);
}

void test_posture__absent_tag_is_checked_once_a_minute(void) {
  prv_scan_init();
  cl_assert_equal_i(prv_ticks(30, NULL, false), 3);  // three misses ...
  cl_assert_equal_i(s_sc.mode, PostureScan_Lost);
  cl_assert_equal_i(prv_ticks(600, NULL, false), 10);  // ... then once a minute
  // the strap goes on: back to every 10 s at the next check
  const HabitVec a = prv_tilt(5);
  prv_ticks(60, &a, true);
  cl_assert_equal_i(s_sc.mode, PostureScan_Active);
  cl_assert_equal_i(prv_ticks(600, &a, true), 60);
}

void test_posture__still_tag_parks_and_wakes_on_movement(void) {
  prv_scan_init();
  const HabitVec a = prv_tilt(5);
  cl_assert_equal_i(prv_ticks(600, &a, false), 60);  // lying on a chair: 10 min at full rate
  cl_assert_equal_i(prv_ticks(10, &a, false), 1);
  cl_assert_equal_i(s_sc.mode, PostureScan_Parked);
  cl_assert_equal_i(prv_ticks(1200, &a, false), 20);  // then once a minute
  // picked up: the next reading moves, full rate again
  const HabitVec b = prv_tilt(30);
  prv_ticks(60, &b, false);
  cl_assert_equal_i(s_sc.mode, PostureScan_Active);
  cl_assert_equal_i(prv_ticks(100, &b, true), 10);
}

void test_posture__parked_readings_still_judge_a_slouch(void) {
  // once-a-minute readings (a very still wearer) must not count as gaps in a slouch
  prv_run(0, 120, 10, true);
  cl_assert_equal_i(prv_run(25, 60, 60, false), 0);
  cl_assert_equal_i(prv_run(25, 60, 60, false), 1);
}

void test_posture__reset_keeps_the_schedule(void) {
  prv_scan_init();
  prv_ticks(30, NULL, false);
  posture_scan_reset(&s_sc);  // a new tag: fast again, same configuration
  cl_assert_equal_i(s_sc.mode, PostureScan_Active);
  cl_assert_equal_i(s_sc.cfg.slow_s, 60);
  cl_assert_equal_i(prv_ticks(30, NULL, false), 3);
  cl_assert_equal_i(prv_ticks(600, NULL, false), 10);
}
