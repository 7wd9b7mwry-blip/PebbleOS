/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/habit/posture.h>

#include <string.h>

#define FX 16

bool minew_acc_parse(const uint8_t *ad, size_t len, MinewAcc *out) {
  size_t i = 0;
  while (i + 1 < len) {
    const uint8_t n = ad[i];
    if (n == 0 || i + 1 + n > len) {
      return false;
    }
    const uint8_t type = ad[i + 1];
    const uint8_t *v = &ad[i + 2];
    const size_t vlen = n - 1;
    // service data, 16-bit UUID 0xFFE1 (little endian), frame 0xA1, version 0x03
    if (type == 0x16 && vlen >= 2 + 2 + 1 + 6 && v[0] == 0xE1 && v[1] == 0xFF && v[2] == 0xA1 &&
        v[3] == 0x03) {
      const uint8_t *p = &v[4];
      out->battery_pct = p[0];
      int16_t raw[3];
      for (int a = 0; a < 3; a++) {
        raw[a] = (int16_t)((uint16_t)(p[1 + 2 * a] << 8) | p[2 + 2 * a]);  // big endian
      }
      // 8.8 fixed point g -> mG
      out->acc_mg = (HabitVec){(int16_t)((int32_t)raw[0] * 1000 / 256),
                               (int16_t)((int32_t)raw[1] * 1000 / 256),
                               (int16_t)((int32_t)raw[2] * 1000 / 256)};
      if (vlen >= 4 + 1 + 6 + 6) {
        memcpy(out->mac, &p[7], 6);
      } else {
        memset(out->mac, 0, 6);
      }
      return true;
    }
    i += 1 + n;
  }
  return false;
}

void posture_default_config(PostureConfig *cfg) {
  *cfg = (PostureConfig){
      .slouch_cos_x1000 = 966,   // 15 degrees
      .recline_cos_x1000 = 500,  // 60 degrees
      .slouch_s = 60,
      .repeat_s = 300,
      .gap_s = 60,
      .ref_n = 12,
  };
}

static void prv_minute_reset(PostureDetector *d) {
  d->min_readings = 0;
  d->min_slouched = 0;
  d->min_cos_sum = 0;
  d->min_cos_min = 1000;
}

void posture_init(PostureDetector *d, const PostureConfig *cfg) {
  memset(d, 0, sizeof(*d));
  d->cfg = *cfg;
  prv_minute_reset(d);
}

void posture_forget_reference(PostureDetector *d) {
  d->calibrated = false;
  d->ref_count = 0;
  memset(d->ref, 0, sizeof(d->ref));
  d->state = PostureState_Unknown;
  d->alerted = false;
}

static int64_t prv_isqrt(int64_t v) {
  if (v <= 0) {
    return 0;
  }
  int64_t x = v, y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + v / x) / 2;
  }
  return x;
}

static int16_t prv_cos_x1000(const int32_t ref[3], const HabitVec *a) {
  const int64_t ax = a->x, ay = a->y, az = a->z;
  const int64_t nr = prv_isqrt((int64_t)ref[0] * ref[0] + (int64_t)ref[1] * ref[1] +
                               (int64_t)ref[2] * ref[2]);
  const int64_t na = prv_isqrt(ax * ax + ay * ay + az * az);
  if (nr == 0 || na == 0) {
    return 1000;
  }
  int64_t c = ((ref[0] * ax + ref[1] * ay + ref[2] * az) * 1000) / (nr * na);
  c = c > 1000 ? 1000 : (c < -1000 ? -1000 : c);
  return (int16_t)c;
}

bool posture_feed(PostureDetector *d, uint32_t t_s, const HabitVec *acc_mg, bool walking) {
  const bool gap = d->last_reading_s && t_s - d->last_reading_s > d->cfg.gap_s;
  d->last_reading_s = t_s;
  if (gap) {
    d->slouch_since_s = 0;
    d->alerted = false;
  }
  const int32_t a[3] = {acc_mg->x * FX, acc_mg->y * FX, acc_mg->z * FX};
  if (walking) {
    // learn the upright reference: a running mean over the last ~ref_n walking readings
    const int32_t n = d->ref_count < d->cfg.ref_n ? d->ref_count + 1 : d->cfg.ref_n;
    for (int i = 0; i < 3; i++) {
      d->ref[i] = d->ref_count == 0 ? a[i] : d->ref[i] + (a[i] - d->ref[i]) / n;
    }
    if (d->ref_count < 255) {
      d->ref_count++;
    }
    d->calibrated = d->ref_count >= 3;
    d->slouch_since_s = 0;
    d->alerted = false;
    return false;
  }
  if (!d->calibrated) {
    d->state = PostureState_Unknown;
    return false;
  }
  const int16_t c = prv_cos_x1000(d->ref, acc_mg);
  d->last_cos_x1000 = c;
  d->min_readings++;
  d->min_cos_sum += c;
  d->min_cos_min = c < d->min_cos_min ? c : d->min_cos_min;

  if (c < d->cfg.recline_cos_x1000) {
    d->state = PostureState_Reclined;  // lying / leaning far back: not judged
    d->slouch_since_s = 0;
    d->alerted = false;
    return false;
  }
  if (c >= d->cfg.slouch_cos_x1000) {
    d->state = PostureState_Upright;
    d->slouch_since_s = 0;
    d->alerted = false;
    return false;
  }
  d->state = PostureState_Slouched;
  d->min_slouched++;
  if (d->slouch_since_s == 0) {
    d->slouch_since_s = t_s;
    return false;
  }
  if (t_s - d->slouch_since_s < d->cfg.slouch_s) {
    return false;
  }
  if (!d->alerted || t_s - d->last_alert_s >= d->cfg.repeat_s) {
    d->alerted = true;
    d->last_alert_s = t_s;
    return true;
  }
  return false;
}

void posture_take_minute(PostureDetector *d, PostureMinute *out) {
  out->readings = d->min_readings;
  out->slouched = d->min_slouched;
  out->mean_cos_x1000 = d->min_readings ? (int16_t)(d->min_cos_sum / d->min_readings) : 0;
  out->min_cos_x1000 = d->min_readings ? d->min_cos_min : 0;
  out->calibrated = d->calibrated;
  prv_minute_reset(d);
}
