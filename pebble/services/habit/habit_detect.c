/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/habit/habit_detect.h>
#include <pbl/services/habit/habit_features.h>
#include <pbl/services/habit/habit_model.h>

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

_Static_assert(HabitFeatCount == 16, "habit_detect keeps 16 features");
_Static_assert(HABIT_DETECT_WINDOW == HABIT_FEAT_WINDOW, "window length");

//! Average the samples of each 100 ms into one classifier sample (10 Hz, like the training data)
static void prv_window_push(HabitDetector *d, uint32_t t_ms, const HabitVec *accel) {
  const uint32_t id = t_ms / (1000 / HABIT_FEAT_HZ);
  if (d->bin_n > 0 && id != d->bin_id) {
    const int32_t n = d->bin_n;
    d->win_acc[d->win_head] = (HabitVec){(int16_t)(d->bin_acc[0] / n), (int16_t)(d->bin_acc[1] / n),
                                         (int16_t)(d->bin_acc[2] / n)};
    d->win_gyro[d->win_head] = (HabitVec){(int16_t)(d->bin_gyro[0] / n),
                                          (int16_t)(d->bin_gyro[1] / n),
                                          (int16_t)(d->bin_gyro[2] / n)};
    d->win_head = (uint8_t)((d->win_head + 1) % HABIT_DETECT_WINDOW);
    if (d->win_count < HABIT_DETECT_WINDOW) {
      d->win_count++;
    }
    d->since_score++;
    memset(d->bin_acc, 0, sizeof(d->bin_acc));
    memset(d->bin_gyro, 0, sizeof(d->bin_gyro));
    d->bin_n = 0;
  }
  d->bin_id = id;
  d->bin_acc[0] += accel->x;
  d->bin_acc[1] += accel->y;
  d->bin_acc[2] += accel->z;
  d->bin_gyro[0] += d->last_gyro.x;
  d->bin_gyro[1] += d->last_gyro.y;
  d->bin_gyro[2] += d->last_gyro.z;
  d->bin_n++;
}

//! Score the window every HABIT_FEAT_HOP samples while the hand is near the head
static void prv_window_score(HabitDetector *d) {
  if (d->win_count < HABIT_DETECT_WINDOW || d->since_score < HABIT_FEAT_HOP) {
    return;
  }
  d->since_score = 0;
  HabitVec acc[HABIT_DETECT_WINDOW], gyro[HABIT_DETECT_WINDOW];
  for (int i = 0; i < HABIT_DETECT_WINDOW; i++) {
    const int j = (d->win_head + i) % HABIT_DETECT_WINDOW;
    acc[i] = d->win_acc[j];
    gyro[i] = d->win_gyro[j];
  }
  habit_features_compute(acc, d->have_gyro ? gyro : NULL, HABIT_DETECT_WINDOW, d->features);
  d->model_score = habit_model_score(d->features);
  d->model_ok = d->model_score >= d->cfg.model_threshold;
  d->scored = true;
  d->features_fresh = true;
}

//! Whether the classifier lets an episode start (always, without a trained model)
static bool prv_model_allows(const HabitDetector *d) {
  if (!d->cfg.use_model || !habit_model_trained()) {
    return true;
  }
  return d->scored && d->model_ok;
}

bool habit_detect_take_window(HabitDetector *d, int32_t features[16], int32_t *score) {
  if (!d->features_fresh) {
    return false;
  }
  d->features_fresh = false;
  memcpy(features, d->features, sizeof(d->features));
  if (score) {
    *score = d->model_score;
  }
  return true;
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
  cfg->use_model = true;
  cfg->model_threshold = g_habit_model.threshold;
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
      d->last_gyro = *gyro;
      const int32_t gmag = (abs(gyro->x) + abs(gyro->y) + abs(gyro->z)) * FX;
      d->gyro_fine = prv_lp(d->gyro_fine, gmag, prv_div(FINE_TAU_MS, hz));
      d->have_gyro = true;
    }
  }

  int32_t cos_x1000 = -1000;
  const HabitRegion r = prv_gravity_sane(d) ? prv_match(d, &cos_x1000) : HabitRegion_None;

  if (!vibrating) {
    prv_window_push(d, t_ms, accel);
  }
  if (r != HabitRegion_None || d->state != HabitState_Idle) {
    prv_window_score(d);
  } else {
    d->scored = false;
  }

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
        if (t_ms - d->region_since_ms >= d->cfg.min_dwell_ms && prv_model_allows(d)) {
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
  d->last_gyro = *gyro;
  const int32_t gmag = (abs(gyro->x) + abs(gyro->y) + abs(gyro->z)) * FX;
  d->gyro_fine = d->have_gyro ? prv_lp(d->gyro_fine, gmag, prv_div(FINE_TAU_MS, d->cfg.sample_hz))
                              : gmag;
  d->have_gyro = true;
}

void habit_detect_gyro_off(HabitDetector *d) {
  d->have_gyro = false;
  d->gyro_fine = 0;
  d->last_gyro = (HabitVec){0, 0, 0};
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
