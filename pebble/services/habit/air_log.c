/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/habit/air_log.h>

#include <string.h>

_Static_assert(sizeof(AirMinute) == AIR_LOG_DATA, "the minute record fills the data");
_Static_assert(sizeof(AirRecord) == 20, "seq, utc, kind, data");

void air_log_init(AirLog *log, AirRecord *recs, uint8_t *sends, uint16_t cap, uint8_t new_sends) {
  memset(log, 0, sizeof(*log));
  log->recs = recs;
  log->sends = sends;
  log->cap = cap;
  log->new_sends = new_sends ? new_sends : 1;
}

// ring index of the i-th oldest record
static uint16_t prv_index(const AirLog *log, uint16_t i) {
  return (uint16_t)((log->head + log->cap - log->count + i) % log->cap);
}

uint16_t air_log_push(AirLog *log, uint32_t utc, uint8_t kind, const void *data, size_t len) {
  AirRecord *r = &log->recs[log->head];
  memset(r, 0, sizeof(*r));
  r->seq = log->next_seq++;
  r->utc = utc;
  r->kind = kind;
  memcpy(r->data, data, len > AIR_LOG_DATA ? AIR_LOG_DATA : len);
  log->sends[log->head] = 0;
  log->head = (uint16_t)((log->head + 1) % log->cap);
  if (log->count < log->cap) {
    log->count++;
  } else if (log->replay_pos > 0) {
    log->replay_pos--;  // the oldest dropped out: positions shift down
  }
  if (log->fresh < log->count) {
    log->fresh++;
  }
  return r->seq;
}

uint16_t air_log_event(AirLog *log, uint32_t utc, uint8_t kind, uint8_t a, uint8_t b, uint16_t c,
                       uint16_t d, uint32_t e) {
  const uint8_t data[10] = {a, b, (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)d, (uint8_t)(d >> 8),
                            (uint8_t)e, (uint8_t)(e >> 8), (uint8_t)(e >> 16), (uint8_t)(e >> 24)};
  return air_log_push(log, utc, kind, data, sizeof(data));
}

bool air_log_next(AirLog *log, AirRecord *out) {
  if (log->count == 0) {
    return false;
  }
  if (log->fresh > 0) {
    // the new records are the newest `fresh`: send them in turn until each went out new_sends times
    if (log->fresh_pos >= log->fresh) {
      log->fresh_pos = 0;
    }
    const uint16_t i = prv_index(log, (uint16_t)(log->count - log->fresh + log->fresh_pos));
    *out = log->recs[i];
    log->sends[i]++;
    log->fresh_pos++;
    // retire the oldest new ones that are done
    while (log->fresh > 0 &&
           log->sends[prv_index(log, (uint16_t)(log->count - log->fresh))] >= log->new_sends) {
      log->fresh--;
      if (log->fresh_pos > 0) {
        log->fresh_pos--;
      }
    }
    return true;
  }
  if (log->replay_pos >= log->count) {
    log->replay_pos = 0;
  }
  *out = log->recs[prv_index(log, log->replay_pos++)];
  return true;
}

void air_log_encode(const AirLog *log, const AirRecord *rec, uint8_t adv[AIR_LOG_ADV_LEN]) {
  const uint16_t newest = (uint16_t)(log->next_seq - 1);
  uint8_t *p = adv;
  *p++ = 2;
  *p++ = 0x01;
  *p++ = 0x06;
  *p++ = AIR_LOG_ADV_LEN - 4;  // this structure's length after the length byte
  *p++ = 0xFF;
  *p++ = 0xFF;
  *p++ = 0xFF;
  *p++ = AIR_LOG_MAGIC;
  *p++ = AIR_LOG_VERSION;
  *p++ = (uint8_t)newest;
  *p++ = (uint8_t)(newest >> 8);
  *p++ = (uint8_t)rec->seq;
  *p++ = (uint8_t)(rec->seq >> 8);
  *p++ = (uint8_t)rec->utc;
  *p++ = (uint8_t)(rec->utc >> 8);
  *p++ = (uint8_t)(rec->utc >> 16);
  *p++ = (uint8_t)(rec->utc >> 24);
  *p++ = rec->kind;
  memcpy(p, rec->data, AIR_LOG_DATA);
}
