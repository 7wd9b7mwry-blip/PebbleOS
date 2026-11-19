/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! Air log: the habit service's records (episodes, vibes, per-minute vitals) broadcast in the
//! presence beacon, for the room receivers (ESPHome Bluetooth proxies -> Home Assistant -> the
//! database), since the iOS Pebble app drops data logging. The receivers hear only some
//! advertisements and only at home, so the log keeps the last records in a ring: a new record is
//! sent `new_sends` times (interleaved with the other new ones), then the whole ring is replayed
//! round robin, which fills in time away from home once back. The receiver dedupes by (utc, kind,
//! seq). Pure: the caller owns the buffer.
//!
//! On air (31 bytes): flags; manufacturer data, company 0xFFFF: 'P', version, newest seq (u16),
//! then one record: seq (u16), utc (u32), kind (u8), data (13 bytes). Little endian.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pbl/kernel/compiler.h>

#define AIR_LOG_DATA 13
#define AIR_LOG_ADV_LEN 31
#define AIR_LOG_MAGIC 0x50
#define AIR_LOG_VERSION 1

typedef enum {
  AirKind_Boot = 1,          //!< a: reason (0 start), c: firmware build (low 16 bits of the git hash)
  AirKind_EpisodeStart = 2,  //!< a: region, b: confidence, e: dwell ms
  AirKind_EpisodeEnd = 3,    //!< a: region, b: confidence, e: duration ms
  AirKind_Play = 4,          //!< a: program, b: AirSource, c: alert level, e: budget ms
  AirKind_PlayEnd = 5,       //!< a: program, b: AirEnd, e: played ms (wave programs only)
  AirKind_Holdout = 6,       //!< a: program, b: AirSource, e: budget ms: a trial vibe not played
  AirKind_Minute = 7,        //!< AirMinute
  AirKind_Posture = 8,       //!< a: readings, b: slouched, c: mean cos + 1000, d (data 5-6): min cos + 1000, e: flags
  AirKind_PostureAlert = 9,  //!< c: cos + 1000
  AirKind_Quiet = 10,        //!< c: minutes (0: alerts on again)
  AirKind_Label = 11,        //!< a: 1 habit, 2 false alarm
  AirKind_Grip = 12,         //!< a: grip (1 controller, 2 phone; 0: forget them all), b: seconds held,
                             //!< c, d: mean gravity x, y (mG, int16), e: z (int16, low half) | mean fine motion << 16
} AirKind;

typedef enum {
  AirSource_Alert = 1,    //!< a habit alert
  AirSource_Smart = 2,    //!< smart vibes (heart rate / HRV / sleep)
  AirSource_App = 3,      //!< the Vibes app on the watch
  AirSource_Command = 4,  //!< Home Assistant ("PDB vibe ...")
  AirSource_Posture = 5,
  AirSource_Console = 6,
  AirSource_Chore = 7,    //!< a chore notification from Home Assistant (title "Chore...")
} AirSource;

typedef enum {
  AirEnd_Done = 1,       //!< ran its time
  AirEnd_Stopped = 2,    //!< stopped (app, command)
  AirEnd_Preempted = 3,  //!< a notification / alarm / call took the motor
  AirEnd_Replaced = 4,   //!< another wave program started
} AirEnd;

//! The per-minute record's data (13 bytes)
typedef struct PBL_PACKED {
  uint8_t hr;            //!< bpm, 0: none this minute
  uint8_t rmssd;         //!< ms (255 = 255 or more), 0: none
  uint16_t lux;
  uint8_t state;         //!< sleep state (bits 0-2) | off wrist << 3 | quiet << 4 | playing << 5 | gyro used << 6
  uint8_t motion;        //!< mean fine motion, mG (255 = 255 or more)
  uint8_t steps;         //!< steps this minute (255 = 255 or more)
  uint8_t sound_rms;     //!< dBFS + 128 (0: no reading)
  uint8_t sound_peak;    //!< dBFS + 128
  int8_t gx, gy, gz;     //!< mean gravity direction, mG / 8
  uint8_t near_s;        //!< seconds with the hand at a habit region
} AirMinute;

typedef struct PBL_PACKED {
  uint16_t seq;
  uint32_t utc;
  uint8_t kind;
  uint8_t data[AIR_LOG_DATA];
} AirRecord;

typedef struct {
  AirRecord *recs;
  uint8_t *sends;
  uint16_t cap;
  uint16_t count;       //!< records in the ring
  uint16_t head;        //!< index of the next write
  uint16_t next_seq;
  uint16_t fresh;       //!< records at the tail still being sent as new
  uint16_t fresh_pos;   //!< round robin among them (0 .. fresh - 1, from the oldest)
  uint16_t replay_pos;  //!< round robin over the ring (0 .. count - 1, from the oldest)
  uint8_t new_sends;
} AirLog;

void air_log_init(AirLog *log, AirRecord *recs, uint8_t *sends, uint16_t cap, uint8_t new_sends);

//! Add a record (the oldest drops out when full); returns its seq.
uint16_t air_log_push(AirLog *log, uint32_t utc, uint8_t kind, const void *data, size_t len);

//! An event record: a, b (u8), c (u16), d (u16), e (u32) in the data, in that order.
uint16_t air_log_event(AirLog *log, uint32_t utc, uint8_t kind, uint8_t a, uint8_t b, uint16_t c,
                       uint16_t d, uint32_t e);

//! The record to send next (new ones first, then the replay); false if the log is empty.
bool air_log_next(AirLog *log, AirRecord *out);

//! The advertisement for a record (AIR_LOG_ADV_LEN bytes).
void air_log_encode(const AirLog *log, const AirRecord *rec, uint8_t adv[AIR_LOG_ADV_LEN]);
