/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/habit/habit_service.h>

#include <string.h>

#include <pbl/cron/cron.h>
#include <pbl/kernel/time.h>
#include <pbl/logging/logging.h>
#include <pbl/services/activity/activity.h>
#include <pbl/services/data_logging/data_logging_service.h>
#include <pbl/services/habit/habit_policy.h>
#include <pbl/services/hrm/hrm_manager.h>
#include <pbl/services/hrm/hrm_manager_private.h>
#include <pbl/services/light.h>
#include <pbl/services/new_timer/new_timer.h>
#include <pbl/services/system_task.h>
#include <pbl/services/time.h>
#include <pbl/services/vibe_pattern.h>
#include <pbl/util/uuid.h>

#include <applib/accel_service_private.h>
#include <kernel/events.h>

PBL_LOG_MODULE_DEFINE(service_habit, CONFIG_SERVICE_HABIT_LOG_LEVEL);

#define SAMPLE_HZ 25
#define BATCH 25               // one accel callback a second
#define QUEUE_AHEAD_MS 2000    // keep this much of a program queued on the motor
#define REFILL_MS 500
#define STEP_MS 50
#define FLOOR_PCT 12           // weakest strength the LRA is felt at (to tune on the watch)
#define HR_FRESH_MS (3 * 60 * 1000)
#define MIN_HR_CONFIDENCE 50
#define MIN_PPI_CONFIDENCE 60

// All state is owned by KernelBG: callbacks from other tasks hop onto it.
static struct {
  bool enabled;
  bool running;
  AccelServiceState *accel;
  HabitDetector det;
  HabitResponse resp;
  SmartVibes smart;
  bool alerts_while_asleep;
  bool asleep;
  // player
  VibePlayer player;
  bool playing;
  VibeProgramId program;
  uint64_t queued_until_ms;
  TimerID refill_timer;
  // per-minute aggregation
  uint32_t motion_sum;
  uint32_t motion_n;
  uint8_t bpm;
  uint64_t bpm_at_ms;
  bool off_wrist;
  uint16_t ppi_prev;
  uint64_t ppi_sq_sum;
  uint16_t ppi_n;
  HRMSessionRef hrm;
  DataLoggingSession *dls;
  // counters
  uint32_t episodes, alerts, smart_vibes;
} s;

// ---- logging ----------------------------------------------------------------------------------

static void prv_log(HabitLogKind kind, uint8_t a, uint16_t b, uint16_t c, uint16_t d, uint32_t e) {
  if (!s.dls) {
    Uuid system_uuid = UUID_SYSTEM;
    s.dls = dls_create(DlsSystemTagHabit, DATA_LOGGING_BYTE_ARRAY, sizeof(HabitLogRecord),
                       true /*buffered*/, true /*resume*/, &system_uuid);
    if (!s.dls) {
      PBL_LOG_WRN("could not create the data logging session");
      return;
    }
  }
  const HabitLogRecord r = {
      .utc = (uint32_t)pbl_realtime(), .kind = kind, .a = a, .b = b, .c = c, .d = d, .e = e};
  dls_log(s.dls, &r, 1);
}

// ---- player -----------------------------------------------------------------------------------

static void prv_refill(void) {
  if (!s.playing) {
    return;
  }
  const uint64_t now = pbl_uptime_ms();
  if (s.queued_until_ms < now) {
    s.queued_until_ms = now;
  }
  while (s.queued_until_ms - now < QUEUE_AHEAD_MS) {
    uint16_t ms;
    uint8_t strength;
    if (!vibe_player_next(&s.player, &ms, &strength)) {
      s.playing = false;  // the program is over (the queued tail finishes by itself)
      new_timer_stop(s.refill_timer);
      return;
    }
    if (!vibe_pattern_stream_step(VibePatternOwner_Habit, ms, strength)) {
      // another vibe (notification, alarm, call) took the motor: give way for good
      PBL_LOG_DBG("program %s preempted", s.player.program->name);
      s.playing = false;
      new_timer_stop(s.refill_timer);
      return;
    }
    s.queued_until_ms += ms;
  }
}

static void prv_refill_kbg(void *unused) {
  prv_refill();
}

static void prv_refill_timer_cb(void *unused) {
  system_task_add_callback(prv_refill_kbg, nullptr);
}

static bool prv_play(VibeProgramId program, uint32_t max_ms) {
  const VibeProgram *p = vibe_program_get(program);
  if (!p) {
    return false;
  }
  // never talk over another vibe; replace our own
  if (vibe_pattern_is_owned_by(VibePatternOwner_Habit)) {
    vibe_pattern_clear_for_owner(VibePatternOwner_Habit);
  }
  const int32_t scale = vibes_get_default_vibe_strength();  // the wearer's intensity setting
  vibe_player_start(&s.player, p, max_ms, (uint8_t)(scale > 100 ? 100 : (scale < 0 ? 0 : scale)),
                    FLOOR_PCT, STEP_MS);
  s.program = program;
  s.playing = true;
  s.queued_until_ms = 0;
  prv_refill();
  if (s.playing) {
    new_timer_start(s.refill_timer, REFILL_MS, prv_refill_timer_cb, nullptr,
                    TIMER_START_FLAG_REPEATING);
  }
  return true;
}

static void prv_stop(void) {
  s.playing = false;
  new_timer_stop(s.refill_timer);
  vibe_pattern_clear_for_owner(VibePatternOwner_Habit);
}

// ---- sensing ----------------------------------------------------------------------------------

static void prv_accel_cb(AccelData *data, uint32_t num_samples) {
  for (uint32_t i = 0; i < num_samples; i++) {
    const HabitVec a = {data[i].x, data[i].y, data[i].z};
    HabitEpisode ep;
    const uint32_t t_ms = (uint32_t)data[i].timestamp;
    const HabitEvent e = habit_detect_feed(&s.det, t_ms, &a, NULL, data[i].did_vibrate, &ep);
    if (e == HabitEvent_None) {
      continue;
    }
    if (e == HabitEvent_Start) {
      s.episodes++;
      prv_log(HabitLogKind_EpisodeStart, ep.region, ep.confidence, 0, 0, ep.duration_ms);
    } else if (e == HabitEvent_End) {
      prv_log(HabitLogKind_EpisodeEnd, ep.region, ep.confidence, 0, 0, ep.duration_ms);
    }
    VibeRequest req;
    if (habit_response_on_event(&s.resp, t_ms, e, &ep, &req) &&
        (!s.asleep || s.alerts_while_asleep)) {
      s.alerts++;
      prv_log(HabitLogKind_Alert, req.program, s.resp.level, 0, 0, 0);
      prv_play(req.program, req.max_ms);
    }
  }
  uint16_t fine;
  habit_detect_levels(&s.det, NULL, NULL, &fine, NULL);
  s.motion_sum += fine;
  s.motion_n++;
}

static void prv_hrm_cb(PebbleHRMEvent *e, void *context) {
  if (e->event_type == HRMEvent_BPM) {
    s.off_wrist = e->bpm.off_wrist;
    if (!e->bpm.off_wrist && e->bpm.confidence >= MIN_HR_CONFIDENCE && e->bpm.bpm >= 30) {
      s.bpm = e->bpm.bpm;
      s.bpm_at_ms = pbl_uptime_ms();
    }
  } else if (e->event_type == HRMEvent_HRV) {
    if (e->hrv.off_wrist || e->hrv.confidence < MIN_PPI_CONFIDENCE || e->hrv.ppi_ms < 300 ||
        e->hrv.ppi_ms > 2000) {
      s.ppi_prev = 0;
      return;
    }
    if (s.ppi_prev) {
      const int32_t d = (int32_t)e->hrv.ppi_ms - s.ppi_prev;
      if (d > -250 && d < 250) {  // drop ectopic / misdetected beats
        s.ppi_sq_sum += (uint64_t)(d * d);
        s.ppi_n++;
      }
    }
    s.ppi_prev = e->hrv.ppi_ms;
  }
}

static uint16_t prv_isqrt(uint64_t v) {
  uint64_t x = v, y = (x + 1) / 2;
  if (v == 0) {
    return 0;
  }
  while (y < x) {
    x = y;
    y = (x + v / x) / 2;
  }
  return (uint16_t)x;
}

static void prv_minute_kbg(void *unused) {
  if (!s.running) {
    return;
  }
  int32_t sleep_state = ActivitySleepStateUnknown;
  activity_get_metric(ActivityMetricSleepState, 1, &sleep_state);
  s.asleep = (sleep_state == ActivitySleepStateRestfulSleep ||
              sleep_state == ActivitySleepStateLightSleep);

  const bool fresh = s.bpm && pbl_uptime_ms() - s.bpm_at_ms < HR_FRESH_MS;
  const uint16_t rmssd = s.ppi_n >= 10 ? prv_isqrt(s.ppi_sq_sum / s.ppi_n) : 0;
  const uint16_t motion = s.motion_n ? (uint16_t)(s.motion_sum / s.motion_n) : 0;
  const uint32_t lux = light_get_ambient_lux();

  const time_t now = pbl_realtime();
  struct pbl_tm lt;
  pbl_localtime_r(&now, &lt);
  const SmartVibeInput in = {
      .t_min = (uint32_t)(now / 60) + (uint32_t)(lt.tm_gmtoff / 60),
      .minute_of_day = (uint16_t)(lt.tm_hour * 60 + lt.tm_min),
      .worn = !s.off_wrist && fresh,
      .asleep = s.asleep,
      .hr_bpm = fresh ? s.bpm : 0,
      .rmssd_ms = rmssd,
      .motion = motion,
  };
  VibeRequest req;
  if (smart_vibes_on_minute(&s.smart, &in, &req) && !s.playing &&
      !vibe_pattern_is_owned_by(VibePatternOwner_Habit)) {
    s.smart_vibes++;
    prv_log(HabitLogKind_SmartVibe, req.program, 0, 0, 0, req.max_ms);
    prv_play(req.program, req.max_ms);
  }
  prv_log(HabitLogKind_Minute, in.hr_bpm, rmssd, motion, (uint16_t)(lux > 0xFFFF ? 0xFFFF : lux),
          (uint32_t)sleep_state);

  s.motion_sum = s.motion_n = 0;
  s.ppi_sq_sum = 0;
  s.ppi_n = 0;
}

static void prv_minute_cb(struct pbl_cron_job *job, void *data) {
  system_task_add_callback(prv_minute_kbg, nullptr);
  pbl_cron_job_schedule(job);
}

static struct pbl_cron_job s_minute_job = {
    .minute = PBL_CRON_MINUTE_ANY,
    .hour = PBL_CRON_HOUR_ANY,
    .mday = PBL_CRON_MDAY_ANY,
    .month = PBL_CRON_MONTH_ANY,
    .cb = prv_minute_cb,
};

static void prv_start_kbg(void *unused) {
  if (s.running || !s.enabled) {
    return;
  }
  s.accel = accel_session_create();
  if (!s.accel) {
    PBL_LOG_ERR("no accel session");
    return;
  }
  accel_session_data_subscribe(s.accel, BATCH, prv_accel_cb);
  accel_session_set_sampling_rate(s.accel, ACCEL_SAMPLING_25HZ);
  // heart rate once a minute, beat intervals for HRV
  s.hrm = hrm_manager_subscribe_with_callback(INSTALL_ID_INVALID, 60, 0 /*expire_s*/,
                                              HRMFeature_BPM | HRMFeature_HRV,
                                              false /*low_latency*/, prv_hrm_cb, nullptr);
  pbl_cron_job_schedule(&s_minute_job);
  s.running = true;
  PBL_LOG_INFO("started");
}

static void prv_stop_kbg(void *unused) {
  if (!s.running) {
    return;
  }
  prv_stop();
  pbl_cron_job_unschedule(&s_minute_job);
  if (s.hrm != HRM_INVALID_SESSION_REF) {
    sys_hrm_manager_unsubscribe(s.hrm);
    s.hrm = HRM_INVALID_SESSION_REF;
  }
  accel_session_data_unsubscribe(s.accel);
  accel_session_delete(s.accel);
  s.accel = nullptr;
  s.running = false;
  PBL_LOG_INFO("stopped");
}

// ---- API --------------------------------------------------------------------------------------

void habit_service_init(void) {
  memset(&s, 0, sizeof(s));
  HabitConfig dcfg;
  habit_detect_default_config(&dcfg);
  dcfg.sample_hz = SAMPLE_HZ;
  habit_detect_init(&s.det, &dcfg);
  HabitResponseConfig rcfg;
  habit_response_default_config(&rcfg);
  habit_response_init(&s.resp, &rcfg);
  SmartVibeConfig scfg;
  smart_vibes_default_config(&scfg);
  smart_vibes_init(&s.smart, &scfg);
  s.refill_timer = new_timer_create();
  s.hrm = HRM_INVALID_SESSION_REF;
}

void habit_service_set_enabled(bool enabled) {
  s.enabled = enabled;
  system_task_add_callback(enabled ? prv_start_kbg : prv_stop_kbg, nullptr);
}

typedef struct {
  VibeProgramId program;
  uint32_t max_ms;
} PlayArgs;

static PlayArgs s_play_args;

static void prv_play_kbg(void *unused) {
  prv_play(s_play_args.program, s_play_args.max_ms);
}

bool habit_service_play(VibeProgramId program, uint32_t max_ms) {
  if (!vibe_program_get(program)) {
    return false;
  }
  s_play_args = (PlayArgs){program, max_ms};
  system_task_add_callback(prv_play_kbg, nullptr);
  return true;
}

static void prv_stop_play_kbg(void *unused) {
  prv_stop();
}

void habit_service_stop(void) {
  system_task_add_callback(prv_stop_play_kbg, nullptr);
}

static void prv_calibrate_kbg(void *region) {
  habit_detect_calibrate_region(&s.det, (HabitRegion)(uintptr_t)region, 940);  // ~20 deg
  PBL_LOG_INFO("calibrated %s", habit_region_name((HabitRegion)(uintptr_t)region));
}

void habit_service_calibrate(HabitRegion region) {
  system_task_add_callback(prv_calibrate_kbg, (void *)(uintptr_t)region);
}

static void prv_label_kbg(void *label) {
  prv_log(HabitLogKind_Label, (uint8_t)(uintptr_t)label, 0, 0, 0, 0);
}

void habit_service_label(uint8_t label) {
  system_task_add_callback(prv_label_kbg, (void *)(uintptr_t)label);
}

void habit_service_get_status(HabitServiceStatus *st) {
  memset(st, 0, sizeof(*st));
  st->running = s.running;
  st->region_now = habit_detect_current_region(&s.det);
  habit_detect_levels(&s.det, &st->gravity, &st->gross_mg, &st->fine_mg, NULL);
  st->episodes = s.episodes;
  st->alerts = s.alerts;
  st->smart_vibes = s.smart_vibes;
  smart_vibes_baselines(&s.smart, &st->rest_hr, &st->sleep_hr, &st->rest_rmssd);
  st->playing = s.playing;
  st->program = s.program;
}
