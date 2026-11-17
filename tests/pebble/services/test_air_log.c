/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <clar.h>

#include <string.h>

#include <pbl/services/habit/air_log.h>

#define CAP 8

static AirRecord s_recs[CAP];
static uint8_t s_sends[CAP];
static AirLog s_log;

void test_air_log__initialize(void) {
  air_log_init(&s_log, s_recs, s_sends, CAP, 3);
}

void test_air_log__empty_sends_nothing(void) {
  AirRecord r;
  cl_assert(!air_log_next(&s_log, &r));
}

void test_air_log__new_records_go_out_interleaved_then_replay(void) {
  air_log_event(&s_log, 100, AirKind_EpisodeStart, 1, 80, 0, 0, 500);
  air_log_event(&s_log, 101, AirKind_EpisodeEnd, 1, 80, 0, 0, 9000);
  AirRecord r;
  int seen[2] = {0};
  for (int i = 0; i < 6; i++) {
    cl_assert(air_log_next(&s_log, &r));
    seen[r.seq]++;
  }
  cl_assert_equal_i(seen[0], 3);
  cl_assert_equal_i(seen[1], 3);
  cl_assert_equal_i(s_log.fresh, 0);
  // then the replay: oldest first, round robin
  cl_assert(air_log_next(&s_log, &r));
  cl_assert_equal_i(r.seq, 0);
  cl_assert(air_log_next(&s_log, &r));
  cl_assert_equal_i(r.seq, 1);
  cl_assert(air_log_next(&s_log, &r));
  cl_assert_equal_i(r.seq, 0);
  // a new record jumps ahead of the replay
  air_log_event(&s_log, 102, AirKind_Quiet, 0, 0, 30, 0, 0);
  cl_assert(air_log_next(&s_log, &r));
  cl_assert_equal_i(r.seq, 2);
}

void test_air_log__full_ring_keeps_the_newest(void) {
  for (int i = 0; i < CAP + 3; i++) {
    air_log_event(&s_log, 1000 + i, AirKind_Quiet, 0, 0, (uint16_t)i, 0, 0);
  }
  cl_assert_equal_i(s_log.count, CAP);
  AirRecord r;
  int seen[CAP + 3] = {0};
  for (int i = 0; i < CAP * 3; i++) {
    cl_assert(air_log_next(&s_log, &r));
    seen[r.seq]++;
  }
  for (int i = 0; i < 3; i++) {
    cl_assert_equal_i(seen[i], 0);  // dropped
  }
  for (int i = 3; i < CAP + 3; i++) {
    cl_assert_equal_i(seen[i], 3);
  }
  cl_assert(air_log_next(&s_log, &r));
  cl_assert_equal_i(r.seq, 3);  // replay from the oldest kept
}

void test_air_log__encodes_the_advertisement(void) {
  AirMinute m = {.hr = 62, .rmssd = 41, .lux = 300, .state = 2, .motion = 7, .steps = 0,
                 .sound_rms = 128 - 55, .sound_peak = 128 - 30, .gx = -10, .gy = 120, .gz = -30,
                 .near_s = 4};
  air_log_push(&s_log, 0x6550A0B0, AirKind_Minute, &m, sizeof(m));
  AirRecord r;
  cl_assert(air_log_next(&s_log, &r));
  uint8_t adv[AIR_LOG_ADV_LEN];
  air_log_encode(&s_log, &r, adv);
  const uint8_t head[] = {2, 1, 6, 27, 0xFF, 0xFF, 0xFF, 0x50, 1, 0, 0, 0, 0, 0xB0, 0xA0, 0x50, 0x65, 7};
  cl_assert(memcmp(adv, head, sizeof(head)) == 0);
  cl_assert_equal_i(adv[18], 62);
  cl_assert_equal_i(adv[20], 300 & 0xFF);
  cl_assert_equal_i(adv[21], 300 >> 8);
  cl_assert_equal_i((int8_t)adv[27], -10);
  cl_assert_equal_i(adv[30], 4);
}
