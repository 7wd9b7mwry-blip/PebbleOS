/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/habit/habit_detect.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// Filter time constants
#define GRAVITY_TAU_MS 400
#define NOISE_TAU_MS 20
#define FINE_TAU_MS 500
#define GROSS_DECAY_TAU_MS 1000
#define ONGOING_PERIOD_MS 1000

#define FX 16  // fixed-point scale of the filter states

static int32_t prv_div(uint32_t tau_ms, uint16_t hz) {
  int32_t n = (int32_t)((tau_ms * hz) / 1000);
  return n < 1 ? 1 : n;
}

// one-pole low-pass: y += (x - y) / n, with x and y in FX units
static int32_t prv_lp(int32_t y, int32_t x, int32_t n) {
  return y + (x - y) / n;
}

static int64_t prv_dot(const int32_t a[3], const HabitVec *b) {
  return (int64_t)a[0] * b->x + (int64_t)a[1] * b->y + (int64_t)a[2] * b->z;
}

static int64_t prv_isqrt64(int64_t v) {
  if (v <= 0) {
    return 0;
  }
  // Newton's method; v fits easily (squares of a few thousand * FX)
  int64_t x = v, y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + v / x) / 2;
  }
  return x;
}

static int64_t prv_norm3(const int32_t a[3]) {
  return prv_isqrt64((int64_t)a[0] * a[0] + (int64_t)a[1] * a[1] + (int64_t)a[2] * a[2]);
}

static int64_t prv_norm_vec(const HabitVec *v) {
  return prv_isqrt64((int64_t)v->x * v->x + (int64_t)v->y * v->y + (int64_t)v->z * v->z);
}

//! cos(angle between the gravity estimate and the prototype) * 1000, or -1000 if undefined
static int32_t prv_cos_x1000(const HabitDetector *d, const HabitVec *proto) {
  const int64_t ng = prv_norm3(d->grav), np = prv_norm_vec(proto);
  if (ng == 0 || np == 0) {
    return -1000;
  }
  return (int32_t)((prv_dot(d->grav, proto) * 1000) / (ng * np));
}

static HabitRegion prv_match(const HabitDetector *d, int32_t *cos_out) {
  HabitRegion best = HabitRegion_None;
  int32_t best_margin = 0, best_cos = -1000;
  for (int r = HabitRegion_None + 1; r < HabitRegionCount; r++) {
    const HabitRegionProto *p = &d->cfg.regions[r];
    if (!p->enabled) {
      continue;
    }
    const int32_t c = prv_cos_x1000(d, &p->gravity);
    const int32_t margin = c - p->cos_min_x1000;
    if (margin >= 0 && (best == HabitRegion_None || margin > best_margin)) {
      best = (HabitRegion)r;
      best_margin = margin;
      best_cos = c;
    }
  }
  if (cos_out) {
    *cos_out = best_cos;
  }
  return best;
}

static bool prv_gravity_sane(const HabitDetector *d) {
  const int64_t g = prv_norm3(d->grav) / FX;
  return g >= d->cfg.min_gravity_mg && g <= d->cfg.max_gravity_mg;
}

static bool prv_motion_ok(const HabitDetector *d, bool need_fine) {
  if (d->gross / FX > d->cfg.max_gross_mg) {
    return false;
  }
  if (!need_fine) {
    return true;
  }
  if (d->fine / FX >= d->cfg.min_fine_mg) {
    return true;
  }
  // with the gyro, wrist rotation counts as fine motion too (rubbing / picking with little travel)
  return d->have_gyro && d->cfg.min_fine_gyro_dps10 > 0 &&
         d->gyro_fine / FX >= d->cfg.min_fine_gyro_dps10;
}

static uint8_t prv_confidence(const HabitDetector *d, HabitRegion r, int32_t cos_x1000) {
  const int32_t cmin = d->cfg.regions[r].cos_min_x1000;
  int32_t orient = cmin >= 1000 ? 100 : ((cos_x1000 - cmin) * 100) / (1000 - cmin);
  orient = orient < 0 ? 0 : (orient > 100 ? 100 : orient);
  int32_t motion = d->cfg.min_fine_mg ? (d->fine / FX) * 50 / d->cfg.min_fine_mg : 100;
  if (d->have_gyro && d->cfg.min_fine_gyro_dps10) {
    const int32_t gm = (d->gyro_fine / FX) * 50 / d->cfg.min_fine_gyro_dps10;
    motion = gm > motion ? gm : motion;
  }
  motion = motion > 100 ? 100 : motion;
  return (uint8_t)((orient * 60 + motion * 40) / 100);
}

static void prv_fill(const HabitDetector *d, uint32_t t_ms, HabitEpisode *out) {
  if (!out) {
    return;
  }
  out->region = d->region;
  out->start_ms = d->region_since_ms;
  out->duration_ms = t_ms - d->region_since_ms;
  out->confidence = d->confidence_n ? (uint8_t)(d->confidence_sum / d->confidence_n) : 0;
}

void habit_detect_default_config(HabitConfig *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->sample_hz = 25;
  // Left wrist, Pebble axes: +x toward 3 o'clock (the hand), +y toward 12, +z out of the screen.
  // Pebble reports the direction of gravity (flat, screen up: z = -1000), so a raised forearm
  // (hand at the head) reads -x. Until the wearer calibrates each region, one generic "hand at
  // the head" region is used.
  cfg->regions[HabitRegion_Face] = (HabitRegionProto){
      .enabled = true, .gravity = {.x = -1000, .y = 0, .z = 0}, .cos_min_x1000 = 766};  // 40 deg
  cfg->min_dwell_ms = 3000;
  cfg->release_ms = 1500;
  cfg->min_fine_mg = 15;
  cfg->max_gross_mg = 450;
  cfg->min_fine_gyro_dps10 = 150;  // 15 deg/s
  cfg->min_gravity_mg = 700;
  cfg->max_gravity_mg = 1300;
}

void habit_detect_init(HabitDetector *d, const HabitConfig *cfg) {
  memset(d, 0, sizeof(*d));
  d->cfg = *cfg;
  if (d->cfg.sample_hz == 0) {
    d->cfg.sample_hz = 25;
  }
}

HabitEvent habit_detect_feed(HabitDetector *d, uint32_t t_ms, const HabitVec *accel,
                             const HabitVec *gyro, bool vibrating, HabitEpisode *out) {
  const uint16_t hz = d->cfg.sample_hz;
  const int32_t a[3] = {accel->x * FX, accel->y * FX, accel->z * FX};
  if (!d->primed) {
    memcpy(d->grav, a, sizeof(a));
    memcpy(d->fast, a, sizeof(a));
    d->primed = true;
  }
  if (!vibrating) {
    const int32_t ng = prv_div(GRAVITY_TAU_MS, hz), nn = prv_div(NOISE_TAU_MS, hz);
    int32_t dyn = 0;
    for (int i = 0; i < 3; i++) {
      d->grav[i] = prv_lp(d->grav[i], a[i], ng);
      d->fast[i] = prv_lp(d->fast[i], a[i], nn);
      dyn += abs(d->fast[i] - d->grav[i]);
    }
    d->fine = prv_lp(d->fine, dyn, prv_div(FINE_TAU_MS, hz));
    // gross: peak hold with decay, so one big swing counts for about a second
    const int32_t decayed = d->gross - d->gross / prv_div(GROSS_DECAY_TAU_MS, hz);
    d->gross = dyn > decayed ? dyn : decayed;
    if (gyro) {
      const int32_t gmag = (abs(gyro->x) + abs(gyro->y) + abs(gyro->z)) * FX;
      d->gyro_fine = prv_lp(d->gyro_fine, gmag, prv_div(FINE_TAU_MS, hz));
      d->have_gyro = true;
    }
  }

  int32_t cos_x1000 = -1000;
  const HabitRegion r = prv_gravity_sane(d) ? prv_match(d, &cos_x1000) : HabitRegion_None;

  switch (d->state) {
    case HabitState_Idle:
      if (r != HabitRegion_None && prv_motion_ok(d, true)) {
        d->state = HabitState_Candidate;
        d->region = r;
        d->region_since_ms = t_ms;
        d->last_match_ms = t_ms;
        d->confidence_sum = prv_confidence(d, r, cos_x1000);
        d->confidence_n = 1;
      }
      return HabitEvent_None;

    case HabitState_Candidate:
      if (r == d->region && prv_motion_ok(d, true)) {
        d->last_match_ms = t_ms;
        d->confidence_sum += prv_confidence(d, r, cos_x1000);
        d->confidence_n++;
        if (t_ms - d->region_since_ms >= d->cfg.min_dwell_ms) {
          d->state = HabitState_Active;
          d->last_report_ms = t_ms;
          prv_fill(d, t_ms, out);
          return HabitEvent_Start;
        }
      } else if (r != HabitRegion_None && r != d->region && prv_motion_ok(d, true)) {
        d->region = r;  // the hand moved to another region: its dwell starts now
        d->region_since_ms = t_ms;
        d->last_match_ms = t_ms;
        d->confidence_sum = prv_confidence(d, r, cos_x1000);
        d->confidence_n = 1;
      } else if (t_ms - d->last_match_ms > d->cfg.release_ms) {
        d->state = HabitState_Idle;
      }
      return HabitEvent_None;

    case HabitState_Active:
      // once confirmed, pauses in the fine motion do not end it: only leaving the region does
      if (r != HabitRegion_None && prv_motion_ok(d, false)) {
        d->last_match_ms = t_ms;
        if (r == d->region) {
          d->confidence_sum += prv_confidence(d, r, cos_x1000);
          d->confidence_n++;
        }
      } else if (t_ms - d->last_match_ms > d->cfg.release_ms) {
        d->state = HabitState_Idle;
        prv_fill(d, d->last_match_ms, out);
        return HabitEvent_End;
      }
      if (t_ms - d->last_report_ms >= ONGOING_PERIOD_MS) {
        d->last_report_ms = t_ms;
        prv_fill(d, t_ms, out);
        return HabitEvent_Ongoing;
      }
      return HabitEvent_None;
  }
  return HabitEvent_None;
}

void habit_detect_feed_gyro(HabitDetector *d, const HabitVec *gyro, bool vibrating) {
  if (vibrating) {
    return;
  }
  const int32_t gmag = (abs(gyro->x) + abs(gyro->y) + abs(gyro->z)) * FX;
  d->gyro_fine = d->have_gyro ? prv_lp(d->gyro_fine, gmag, prv_div(FINE_TAU_MS, d->cfg.sample_hz))
                              : gmag;
  d->have_gyro = true;
}

void habit_detect_gyro_off(HabitDetector *d) {
  d->have_gyro = false;
  d->gyro_fine = 0;
}

bool habit_detect_wants_gyro(const HabitDetector *d) {
  return d->state != HabitState_Idle || habit_detect_current_region(d) != HabitRegion_None;
}

HabitRegion habit_detect_current_region(const HabitDetector *d) {
  return prv_gravity_sane(d) ? prv_match(d, NULL) : HabitRegion_None;
}

void habit_detect_levels(const HabitDetector *d, HabitVec *gravity, uint16_t *gross_mg,
                         uint16_t *fine_mg, uint16_t *gyro_fine_dps10) {
  if (gravity) {
    gravity->x = (int16_t)(d->grav[0] / FX);
    gravity->y = (int16_t)(d->grav[1] / FX);
    gravity->z = (int16_t)(d->grav[2] / FX);
  }
  if (gross_mg) {
    *gross_mg = (uint16_t)(d->gross / FX);
  }
  if (fine_mg) {
    *fine_mg = (uint16_t)(d->fine / FX);
  }
  if (gyro_fine_dps10) {
    *gyro_fine_dps10 = (uint16_t)(d->gyro_fine / FX);
  }
}

void habit_detect_calibrate_region(HabitDetector *d, HabitRegion region, int16_t cos_min_x1000) {
  if (region <= HabitRegion_None || region >= HabitRegionCount) {
    return;
  }
  HabitRegionProto *p = &d->cfg.regions[region];
  p->enabled = true;
  p->gravity = (HabitVec){(int16_t)(d->grav[0] / FX), (int16_t)(d->grav[1] / FX),
                          (int16_t)(d->grav[2] / FX)};
  p->cos_min_x1000 = cos_min_x1000;
}

const char *habit_region_name(HabitRegion region) {
  switch (region) {
    case HabitRegion_Mouth:
      return "mouth";
    case HabitRegion_Nose:
      return "nose";
    case HabitRegion_Face:
      return "face";
    case HabitRegion_Hair:
      return "hair";
    default:
      return "none";
  }
}
