/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/habit/habit_service.h>

#include <string.h>

#include <pbl/bluetooth/observer.h>
#include <pbl/cron/cron.h>
#include <pbl/drivers/gyro.h>
#include <pbl/kernel/mutex.h>
#include <pbl/kernel/time.h>
#include <pbl/logging/logging.h>
#include <pbl/services/activity/activity.h>
#include <pbl/services/battery/battery_state.h>
#include <pbl/services/data_logging/data_logging_service.h>
#include <pbl/services/habit/habit_features.h>
#include <pbl/services/habit/habit_model.h>
#include <pbl/services/habit/habit_policy.h>
#include <pbl/services/habit/posture.h>
#include <pbl/services/habit/sound_level.h>
#ifdef CONFIG_SERVICE_MIC_MANAGER
#include <pbl/services/mic_manager.h>
#endif
#include <pbl/services/hrm/hrm_manager.h>
#include <pbl/services/hrm/hrm_manager_private.h>
#include <pbl/services/light.h>
#include <pbl/services/new_timer/new_timer.h>
#include <pbl/services/system_task.h>
#include <pbl/services/time.h>
#include <pbl/services/vibe_pattern.h>
#include <pbl/settings/settings.h>
#include <pbl/util/uuid.h>

#include <applib/accel_service_private.h>
#include <comm/ble/gap_le_advert.h>
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
#define SETTINGS_FILE "/habit"
#define SETTINGS_MAX_SIZE 2048
#define SETTINGS_VERSION 1
#define BASELINE_SAVE_MIN 60    // learned baselines are saved hourly
#define BEACON_INTERVAL_MS 1000
#define BEACON_REFRESH_MIN 10   // battery level refresh; also re-arms after a Bluetooth restart
#define SOUND_FRAME_SAMPLES 320   // 20 ms at 16 kHz
#define SOUND_FRAMES 50           // one second a minute
#define POSTURE_PERIOD_MS 10000   // a scan burst every 10 s ...
#define POSTURE_BURST_MS 1200     // ... long enough for one 1 s advertisement of the tag
#define POSTURE_PAIR_MIN_RSSI -65 // pairing: the tag must be right next to the watch
#define WALKING_STEPS_PER_MIN 60  // a minute with this many steps teaches the upright reference
#define GYRO_BUF 64             // > 2 s at 26 Hz
#define GYRO_OFF_AFTER_MS 10000 // hand away from the head this long: gyro off again

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
  DataLoggingSession *dls_windows;
  // gyro: on only while the hand is at (or heading into) an episode
  bool gyro_on;
  uint64_t gyro_wanted_at_ms;
  // counters
  uint32_t episodes, alerts, smart_vibes;
  bool settings_loaded;
  bool beacon;
  uint8_t beacon_minutes;
  // sound level
  bool sound;
  bool sound_capturing;
  bool sound_done_posted;
  uint8_t sound_frames;
  SoundLevelAcc sound_acc;
  int16_t sound_rms_db10, sound_peak_db10;
  // back posture (Minew E8 tag)
  bool posture_on;
  bool posture_pairing;
  uint8_t posture_tag[6];   // as on air (least significant byte first); all 0: none
  PostureDetector posture;
  TimerID posture_timer;
  bool walking;
  int32_t last_steps;
  uint8_t tag_battery;
  uint32_t posture_readings, posture_alerts, scans_ok, scans_failed;
  int32_t last_scan_rc;
  uint8_t regions_calibrated;
  uint16_t minutes_since_save;
} s;

// ---- settings ---------------------------------------------------------------------------------

typedef struct PBL_PACKED {
  uint8_t version;
  uint8_t calibrated;  // bit per region
  HabitRegionProto regions[HabitRegionCount];
} RegionsRecord;

typedef struct PBL_PACKED {
  uint8_t version;
  int32_t rest_hr, sleep_hr, rest_rmssd;
  uint16_t rest_n, sleep_n, hrv_n;
} BaselinesRecord;

static const char KEY_REGIONS[] = "regions";
static const char KEY_VIBRATE[] = "vibrate";
static const char KEY_SLEEP_ALERTS[] = "sleep_alerts";
static const char KEY_BASELINES[] = "baselines";
static const char KEY_BEACON[] = "beacon";
static const char KEY_SOUND[] = "sound";
static const char KEY_MODEL[] = "model";
static const char KEY_POSTURE[] = "posture";
static const char KEY_POSTURE_TAG[] = "posture_tag";

static bool prv_settings_get(const char *key, void *val, size_t len) {
  struct pbl_settings_file file;
  if (pbl_settings_open(&file, SETTINGS_FILE, SETTINGS_MAX_SIZE) != 0) {
    return false;
  }
  const bool ok = pbl_settings_get_len(&file, key, strlen(key)) == len &&
                  pbl_settings_get(&file, key, strlen(key), val, len) == 0;
  pbl_settings_close(&file);
  return ok;
}

static void prv_settings_set(const char *key, const void *val, size_t len) {
  struct pbl_settings_file file;
  if (pbl_settings_open(&file, SETTINGS_FILE, SETTINGS_MAX_SIZE) != 0) {
    PBL_LOG_WRN("could not open the settings file");
    return;
  }
  if (pbl_settings_set(&file, key, strlen(key), val, len) != 0) {
    PBL_LOG_WRN("could not save %s", key);
  }
  pbl_settings_close(&file);
}

static void prv_save_regions(void) {
  RegionsRecord r = {.version = SETTINGS_VERSION, .calibrated = s.regions_calibrated};
  memcpy(r.regions, s.det.cfg.regions, sizeof(r.regions));
  prv_settings_set(KEY_REGIONS, &r, sizeof(r));
}

static void prv_save_baselines(void) {
  const BaselinesRecord b = {
      .version = SETTINGS_VERSION,
      .rest_hr = s.smart.rest_hr,
      .sleep_hr = s.smart.sleep_hr,
      .rest_rmssd = s.smart.rest_rmssd,
      .rest_n = s.smart.rest_n,
      .sleep_n = s.smart.sleep_n,
      .hrv_n = s.smart.hrv_n,
  };
  prv_settings_set(KEY_BASELINES, &b, sizeof(b));
  s.minutes_since_save = 0;
}

static void prv_load_settings(void) {
  if (s.settings_loaded) {
    return;
  }
  s.settings_loaded = true;
  RegionsRecord r;
  if (prv_settings_get(KEY_REGIONS, &r, sizeof(r)) && r.version == SETTINGS_VERSION) {
    memcpy(s.det.cfg.regions, r.regions, sizeof(r.regions));
    s.regions_calibrated = r.calibrated;
  }
  uint8_t v;
  if (prv_settings_get(KEY_VIBRATE, &v, 1)) {
    s.resp.cfg.mode = v ? HabitResponseMode_Vibrate : HabitResponseMode_Off;
  }
  if (prv_settings_get(KEY_SLEEP_ALERTS, &v, 1)) {
    s.alerts_while_asleep = v != 0;
  }
  if (prv_settings_get(KEY_BEACON, &v, 1)) {
    s.beacon = v != 0;
  }
  if (prv_settings_get(KEY_SOUND, &v, 1)) {
    s.sound = v != 0;
  }
  if (prv_settings_get(KEY_MODEL, &v, 1)) {
    s.det.cfg.use_model = v != 0;
  }
  if (prv_settings_get(KEY_POSTURE, &v, 1)) {
    s.posture_on = v != 0;
  }
  prv_settings_get(KEY_POSTURE_TAG, s.posture_tag, sizeof(s.posture_tag));
  BaselinesRecord b;
  if (prv_settings_get(KEY_BASELINES, &b, sizeof(b)) && b.version == SETTINGS_VERSION) {
    s.smart.rest_hr = b.rest_hr;
    s.smart.sleep_hr = b.sleep_hr;
    s.smart.rest_rmssd = b.rest_rmssd;
    s.smart.rest_n = b.rest_n;
    s.smart.sleep_n = b.sleep_n;
    s.smart.hrv_n = b.hrv_n;
  }
}

// gyro samples from the IMU driver's work context, drained on KernelBG
static PBL_MUTEX_DEFINE(s_gyro_mutex);
static GyroSample s_gyro_buf[GYRO_BUF];
static size_t s_gyro_n;

void pbl_gyro_samples_cb(const GyroSample *samples, size_t num_samples) {
  pbl_mutex_lock(&s_gyro_mutex, PBL_FOREVER);
  for (size_t i = 0; i < num_samples; i++) {
    if (s_gyro_n < GYRO_BUF) {
      s_gyro_buf[s_gyro_n++] = samples[i];
    }
  }
  pbl_mutex_unlock(&s_gyro_mutex);
}

static void prv_gyro_drain(bool vibrating) {
  GyroSample buf[GYRO_BUF];
  pbl_mutex_lock(&s_gyro_mutex, PBL_FOREVER);
  const size_t n = s_gyro_n;
  memcpy(buf, s_gyro_buf, n * sizeof(GyroSample));
  s_gyro_n = 0;
  pbl_mutex_unlock(&s_gyro_mutex);
  for (size_t i = 0; i < n; i++) {
    const HabitVec g = {buf[i].x, buf[i].y, buf[i].z};
    habit_detect_feed_gyro(&s.det, &g, vibrating);
  }
}

static void prv_gyro_update(void) {
  const uint64_t now = pbl_uptime_ms();
  if (habit_detect_wants_gyro(&s.det)) {
    s.gyro_wanted_at_ms = now;
    if (!s.gyro_on && pbl_gyro_set_enabled(true) == 0) {
      s.gyro_on = true;
    }
  } else if (s.gyro_on && now - s.gyro_wanted_at_ms > GYRO_OFF_AFTER_MS) {
    pbl_gyro_set_enabled(false);
    s.gyro_on = false;
    habit_detect_gyro_off(&s.det);
  }
}

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

// ---- sound level ------------------------------------------------------------------------------

#ifdef CONFIG_SERVICE_MIC_MANAGER
static int16_t s_mic_buf[SOUND_FRAME_SAMPLES];

static void prv_sound_done_kbg(void *unused) {
  mic_manager_release(MicClientSoundLevel);
  if (!s.sound_capturing) {
    return;  // preempted: no level this minute
  }
  s.sound_capturing = false;
  s.sound_rms_db10 = sound_level_rms_db10(&s.sound_acc);
  s.sound_peak_db10 = sound_level_peak_db10(&s.sound_acc);
  prv_log(HabitLogKind_Sound, 0, (uint16_t)(s.sound_rms_db10 + 1000),
          (uint16_t)(s.sound_peak_db10 + 1000), 0, s.sound_acc.n);
}

// on KernelBG, like the rest of the service
static void prv_sound_frame(int16_t *samples, size_t count, void *context) {
  if (!s.sound_capturing || s.sound_done_posted) {
    return;
  }
  sound_level_add(&s.sound_acc, samples, count);
  if (++s.sound_frames >= SOUND_FRAMES) {
    s.sound_done_posted = true;
    system_task_add_callback(prv_sound_done_kbg, nullptr);
  }
}

static void prv_sound_preempted(void *context) {
  s.sound_capturing = false;  // an app or dictation took the mic
}

static void prv_sound_start(void) {
  if (s.sound_capturing) {  // last minute's never finished
    mic_manager_release(MicClientSoundLevel);
    s.sound_capturing = false;
  }
  if (!s.sound) {
    return;
  }
  sound_level_reset(&s.sound_acc);
  s.sound_frames = 0;
  s.sound_done_posted = false;
  s.sound_capturing = true;
  if (!mic_manager_acquire(MicClientSoundLevel, prv_sound_frame, nullptr, s_mic_buf,
                           SOUND_FRAME_SAMPLES, prv_sound_preempted, nullptr)) {
    s.sound_capturing = false;  // the mic is in use
  }
}

static void prv_sound_stop(void) {
  mic_manager_release(MicClientSoundLevel);
  s.sound_capturing = false;
}
#else
static void prv_sound_start(void) {}
static void prv_sound_stop(void) {}
#endif

static bool prv_play(VibeProgramId program, uint32_t max_ms);

// ---- back posture --------------------------------------------------------------------------------

// filled on the Bluetooth host task during a burst, taken on KernelBG when it ends
static PBL_MUTEX_DEFINE(s_scan_mutex);
static struct {
  int32_t sum[3];
  uint16_t n;
  uint8_t battery;
  int8_t best_rssi;
  uint8_t best_addr[6];
  bool best_found;
} s_scan;

static bool prv_tag_set(void) {
  static const uint8_t zero[6] = {0};
  return memcmp(s.posture_tag, zero, sizeof(zero)) != 0;
}

static void prv_scan_report(const uint8_t addr[6], int8_t rssi, const uint8_t *data, uint8_t len,
                            void *ctx) {
  MinewAcc m;
  if (!minew_acc_parse(data, len, &m)) {
    return;
  }
  pbl_mutex_lock(&s_scan_mutex, PBL_FOREVER);
  if (s.posture_pairing) {
    if (rssi >= POSTURE_PAIR_MIN_RSSI && (!s_scan.best_found || rssi > s_scan.best_rssi)) {
      s_scan.best_found = true;
      s_scan.best_rssi = rssi;
      memcpy(s_scan.best_addr, addr, 6);
    }
  } else if (memcmp(addr, s.posture_tag, 6) == 0) {
    s_scan.sum[0] += m.acc_mg.x;
    s_scan.sum[1] += m.acc_mg.y;
    s_scan.sum[2] += m.acc_mg.z;
    s_scan.n++;
    s_scan.battery = m.battery_pct;
  }
  pbl_mutex_unlock(&s_scan_mutex);
}

static void prv_posture_burst_kbg(void *unused) {
  pbl_mutex_lock(&s_scan_mutex, PBL_FOREVER);
  const typeof(s_scan) got = s_scan;
  memset(&s_scan, 0, sizeof(s_scan));
  pbl_mutex_unlock(&s_scan_mutex);

  if (s.posture_pairing) {
    if (got.best_found) {
      memcpy(s.posture_tag, got.best_addr, 6);
      prv_settings_set(KEY_POSTURE_TAG, s.posture_tag, sizeof(s.posture_tag));
      posture_forget_reference(&s.posture);
      s.posture_pairing = false;
      PBL_LOG_INFO("posture tag paired (rssi %d)", got.best_rssi);
    }
    return;
  }
  if (got.n == 0) {
    return;
  }
  s.tag_battery = got.battery;
  s.posture_readings++;
  const HabitVec avg = {(int16_t)(got.sum[0] / got.n), (int16_t)(got.sum[1] / got.n),
                        (int16_t)(got.sum[2] / got.n)};
  const uint32_t now_s = (uint32_t)(pbl_uptime_ms() / 1000);
  if (posture_feed(&s.posture, now_s, &avg, s.walking) && !s.playing &&
      !vibe_pattern_is_owned_by(VibePatternOwner_Habit)) {
    s.posture_alerts++;
    prv_log(HabitLogKind_PostureAlert, 0, 0, (uint16_t)(s.posture.last_cos_x1000 + 1000), 0, 0);
    prv_play(VibeProgram_Posture, 0);
  }
}

static void prv_scan_done(void *ctx) {
  system_task_add_callback(prv_posture_burst_kbg, nullptr);
}

static void prv_posture_tick_kbg(void *unused) {
  if (!s.running || s.asleep) {
    return;
  }
  if (!s.posture_pairing && !(s.posture_on && prv_tag_set())) {
    return;
  }
  const int rc = pbl_bt_observer_scan(POSTURE_BURST_MS, prv_scan_report, prv_scan_done, nullptr);
  s.last_scan_rc = rc;
  if (rc == 0) {
    s.scans_ok++;
  } else {
    s.scans_failed++;
  }
}

static void prv_posture_timer_cb(void *unused) {
  system_task_add_callback(prv_posture_tick_kbg, nullptr);
}

static void prv_posture_minute(void) {
  int32_t steps = 0;
  if (activity_get_metric(ActivityMetricStepCount, 1, &steps)) {
    const int32_t delta = steps - s.last_steps;
    s.walking = s.last_steps > 0 && delta >= WALKING_STEPS_PER_MIN;
    s.last_steps = steps;
  }
  PostureMinute m;
  posture_take_minute(&s.posture, &m);
  if (m.readings > 0 || (s.posture_on && prv_tag_set())) {
    prv_log(HabitLogKind_Posture, (uint8_t)(m.readings > 255 ? 255 : m.readings), m.slouched,
            (uint16_t)(m.mean_cos_x1000 + 1000), (uint16_t)(m.min_cos_x1000 + 1000),
            (uint32_t)s.tag_battery | ((uint32_t)m.calibrated << 8) | ((uint32_t)s.walking << 9));
  }
}

// ---- presence beacon ----------------------------------------------------------------------------

static void prv_beacon_update(void) {
  if (!s.beacon || !s.running) {
    gap_le_advert_set_beacon(NULL, 0);
    return;
  }
  const uint8_t pct = battery_get_charge_state().charge_percent;
  // flags; BTHome v2 service data (UUID 0xFCD2, unencrypted, object 0x01 battery %); name
  const uint8_t ad[] = {
      2, 0x01, 0x06,
      6, 0x16, 0xD2, 0xFC, 0x40, 0x01, pct,
      4, 0x09, 'P', 'T', '2',
  };
  uint8_t buf[sizeof(struct pbl_bt_ad_data) + sizeof(ad)];
  struct pbl_bt_ad_data *data = (struct pbl_bt_ad_data *)buf;
  data->ad_data_length = sizeof(ad);
  data->scan_resp_data_length = 0;
  memcpy(data->data, ad, sizeof(ad));
  gap_le_advert_set_beacon(data, BEACON_INTERVAL_MS);
  s.beacon_minutes = 0;
}

static void prv_log_window(void) {
  int32_t features[HabitFeatCount];
  int32_t score;
  if (!habit_detect_take_window(&s.det, features, &score)) {
    return;
  }
  HabitWindowRecord w = {.utc = (uint32_t)pbl_realtime(), .score = score};
  memcpy(w.features, features, sizeof(features));
  if (!s.dls_windows) {
    Uuid system_uuid = UUID_SYSTEM;
    s.dls_windows = dls_create(DlsSystemTagHabitWindow, DATA_LOGGING_BYTE_ARRAY,
                               sizeof(HabitWindowRecord), true /*buffered*/, true /*resume*/,
                               &system_uuid);
    if (!s.dls_windows) {
      return;
    }
  }
  w.region = (uint8_t)habit_detect_current_region(&s.det);
  w.state = (uint8_t)s.det.state;
  w.gyro = s.gyro_on ? 1 : 0;
  dls_log(s.dls_windows, &w, 1);
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
  prv_gyro_drain(num_samples > 0 && data[0].did_vibrate);
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
  prv_log_window();
  prv_gyro_update();
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
  if (++s.minutes_since_save >= BASELINE_SAVE_MIN) {
    prv_save_baselines();
  }
  if (++s.beacon_minutes >= BEACON_REFRESH_MIN) {
    prv_beacon_update();
  }
  prv_sound_start();
  prv_posture_minute();
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
  prv_load_settings();
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
  prv_beacon_update();
  new_timer_start(s.posture_timer, POSTURE_PERIOD_MS, prv_posture_timer_cb, nullptr,
                  TIMER_START_FLAG_REPEATING);
  PBL_LOG_INFO("started");
}

static void prv_stop_kbg(void *unused) {
  if (!s.running) {
    return;
  }
  prv_stop();
  pbl_cron_job_unschedule(&s_minute_job);
  prv_save_baselines();
  prv_sound_stop();
  new_timer_stop(s.posture_timer);
  pbl_bt_observer_stop();
  if (s.hrm != HRM_INVALID_SESSION_REF) {
    sys_hrm_manager_unsubscribe(s.hrm);
    s.hrm = HRM_INVALID_SESSION_REF;
  }
  if (s.gyro_on) {
    pbl_gyro_set_enabled(false);
    s.gyro_on = false;
    habit_detect_gyro_off(&s.det);
  }
  accel_session_data_unsubscribe(s.accel);
  accel_session_delete(s.accel);
  s.accel = nullptr;
  s.running = false;
  gap_le_advert_set_beacon(NULL, 0);
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
  s.beacon = true;
  s.sound = true;
  s.posture_on = true;
  s.posture_timer = new_timer_create();
  PostureConfig pcfg;
  posture_default_config(&pcfg);
  posture_init(&s.posture, &pcfg);
  s.sound_rms_db10 = s.sound_peak_db10 = SOUND_LEVEL_FLOOR_DB10;
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
  const HabitRegion r = (HabitRegion)(uintptr_t)region;
  prv_load_settings();
  if (s.regions_calibrated == 0) {
    // the first calibrated region replaces the generic default
    for (int i = 0; i < HabitRegionCount; i++) {
      s.det.cfg.regions[i].enabled = false;
    }
  }
  habit_detect_calibrate_region(&s.det, r, 940);  // ~20 deg
  s.regions_calibrated |= (uint8_t)(1u << r);
  prv_save_regions();
  PBL_LOG_INFO("calibrated %s", habit_region_name(r));
}

static void prv_reset_calibration_kbg(void *unused) {
  HabitConfig dcfg;
  habit_detect_default_config(&dcfg);
  memcpy(s.det.cfg.regions, dcfg.regions, sizeof(dcfg.regions));
  s.regions_calibrated = 0;
  prv_save_regions();
}

void habit_service_reset_calibration(void) {
  system_task_add_callback(prv_reset_calibration_kbg, nullptr);
}

static void prv_set_vibrate_kbg(void *on) {
  prv_load_settings();
  s.resp.cfg.mode = on ? HabitResponseMode_Vibrate : HabitResponseMode_Off;
  const uint8_t v = on ? 1 : 0;
  prv_settings_set(KEY_VIBRATE, &v, 1);
}

void habit_service_set_vibrate(bool vibrate) {
  system_task_add_callback(prv_set_vibrate_kbg, (void *)(uintptr_t)vibrate);
}

static void prv_set_sleep_alerts_kbg(void *on) {
  prv_load_settings();
  s.alerts_while_asleep = on != NULL;
  const uint8_t v = on ? 1 : 0;
  prv_settings_set(KEY_SLEEP_ALERTS, &v, 1);
}

void habit_service_set_alerts_while_asleep(bool on) {
  system_task_add_callback(prv_set_sleep_alerts_kbg, (void *)(uintptr_t)on);
}

static void prv_set_beacon_kbg(void *on) {
  prv_load_settings();
  s.beacon = on != NULL;
  const uint8_t v = on ? 1 : 0;
  prv_settings_set(KEY_BEACON, &v, 1);
  prv_beacon_update();
}

void habit_service_set_beacon(bool on) {
  system_task_add_callback(prv_set_beacon_kbg, (void *)(uintptr_t)on);
}

static void prv_set_sound_kbg(void *on) {
  prv_load_settings();
  s.sound = on != NULL;
  const uint8_t v = on ? 1 : 0;
  prv_settings_set(KEY_SOUND, &v, 1);
  if (!s.sound) {
    prv_sound_stop();
  }
}

void habit_service_set_sound(bool on) {
  system_task_add_callback(prv_set_sound_kbg, (void *)(uintptr_t)on);
}

static void prv_set_model_kbg(void *on) {
  prv_load_settings();
  s.det.cfg.use_model = on != NULL;
  const uint8_t v = on ? 1 : 0;
  prv_settings_set(KEY_MODEL, &v, 1);
}

void habit_service_set_model(bool on) {
  system_task_add_callback(prv_set_model_kbg, (void *)(uintptr_t)on);
}

static void prv_set_posture_kbg(void *on) {
  prv_load_settings();
  s.posture_on = on != NULL;
  const uint8_t v = on ? 1 : 0;
  prv_settings_set(KEY_POSTURE, &v, 1);
  if (!s.posture_on) {
    pbl_bt_observer_stop();
  }
}

void habit_service_set_posture(bool on) {
  system_task_add_callback(prv_set_posture_kbg, (void *)(uintptr_t)on);
}

static void prv_posture_pair_kbg(void *unused) {
  prv_load_settings();
  pbl_mutex_lock(&s_scan_mutex, PBL_FOREVER);
  memset(&s_scan, 0, sizeof(s_scan));
  s.posture_pairing = true;
  pbl_mutex_unlock(&s_scan_mutex);
}

void habit_service_posture_pair(void) {
  system_task_add_callback(prv_posture_pair_kbg, nullptr);
}

static uint8_t s_tag_arg[6];

static void prv_posture_set_tag_kbg(void *unused) {
  prv_load_settings();
  for (int i = 0; i < 6; i++) {
    s.posture_tag[i] = s_tag_arg[5 - i];  // stored as on air, least significant byte first
  }
  prv_settings_set(KEY_POSTURE_TAG, s.posture_tag, sizeof(s.posture_tag));
  posture_forget_reference(&s.posture);
  s.posture_pairing = false;
}

void habit_service_posture_set_tag(const uint8_t mac_msb_first[6]) {
  memcpy(s_tag_arg, mac_msb_first, 6);
  system_task_add_callback(prv_posture_set_tag_kbg, nullptr);
}

static void prv_posture_forget_kbg(void *unused) {
  posture_forget_reference(&s.posture);
}

void habit_service_posture_forget(void) {
  system_task_add_callback(prv_posture_forget_kbg, nullptr);
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
  st->vibrate = s.resp.cfg.mode == HabitResponseMode_Vibrate;
  st->alerts_while_asleep = s.alerts_while_asleep;
  st->regions_calibrated = s.regions_calibrated;
  st->beacon = s.beacon;
  st->beacon_airing = gap_le_advert_is_beacon_airing();
  st->sound = s.sound;
  st->sound_rms_db10 = s.sound_rms_db10;
  st->sound_peak_db10 = s.sound_peak_db10;
  st->model = s.det.cfg.use_model;
  st->model_version = g_habit_model.version;
  st->model_score = s.det.model_score;
  st->model_threshold = s.det.cfg.model_threshold;
  st->posture = s.posture_on;
  st->posture_pairing = s.posture_pairing;
  for (int i = 0; i < 6; i++) {
    st->posture_tag[i] = s.posture_tag[5 - i];
  }
  st->posture_state = s.posture.state;
  st->posture_calibrated = s.posture.calibrated;
  st->posture_cos_x1000 = s.posture.last_cos_x1000;
  st->posture_battery = s.tag_battery;
  st->posture_readings = s.posture_readings;
  st->posture_alerts = s.posture_alerts;
  st->posture_scans_ok = s.scans_ok;
  st->posture_scans_failed = s.scans_failed;
  st->posture_last_scan_rc = s.last_scan_rc;
  st->walking = s.walking;
}
