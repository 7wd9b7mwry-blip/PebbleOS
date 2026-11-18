/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/habit/habit_service.h>

#include <pbl/services/habit/air_log.h>
#include <pbl/services/habit/home_cmd.h>
#include <pbl/services/habit/habit_negatives.h>

#include <mbedtls/cipher.h>
// the key shared with the home device, for this wearer's builds only (written at build time, never committed:
// .gitignore); without it the key is set with "PDB key <hex>"
#if __has_include(<pbl/services/habit/home_key.h>)
#include <pbl/services/habit/home_key.h>
#endif
#include <mbedtls/cmac.h>

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
#include <pbl/kernel/work.h>
#include <pbl/services/time.h>
#include <pbl/services/vibe_pattern.h>
#include <pbl/settings/settings.h>
#include <pbl/util/rand32.h>
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
#define HR_FRESH_SLACK_MS (2 * 60 * 1000)  // a reading stays current this long past its interval
#define MIN_HR_CONFIDENCE 50
#define MIN_PPI_CONFIDENCE 60
#define SETTINGS_FILE "/habit"
#define SETTINGS_MAX_SIZE 2048
#define SETTINGS_VERSION 1
#define BASELINE_SAVE_MIN 60    // learned baselines are saved hourly
#define BEACON_INTERVAL_MS 250   // each page (changed every second) goes out ~4 times: the receivers miss many
#define AIR_LOG_CAP 512          // records kept for the replay (~8 h of minutes with the events)
#define AIR_NEW_SENDS 3
#define VITALS_EVERY 10          // beacon slots: odd = detector page, every VITALS_EVERY-th the vitals page
#define HOLDOUT_PCT_DEFAULT 25   // trial vibes held out (logged, not played): how the effect is measured
#define BEACON_REFRESH_MIN 10   // battery level refresh; also re-arms after a Bluetooth restart
#define SOUND_FRAME_SAMPLES 320   // 20 ms at 16 kHz
#define SOUND_FRAMES 50           // one second a minute
#define SOUND_SKIP_FRAMES 10      // the first 200 ms after the mic starts: its start-up, not the room
#define POSTURE_PERIOD_MS 10000   // the scan timer; bursts every 10 s while the tag is worn ...
#define POSTURE_BURST_MS 1200     // ... long enough for one 1 s advertisement of the tag
#define POSTURE_PAIR_MIN_RSSI -65 // pairing: the tag must be right next to the watch
#define POSTURE_UPRIGHT_MAX_AGE_S 90  // "upright now" needs a tag reading this recent
#define WALKING_STEPS_PER_MIN 60  // a minute with this many steps teaches the upright reference
#define FALSE_ALARM_WINDOW_MS 15000   // a button press this soon after a habit buzz marks it a false alarm
#define HOME_SCAN_PERIOD_MS 60000   // listen for home commands (home_cmd.h) a burst a minute ...
#define HOME_BURST_MS 600           // ... the home device advertises every 20-40 ms
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
  // a wave program (calm, sleep...) interrupted by an alert: it carries on after the alert
  bool resume;
  VibePlayer resume_player;
  VibeProgramId resume_program;
  // per-minute aggregation
  uint32_t motion_sum;
  uint32_t motion_n;
  uint8_t bpm;
  bool bpm_new;            // a reading arrived since the last minute
  uint64_t bpm_at_ms;
  uint16_t hr_interval_s;  // current heart rate interval (adaptive)
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
  uint16_t last_rmssd;   // the last minute's HRV (beacon)
  uint64_t quiet_until_ms;   // habit alerts paused until (in bed, in the bathroom: from Home Assistant)
  uint32_t beacon_slot;  // the beacon rotates log / detector / vitals pages, one a second
  // the wave program playing (calm, sleep...), for its end record
  bool wave_on;
  VibeProgramId wave_program;
  uint64_t wave_started_ms;
  uint8_t holdout_pct;
  // this minute, for the air log's minute record
  int32_t grav_sum[3];
  uint16_t grav_n;
  uint8_t near_s;
  bool sound_new;
  uint8_t sound_skip;  //!< frames dropped since the mic started
  // false alarms (habit_negatives.h): the last alert, which a button press can mark
  HabitNegatives neg;
  uint32_t alert_at_ms;
  uint8_t alert_region;
  int32_t alert_feat[16];
  volatile bool alert_claimable;
  uint32_t false_alarms;
  // home commands (home_cmd.h)
  uint8_t home_key[16];
  bool home_key_set;
  uint32_t home_last_issued;
  uint32_t home_cmds, home_rejected;
  bool burst_active;   // a scan burst in flight (posture or home)
  bool home_burst;     // ... and it is a home command burst
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
  PostureScanner scanner;
  bool walking;
  int32_t last_steps;
  uint8_t tag_battery;
  uint32_t posture_readings, posture_alerts, scans_ok, scans_failed;
  HabitVec posture_last_acc;   // the tag's last reading (for "upright now")
  uint32_t posture_last_s;
  int32_t last_scan_rc;
  uint8_t regions_calibrated;
  uint16_t minutes_since_save;
  struct {
    uint8_t grip;     //!< being captured (0: none)
    uint8_t left_s;   //!< seconds still to take
    uint8_t n;
    int32_t sum[3];
    uint32_t motion_sum;
  } grip;
} s;

// while a program plays: top the motor's queue up every REFILL_MS (on the system work queue, like everything here)
static void prv_refill_work(struct pbl_work *work);
static PBL_DELAYED_WORK_DEFINE(s_refill_work, prv_refill_work);

static AirRecord s_air_recs[AIR_LOG_CAP];
static uint8_t s_air_sends[AIR_LOG_CAP];
static AirLog s_air;

static void prv_air(AirKind kind, uint8_t a, uint8_t b, uint16_t c, uint16_t d, uint32_t e) {
  air_log_event(&s_air, (uint32_t)pbl_realtime(), (uint8_t)kind, a, b, c, d, e);
}

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
static const char KEY_HOLDOUT[] = "holdout";
static const char KEY_HOME_KEY[] = "home_key";
static const char KEY_NEG[] = "neg";

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
#ifdef POSTURE_TAG_DEFAULT
  {  // nothing paired or set: the build's default tag (home_key.h)
    static const uint8_t zero[6] = {0};
    static const uint8_t def[6] = POSTURE_TAG_DEFAULT;
    if (memcmp(s.posture_tag, zero, sizeof(zero)) == 0) {
      memcpy(s.posture_tag, def, sizeof(def));
    }
  }
#endif
  s.home_key_set = prv_settings_get(KEY_HOME_KEY, s.home_key, sizeof(s.home_key));
  if (!prv_settings_get(KEY_NEG, &s.neg, sizeof(s.neg)) || s.neg.version != HABIT_NEG_VERSION) {
    habit_negatives_init(&s.neg);
  }
#ifdef HOME_CMD_KEY
  if (!s.home_key_set) {
    static const uint8_t built_in[16] = HOME_CMD_KEY;
    memcpy(s.home_key, built_in, sizeof(s.home_key));
    s.home_key_set = true;
  }
#endif
  if (prv_settings_get(KEY_HOLDOUT, &v, 1) && v <= 100) {
    s.holdout_pct = v;
  }
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
  s.sound_new = true;
  prv_log(HabitLogKind_Sound, 0, (uint16_t)(s.sound_rms_db10 + 1000),
          (uint16_t)(s.sound_peak_db10 + 1000), 0, s.sound_acc.n);
}

// on KernelBG, like the rest of the service
static void prv_sound_frame(int16_t *samples, size_t count, void *context) {
  if (!s.sound_capturing || s.sound_done_posted) {
    return;
  }
  if (s.sound_skip < SOUND_SKIP_FRAMES) {
    s.sound_skip++;
    return;
  }
  sound_level_add(&s.sound_acc, samples, count);
  if (++s.sound_frames >= SOUND_FRAMES) {
    s.sound_done_posted = true;
    pbl_sys_work_call(prv_sound_done_kbg, nullptr);
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
  s.sound_skip = 0;
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

static bool prv_play(VibeProgramId program, uint32_t max_ms, AirSource source, uint8_t level);

// ---- home commands (home_cmd.h) -----------------------------------------------------------------

static void prv_set_quiet_kbg(void *minutes);
static void prv_play_trial(VibeProgramId program, uint32_t max_ms, uint8_t source, bool trial);
static void prv_stop(void);

static void prv_home_cmd(const HomeCmd *c) {
  if (!s.home_key_set || !home_cmd_is_new(c, s.home_last_issued, (uint32_t)pbl_realtime())) {
    return;
  }
  uint8_t mac[16];
  const mbedtls_cipher_info_t *aes = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
  if (!aes || mbedtls_cipher_cmac(aes, s.home_key, 128, c->signed_bytes, HOME_CMD_SIGNED_LEN, mac) != 0) {
    return;
  }
  uint8_t diff = 0;
  for (int i = 0; i < HOME_CMD_TAG_LEN; i++) {
    diff |= (uint8_t)(mac[i] ^ c->tag[i]);
  }
  if (diff) {
    s.home_rejected++;
    return;
  }
  s.home_last_issued = c->issued;
  s.home_cmds++;
  PBL_LOG_INFO("home command %u (%u, %u min)", c->op, c->program, c->minutes);
  switch (c->op) {
    case HomeCmdOp_Vibe: {
      const VibeProgram *p = vibe_program_get((VibeProgramId)c->program);
      if (p && p->loops) {
        const uint32_t minutes = c->minutes > 120 ? 120 : c->minutes;
        prv_play_trial((VibeProgramId)c->program, minutes * 60 * 1000, AirSource_Command, c->flags & 1);
      }
      break;
    }
    case HomeCmdOp_Stop:
      prv_stop();
      break;
    case HomeCmdOp_Quiet:
      prv_set_quiet_kbg((void *)(uintptr_t)(c->minutes > 720 ? 720 : c->minutes));
      break;
    case HomeCmdOp_Holdout:
      s.holdout_pct = c->minutes > 100 ? 100 : (uint8_t)c->minutes;
      prv_settings_set(KEY_HOLDOUT, &s.holdout_pct, 1);
      break;
  }
}

static void prv_scan_done(void *ctx);
static void prv_scan_report(const uint8_t addr[6], int8_t rssi, const uint8_t *data, uint8_t len, void *ctx);

static void prv_home_work(struct pbl_work *work);
static PBL_DELAYED_WORK_DEFINE(s_home_work, prv_home_work);

static void prv_home_work(struct pbl_work *work) {
  if (!s.running) {
    return;
  }
  if (s.home_key_set && !s.burst_active &&
      pbl_bt_observer_scan(HOME_BURST_MS, prv_scan_report, prv_scan_done, nullptr) == 0) {
    s.burst_active = true;
    s.home_burst = true;
  }
  pbl_delayed_work_schedule_next(&s_home_work, pbl_ms_to_ticks(HOME_SCAN_PERIOD_MS));
}

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
  bool home_found;
  HomeCmd home;   // the newest home command heard
} s_scan;

static bool prv_tag_set(void) {
  static const uint8_t zero[6] = {0};
  return memcmp(s.posture_tag, zero, sizeof(zero)) != 0;
}

static void prv_scan_report(const uint8_t addr[6], int8_t rssi, const uint8_t *data, uint8_t len,
                            void *ctx) {
  HomeCmd hc;
  if (home_cmd_parse(data, len, &hc)) {
    pbl_mutex_lock(&s_scan_mutex, PBL_FOREVER);
    if (!s_scan.home_found || hc.issued > s_scan.home.issued) {
      s_scan.home = hc;
      s_scan.home_found = true;
    }
    pbl_mutex_unlock(&s_scan_mutex);
    return;
  }
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
  const bool home_burst = s.home_burst;
  s.burst_active = false;
  s.home_burst = false;
  if (got.home_found) {
    prv_home_cmd(&got.home);
  }
  if (home_burst) {
    return;   // not a posture burst: no tag reading expected
  }

  if (s.posture_pairing) {
    if (got.best_found) {
      memcpy(s.posture_tag, got.best_addr, 6);
      prv_settings_set(KEY_POSTURE_TAG, s.posture_tag, sizeof(s.posture_tag));
      posture_forget_reference(&s.posture);
      posture_scan_reset(&s.scanner);
      s.posture_pairing = false;
      PBL_LOG_INFO("posture tag paired (rssi %d)", got.best_rssi);
    }
    return;
  }
  const uint32_t now_s = (uint32_t)(pbl_uptime_ms() / 1000);
  if (got.n == 0) {
    posture_scan_result(&s.scanner, now_s, NULL);
    return;
  }
  s.tag_battery = got.battery;
  s.posture_readings++;
  const HabitVec avg = {(int16_t)(got.sum[0] / got.n), (int16_t)(got.sum[1] / got.n),
                        (int16_t)(got.sum[2] / got.n)};
  posture_scan_result(&s.scanner, now_s, &avg);
  s.posture_last_acc = avg;
  s.posture_last_s = now_s;
  if (posture_feed(&s.posture, now_s, &avg, s.walking) && !s.playing &&
      !vibe_pattern_is_owned_by(VibePatternOwner_Habit)) {
    s.posture_alerts++;
    prv_log(HabitLogKind_PostureAlert, 0, 0, (uint16_t)(s.posture.last_cos_x1000 + 1000), 0, 0);
    prv_air(AirKind_PostureAlert, 0, 0, (uint16_t)(s.posture.last_cos_x1000 + 1000), 0, 0);
    prv_play(VibeProgram_Posture, 0, AirSource_Posture, 0);
  }
}

static void prv_scan_done(void *ctx) {
  pbl_sys_work_call(prv_posture_burst_kbg, nullptr);
}

static void prv_posture_tick_kbg(void *unused) {
  if (!s.running || s.asleep) {
    return;
  }
  if (!s.posture_pairing && !(s.posture_on && prv_tag_set())) {
    return;
  }
  // pairing listens on every tick; otherwise the scheduler slows down for an absent / unworn tag
  if (!s.posture_pairing && !posture_scan_due(&s.scanner, (uint32_t)(pbl_uptime_ms() / 1000))) {
    return;
  }
  if (s.burst_active) {
    return;
  }
  const int rc = pbl_bt_observer_scan(POSTURE_BURST_MS, prv_scan_report, prv_scan_done, nullptr);
  s.last_scan_rc = rc;
  if (rc == 0) {
    s.burst_active = true;
    s.home_burst = false;
    s.scans_ok++;
  } else {
    s.scans_failed++;
  }
}

static void prv_posture_work(struct pbl_work *work);
static PBL_DELAYED_WORK_DEFINE(s_posture_work, prv_posture_work);

static void prv_posture_work(struct pbl_work *work) {
  prv_posture_tick_kbg(nullptr);
  if (s.running) {
    pbl_delayed_work_schedule_next(&s_posture_work, pbl_ms_to_ticks(POSTURE_PERIOD_MS));
  }
}

// steps since the last minute (the day's count resets at midnight: 0 then)
static uint8_t prv_steps_minute(void) {
  int32_t steps = 0;
  int32_t delta = 0;
  if (activity_get_metric(ActivityMetricStepCount, 1, &steps)) {
    delta = s.last_steps > 0 && steps >= s.last_steps ? steps - s.last_steps : 0;
    s.walking = delta >= WALKING_STEPS_PER_MIN;
    s.last_steps = steps;
  }
  return (uint8_t)(delta > 255 ? 255 : delta);
}

static void prv_posture_minute(void) {
  PostureMinute m;
  posture_take_minute(&s.posture, &m);
  if (m.readings > 0) {
    prv_air(AirKind_Posture, (uint8_t)(m.readings > 255 ? 255 : m.readings), (uint8_t)(m.slouched > 255 ? 255 : m.slouched),
            (uint16_t)(m.mean_cos_x1000 + 1000), (uint16_t)(m.min_cos_x1000 + 1000),
            (uint32_t)s.tag_battery | ((uint32_t)m.calibrated << 8) | ((uint32_t)s.walking << 9));
  }
  if (m.readings > 0 || (s.posture_on && prv_tag_set())) {
    prv_log(HabitLogKind_Posture, (uint8_t)(m.readings > 255 ? 255 : m.readings), m.slouched,
            (uint16_t)(m.mean_cos_x1000 + 1000), (uint16_t)(m.min_cos_x1000 + 1000),
            (uint32_t)s.tag_battery | ((uint32_t)m.calibrated << 8) | ((uint32_t)s.walking << 9) |
                ((uint32_t)s.scanner.mode << 10));
  }
}

// ---- presence beacon ----------------------------------------------------------------------------

static void prv_beacon_update(void) {
  if (!s.beacon || !s.running) {
    gap_le_advert_set_beacon(NULL, 0);
    return;
  }
  const uint8_t pct = battery_get_charge_state().charge_percent;
  // flags; BTHome v2 service data (UUID 0xFCD2, unencrypted): battery %, then the detector's live state (refreshed every
  // second from the accel callback, so detection can be tuned on the watch from Home Assistant): count 0x09 x3 (state
  // bits, episodes, alerts), count 0x3D x2 (fine motion mg, gyro fine 0.1 deg/s), signed count 0x5A x3 (gravity x y z
  // mg). Objects in ascending id order; 31 bytes, so no name (HA knows it by address).
  // every other second the air log's page (air_log.h: manufacturer data), now and then the vitals page (smart vibes
  // can be checked on the watch): count 0x09 x4 (0x80 | asleep
  // | off wrist << 1, heart rate, resting / sleeping baselines), count 0x3D x3 (the last minute's RMSSD ms, smart vibes
  // played, seconds since the last heart rate reading)
  const uint32_t slot = s.beacon_slot++;
  AirRecord rec;
  if (slot % 2 == 0 && air_log_next(&s_air, &rec)) {
    uint8_t lbuf[sizeof(struct pbl_bt_ad_data) + AIR_LOG_ADV_LEN];
    struct pbl_bt_ad_data *ldata = (struct pbl_bt_ad_data *)lbuf;
    ldata->ad_data_length = AIR_LOG_ADV_LEN;
    ldata->scan_resp_data_length = 0;
    air_log_encode(&s_air, &rec, ldata->data);
    gap_le_advert_set_beacon(ldata, BEACON_INTERVAL_MS);
    s.beacon_minutes = 0;
    return;
  }
  if (slot % (2 * VITALS_EVERY) == 1) {
    uint8_t rest = 0, sleep = 0;
    uint16_t base_rmssd = 0;
    smart_vibes_baselines(&s.smart, &rest, &sleep, &base_rmssd);
    const uint64_t age_ms = s.bpm_at_ms ? pbl_uptime_ms() - s.bpm_at_ms : 0;
    const uint16_t age_s = (uint16_t)(age_ms / 1000 > 0xFFFF ? 0xFFFF : age_ms / 1000);
    const uint16_t sv = (uint16_t)s.smart_vibes;
    const uint8_t vad[] = {
        2, 0x01, 0x06,
        23, 0x16, 0xD2, 0xFC, 0x40, 0x01, pct,
        0x09, (uint8_t)(0x80 | s.asleep | (s.off_wrist << 1)), 0x09, s.bpm, 0x09, rest, 0x09, sleep,
        0x3D, (uint8_t)s.last_rmssd, (uint8_t)(s.last_rmssd >> 8), 0x3D, (uint8_t)sv, (uint8_t)(sv >> 8),
        0x3D, (uint8_t)age_s, (uint8_t)(age_s >> 8),
    };
    uint8_t vbuf[sizeof(struct pbl_bt_ad_data) + sizeof(vad)];
    struct pbl_bt_ad_data *vdata = (struct pbl_bt_ad_data *)vbuf;
    vdata->ad_data_length = sizeof(vad);
    vdata->scan_resp_data_length = 0;
    memcpy(vdata->data, vad, sizeof(vad));
    gap_le_advert_set_beacon(vdata, BEACON_INTERVAL_MS);
    s.beacon_minutes = 0;
    return;
  }
  HabitVec g = {0};
  uint16_t fine = 0, gyro = 0;
  habit_detect_levels(&s.det, &g, NULL, &fine, &gyro);
  const uint8_t bits = (uint8_t)((uint8_t)s.det.state |
                                 ((habit_detect_current_region(&s.det) != HabitRegion_None) << 2) |
                                 (s.det.have_gyro << 3) | (s.det.model_ok << 4) | (s.gyro_on << 5) |
                                 (s.det.scored << 6));
  const uint8_t ad[] = {
      2, 0x01, 0x06,
      27, 0x16, 0xD2, 0xFC, 0x40, 0x01, pct,
      0x09, bits, 0x09, (uint8_t)s.episodes, 0x09, (uint8_t)s.alerts,
      0x3D, (uint8_t)fine, (uint8_t)(fine >> 8), 0x3D, (uint8_t)gyro, (uint8_t)(gyro >> 8),
      0x5A, (uint8_t)g.x, (uint8_t)((uint16_t)g.x >> 8), 0x5A, (uint8_t)g.y, (uint8_t)((uint16_t)g.y >> 8),
      0x5A, (uint8_t)g.z, (uint8_t)((uint16_t)g.z >> 8),
  };
  _Static_assert(sizeof(ad) <= 31, "legacy advertising data is at most 31 bytes");
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

static void prv_wave_end(AirEnd why) {
  if (!s.wave_on) {
    return;
  }
  s.wave_on = false;
  prv_air(AirKind_PlayEnd, (uint8_t)s.wave_program, (uint8_t)why, 0, 0,
          (uint32_t)(pbl_uptime_ms() - s.wave_started_ms));
}

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
      if (s.resume) {   // the alert is over: back to the wave program it interrupted
        s.resume = false;
        s.player = s.resume_player;
        s.program = s.resume_program;
        continue;
      }
      s.playing = false;  // the program is over (the queued tail finishes by itself)
      prv_wave_end(AirEnd_Done);
      pbl_delayed_work_cancel(&s_refill_work);
      return;
    }
    if (!vibe_pattern_stream_step(VibePatternOwner_Habit, ms, strength)) {
      // another vibe (notification, alarm, call) took the motor: give way for good
      PBL_LOG_DBG("program %s preempted", s.player.program->name);
      s.playing = false;
      s.resume = false;
      prv_wave_end(AirEnd_Preempted);
      pbl_delayed_work_cancel(&s_refill_work);
      return;
    }
    s.queued_until_ms += ms;
  }
}

static void prv_refill_work(struct pbl_work *work) {
  prv_refill();
  if (s.playing) {
    pbl_delayed_work_schedule_next(&s_refill_work, pbl_ms_to_ticks(REFILL_MS));
  }
}

static bool prv_play(VibeProgramId program, uint32_t max_ms, AirSource source, uint8_t level) {
  const VibeProgram *p = vibe_program_get(program);
  if (!p) {
    return false;
  }
  prv_air(AirKind_Play, (uint8_t)program, (uint8_t)source, level, 0, max_ms);
  if (p->loops) {
    prv_wave_end(AirEnd_Replaced);
    s.wave_on = true;
    s.wave_program = program;
    s.wave_started_ms = pbl_uptime_ms();
  }
  // an alert or nudge over a wave program: the program resumes after it (a new wave program replaces it)
  if (!p->loops && s.playing && s.player.program && s.player.program->loops) {
    s.resume = true;
    s.resume_player = s.player;
    s.resume_program = s.program;
  } else if (p->loops) {
    s.resume = false;
  }
  // never talk over another vibe; replace our own
  if (vibe_pattern_is_owned_by(VibePatternOwner_Habit)) {
    vibe_pattern_clear_for_owner(VibePatternOwner_Habit);
  }
  // the wearer's intensity setting; the lab plays at the motor's full range (so the variants compare fairly)
  const int32_t scale = program >= VibeProgram_LabFirst ? 100 : vibes_get_default_vibe_strength();
  if (program == VibeProgram_LabHeart) {   // 80 % of my heart rate (60 bpm without one)
    vibe_program_set_heart_bpm((uint8_t)(s.bpm ? s.bpm * 4 / 5 : 60));
  }
  vibe_player_start(&s.player, p, max_ms, (uint8_t)(scale > 100 ? 100 : (scale < 0 ? 0 : scale)),
                    FLOOR_PCT, STEP_MS);
  s.program = program;
  s.playing = true;
  s.queued_until_ms = 0;
  prv_refill();
  if (s.playing) {
    pbl_delayed_work_reschedule(&pbl_sys_workq, &s_refill_work, PBL_MSEC(REFILL_MS));
  }
  return true;
}

static void prv_stop(void) {
  prv_wave_end(AirEnd_Stopped);
  s.playing = false;
  s.resume = false;
  pbl_delayed_work_cancel(&s_refill_work);
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
      prv_air(AirKind_EpisodeStart, ep.region, ep.confidence, 0, 0,
              ep.duration_ms);
    } else if (e == HabitEvent_End) {
      prv_log(HabitLogKind_EpisodeEnd, ep.region, ep.confidence, 0, 0, ep.duration_ms);
      prv_air(AirKind_EpisodeEnd, ep.region, ep.confidence, 0, 0,
              ep.duration_ms);
    }
    VibeRequest req;
    if (habit_response_on_event(&s.resp, t_ms, e, &ep, &req) &&
        (!s.asleep || s.alerts_while_asleep) && pbl_uptime_ms() >= s.quiet_until_ms) {
      s.alerts++;
      prv_log(HabitLogKind_Alert, req.program, s.resp.level, 0, 0, 0);
      prv_play(req.program, req.max_ms, AirSource_Alert, s.resp.level);
      s.alert_at_ms = (uint32_t)pbl_uptime_ms();
      s.alert_region = (uint8_t)ep.region;
      memcpy(s.alert_feat, s.det.features, sizeof(s.alert_feat));
      s.alert_claimable = true;
    }
  }
  uint16_t fine;
  HabitVec grav;
  habit_detect_levels(&s.det, &grav, NULL, &fine, NULL);
  s.motion_sum += fine;
  s.motion_n++;
  s.grav_sum[0] += grav.x;
  s.grav_sum[1] += grav.y;
  s.grav_sum[2] += grav.z;
  s.grav_n++;
  if (s.grip.grip && s.grip.left_s) {   // a grip being captured: about one sample a second
    s.grip.sum[0] += grav.x;
    s.grip.sum[1] += grav.y;
    s.grip.sum[2] += grav.z;
    s.grip.motion_sum += fine;
    s.grip.n++;
    if (--s.grip.left_s == 0 && s.grip.n) {
      const int16_t gx = (int16_t)(s.grip.sum[0] / s.grip.n), gy = (int16_t)(s.grip.sum[1] / s.grip.n),
                    gz = (int16_t)(s.grip.sum[2] / s.grip.n);
      const uint32_t mot = s.grip.motion_sum / s.grip.n;
      prv_air(AirKind_Grip, s.grip.grip, s.grip.n, (uint16_t)gx, (uint16_t)gy,
              (uint32_t)(uint16_t)gz | ((mot > 0xFFFF ? 0xFFFF : mot) << 16));
      PBL_LOG_INFO("grip %u: %d %d %d, motion %lu", s.grip.grip, gx, gy, gz, (unsigned long)mot);
      s.grip.grip = 0;
    }
  }
  if (habit_detect_current_region(&s.det) != HabitRegion_None && s.near_s < 255) {
    s.near_s++;
  }
  prv_log_window();
  prv_gyro_update();
  prv_beacon_update();   // the live detector state (about once a second)
}

static void prv_hrm_cb(PebbleHRMEvent *e, void *context) {
  if (e->event_type == HRMEvent_BPM) {
    s.off_wrist = e->bpm.off_wrist;
    if (!e->bpm.off_wrist && e->bpm.confidence >= MIN_HR_CONFIDENCE && e->bpm.bpm >= 30) {
      s.bpm = e->bpm.bpm;
      s.bpm_new = true;
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

static void prv_hr_interval_update(void) {
  const uint16_t want = smart_vibes_hr_interval_s(&s.smart, s.asleep);
  if (want == s.hr_interval_s || s.hrm == HRM_INVALID_SESSION_REF) {
    return;
  }
  if (sys_hrm_manager_set_update_interval(s.hrm, want, 0 /*expire_s*/)) {
    PBL_LOG_DBG("heart rate every %u s", want);
    s.hr_interval_s = want;
  }
}

static uint8_t prv_db_byte(int16_t db10) {
  const int v = db10 / 10 + 128;
  return (uint8_t)(v < 1 ? 1 : (v > 255 ? 255 : v));
}

static int8_t prv_grav8(int32_t sum, uint16_t n) {
  const int32_t v = n ? sum / n / 8 : 0;
  return (int8_t)(v < -127 ? -127 : (v > 127 ? 127 : v));
}

static void prv_air_minute(time_t now, const SmartVibeInput *in, uint16_t rmssd, uint16_t motion, uint32_t lux,
                           int32_t sleep_state) {
  const AirMinute m = {
      .hr = in->hr_bpm,
      .rmssd = (uint8_t)(rmssd > 255 ? 255 : rmssd),
      .lux = (uint16_t)(lux > 0xFFFF ? 0xFFFF : lux),
      .state = (uint8_t)(((uint32_t)sleep_state & 7) | (s.off_wrist << 3) |
                         ((pbl_uptime_ms() < s.quiet_until_ms) << 4) | (s.playing << 5) | (s.gyro_on << 6)),
      .motion = (uint8_t)(motion > 255 ? 255 : motion),
      .steps = prv_steps_minute(),
      .sound_rms = s.sound_new ? prv_db_byte(s.sound_rms_db10) : 0,
      .sound_peak = s.sound_new ? prv_db_byte(s.sound_peak_db10) : 0,
      .gx = prv_grav8(s.grav_sum[0], s.grav_n),
      .gy = prv_grav8(s.grav_sum[1], s.grav_n),
      .gz = prv_grav8(s.grav_sum[2], s.grav_n),
      .near_s = s.near_s,
  };
  // stamped with the minute it describes (the one just over)
  air_log_push(&s_air, (uint32_t)(now / 60 * 60 - 60), AirKind_Minute, &m, sizeof(m));
  s.sound_new = false;
  memset(s.grav_sum, 0, sizeof(s.grav_sum));
  s.grav_n = 0;
  s.near_s = 0;
}

static void prv_minute_kbg(void *unused) {
  if (!s.running) {
    return;
  }
  int32_t sleep_state = ActivitySleepStateUnknown;
  activity_get_metric(ActivityMetricSleepState, 1, &sleep_state);
  s.asleep = (sleep_state == ActivitySleepStateRestfulSleep ||
              sleep_state == ActivitySleepStateLightSleep);

  // worn: a reading within the current interval (plus slack); judged only on new readings
  const bool fresh = s.bpm && pbl_uptime_ms() - s.bpm_at_ms <
                                  (uint64_t)s.hr_interval_s * 1000 + HR_FRESH_SLACK_MS;
  const uint16_t rmssd = s.ppi_n >= 10 ? prv_isqrt(s.ppi_sq_sum / s.ppi_n) : 0;
  s.last_rmssd = rmssd;
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
      .hr_bpm = fresh && s.bpm_new ? s.bpm : 0,
      .rmssd_ms = rmssd,
      .motion = motion,
  };
  VibeRequest req;
  if (smart_vibes_on_minute(&s.smart, &in, &req) && !s.playing &&
      !vibe_pattern_is_owned_by(VibePatternOwner_Habit)) {
    s.smart_vibes++;
    if (pbl_rand32() % 100 < s.holdout_pct) {
      prv_air(AirKind_Holdout, (uint8_t)req.program, AirSource_Smart, 0, 0, req.max_ms);
    } else {
      prv_log(HabitLogKind_SmartVibe, req.program, 0, 0, 0, req.max_ms);
      prv_play(req.program, req.max_ms, AirSource_Smart, 0);
    }
  }
  prv_log(HabitLogKind_Minute, in.hr_bpm, rmssd, motion, (uint16_t)(lux > 0xFFFF ? 0xFFFF : lux),
          (uint32_t)sleep_state);
  prv_air_minute(now, &in, rmssd, motion, lux, sleep_state);

  s.bpm_new = false;
  prv_hr_interval_update();
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
  pbl_sys_work_call(prv_minute_kbg, nullptr);
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
  // heart rate (and beat intervals for HRV) every minute or every few, see prv_hr_interval_update()
  s.hr_interval_s = smart_vibes_hr_interval_s(&s.smart, s.asleep);
  s.hrm = hrm_manager_subscribe_with_callback(INSTALL_ID_INVALID, s.hr_interval_s, 0 /*expire_s*/,
                                              HRMFeature_BPM | HRMFeature_HRV,
                                              false /*low_latency*/, prv_hrm_cb, nullptr);
  pbl_cron_job_schedule(&s_minute_job);
  s.running = true;
  prv_air(AirKind_Boot, 0, 0, 0, 0, 0);
  prv_beacon_update();
  pbl_delayed_work_reschedule(&pbl_sys_workq, &s_posture_work, PBL_MSEC(POSTURE_PERIOD_MS));
  pbl_delayed_work_reschedule(&pbl_sys_workq, &s_home_work, PBL_MSEC(HOME_SCAN_PERIOD_MS));
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
  pbl_delayed_work_cancel(&s_posture_work);
  pbl_delayed_work_cancel(&s_home_work);
  pbl_bt_observer_stop();
  s.burst_active = false;
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
  air_log_init(&s_air, s_air_recs, s_air_sends, AIR_LOG_CAP, AIR_NEW_SENDS);
  s.holdout_pct = HOLDOUT_PCT_DEFAULT;
  HabitConfig dcfg;
  habit_detect_default_config(&dcfg);
  dcfg.sample_hz = SAMPLE_HZ;
  // Measured on the wearer's watch (left wrist, 2026-11-16), until the regions are calibrated on the watch: biting
  // reads gravity about (-0.65, +0.75, -0.15) and picking with the hand facing forward (-0.62, +0.21, +0.76); the
  // hand resting (0.06, 0.96, -0.26) and holding the phone (-0.9, 0.4, -0.2) / (-0.22, 0.9, -0.37) differ in motion
  // rather than angle from biting: the phone shakes (fine accel 15-120 mg) but barely turns (gyro under 14 deg/s),
  // biting and picking turn the wrist 15-200 deg/s. So: 32 deg cones, and the fine motion must be rotation (30 deg/s).
  // Biting rolls the wrist to bring each nail up (gravity swung from (-0.76, 0.59, -0.25) to (-0.81, 0.04, -0.58) and
  // (-0.83, -0.33, -0.45) within seconds) while the forearm stays raised: the mouth region is the raised forearm (-x)
  // at any roll, 50 deg.
  dcfg.regions[HabitRegion_Mouth] = (HabitRegionProto){
      .enabled = true, .gravity = {.x = -1000, .y = 0, .z = 0}, .cos_min_x1000 = 643};
  dcfg.regions[HabitRegion_Face] = (HabitRegionProto){
      .enabled = true, .gravity = {.x = -616, .y = 212, .z = 759}, .cos_min_x1000 = 848};
  // Moment to moment, biting's rotation overlaps the phone's; averaged over ~4 s biting runs ~35 deg/s (the small rolls
  // between nails), the phone ~4-5 deg/s.
  dcfg.gyro_required = true;
  dcfg.gyro_tau_ms = 4000;
  dcfg.min_fine_gyro_dps10 = 150;
  // The classifier (trained on other people's wrist data) rejected every window on this watch: episodes come from
  // the region and the fine motion until it is retrained on the wearer's labelled windows ("habit model on").
  dcfg.use_model = false;
  habit_detect_init(&s.det, &dcfg);
  s.det.neg = &s.neg;
  HabitResponseConfig rcfg;
  habit_response_default_config(&rcfg);
  habit_response_init(&s.resp, &rcfg);
  SmartVibeConfig scfg;
  smart_vibes_default_config(&scfg);
  smart_vibes_init(&s.smart, &scfg);
  s.hrm = HRM_INVALID_SESSION_REF;
  s.beacon = true;
  s.sound = true;
  s.posture_on = true;
  PostureConfig pcfg;
  posture_default_config(&pcfg);
  posture_init(&s.posture, &pcfg);
  PostureScanConfig sccfg;
  posture_scan_default_config(&sccfg);
  sccfg.period_s = POSTURE_PERIOD_MS / 1000;
  posture_scan_init(&s.scanner, &sccfg);
  s.sound_rms_db10 = s.sound_peak_db10 = SOUND_LEVEL_FLOOR_DB10;
}

static void prv_set_quiet_kbg(void *minutes) {
  const uint32_t m = (uint32_t)(uintptr_t)minutes;
  s.quiet_until_ms = m ? pbl_uptime_ms() + (uint64_t)m * 60 * 1000 : 0;
  prv_air(AirKind_Quiet, 0, 0, (uint16_t)m, 0, 0);
  PBL_LOG_INFO("alerts quiet for %" PRIu32 " min", m);
}

void habit_service_set_quiet(uint32_t minutes) {
  pbl_sys_work_call(prv_set_quiet_kbg, (void *)(uintptr_t)minutes);
}

// the next space-separated word of *p (at most n-1 chars) into out; advances *p
static void prv_word(const char **p, char *out, size_t n) {
  while (**p == ' ') {
    (*p)++;
  }
  size_t i = 0;
  while (**p && **p != ' ') {
    if (i + 1 < n) {
      out[i++] = **p;
    }
    (*p)++;
  }
  out[i] = '\0';
}

static uint32_t prv_number(const char *w, uint32_t fallback) {
  if (!*w) {
    return fallback;
  }
  uint32_t v = 0;
  for (; *w >= '0' && *w <= '9'; w++) {
    v = v * 10 + (uint32_t)(*w - '0');
  }
  return v;
}

void habit_service_command(const char *cmd) {
  char verb[12], what[40], arg[8], flag[8];
  const char *p = cmd;
  prv_word(&p, verb, sizeof(verb));
  prv_word(&p, what, sizeof(what));
  prv_word(&p, arg, sizeof(arg));
  prv_word(&p, flag, sizeof(flag));
  if (strcmp(verb, "vibe") == 0) {
    if (strcmp(what, "stop") == 0) {
      habit_service_stop();
      return;
    }
    for (int i = 0; i < VibeProgramCount; i++) {
      const VibeProgram *prog = vibe_program_get(i);
      if (prog && prog->loops && strcmp(prog->name, what) == 0) {
        const uint32_t minutes = prv_number(arg, 15);
        // "trial": part of the experiment (a share held out, see HOLDOUT_PCT_DEFAULT)
        habit_service_play_from((VibeProgramId)i, (minutes > 120 ? 120 : minutes) * 60 * 1000, AirSource_Command,
                                strcmp(flag, "trial") == 0);
        return;
      }
    }
  } else if (strcmp(verb, "key") == 0) {
    habit_service_set_home_key(what);
    return;
  } else if (strcmp(verb, "holdout") == 0) {
    habit_service_set_holdout((uint8_t)prv_number(what, HOLDOUT_PCT_DEFAULT));
    return;
  } else if (strcmp(verb, "quiet") == 0) {
    if (strcmp(what, "off") == 0) {
      habit_service_set_quiet(0);
    } else if (strcmp(what, "on") == 0) {
      const uint32_t minutes = prv_number(arg, 60);
      habit_service_set_quiet(minutes > 720 ? 720 : minutes);
    }
    return;
  }
  PBL_LOG_WRN("unknown command");
}

void habit_service_set_enabled(bool enabled) {
  s.enabled = enabled;
  pbl_sys_work_call(enabled ? prv_start_kbg : prv_stop_kbg, nullptr);
}

typedef struct {
  VibeProgramId program;
  uint32_t max_ms;
  uint8_t source;
  bool trial;
} PlayArgs;

static PlayArgs s_play_args;

static void prv_play_trial(VibeProgramId program, uint32_t max_ms, uint8_t source, bool trial) {
  if (trial && pbl_rand32() % 100 < s.holdout_pct) {
    prv_air(AirKind_Holdout, (uint8_t)program, source, 0, 0, max_ms);
    return;
  }
  prv_play(program, max_ms, (AirSource)source, 0);
}

static void prv_play_kbg(void *unused) {
  const PlayArgs a = s_play_args;
  prv_play_trial(a.program, a.max_ms, a.source, a.trial);
}

bool habit_service_play_from(VibeProgramId program, uint32_t max_ms, uint8_t source, bool trial) {
  if (!vibe_program_get(program)) {
    return false;
  }
  s_play_args = (PlayArgs){program, max_ms, source, trial};
  pbl_sys_work_call(prv_play_kbg, nullptr);
  return true;
}

bool habit_service_play(VibeProgramId program, uint32_t max_ms) {
  return habit_service_play_from(program, max_ms, AirSource_App, false);
}

static void prv_set_holdout_kbg(void *pct) {
  prv_load_settings();
  const uint8_t v = (uint8_t)(uintptr_t)pct;
  s.holdout_pct = v > 100 ? 100 : v;
  prv_settings_set(KEY_HOLDOUT, &s.holdout_pct, 1);
}

static uint8_t s_key_arg[16];

static void prv_set_home_key_kbg(void *unused) {
  prv_load_settings();
  memcpy(s.home_key, s_key_arg, sizeof(s.home_key));
  s.home_key_set = true;
  s.home_last_issued = 0;
  prv_settings_set(KEY_HOME_KEY, s.home_key, sizeof(s.home_key));
  PBL_LOG_INFO("home command key set");
}

static int prv_hex(char ch) {
  return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

bool habit_service_set_home_key(const char *hex32) {
  if (strlen(hex32) != 32) {
    return false;
  }
  for (int i = 0; i < 16; i++) {
    const int hi = prv_hex(hex32[2 * i]), lo = prv_hex(hex32[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    s_key_arg[i] = (uint8_t)(hi << 4 | lo);
  }
  pbl_sys_work_call(prv_set_home_key_kbg, nullptr);
  return true;
}

void habit_service_set_holdout(uint8_t pct) {
  pbl_sys_work_call(prv_set_holdout_kbg, (void *)(uintptr_t)pct);
}

static void prv_stop_play_kbg(void *unused) {
  prv_stop();
}

void habit_service_stop(void) {
  pbl_sys_work_call(prv_stop_play_kbg, nullptr);
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

static void prv_grip_kbg(void *arg) {
  const uint32_t v = (uint32_t)(uintptr_t)arg;
  memset(&s.grip, 0, sizeof(s.grip));
  if ((v & 0xFF) == 0) {
    prv_air(AirKind_Grip, 0, 0, 0, 0, 0);
    return;
  }
  s.grip.grip = (uint8_t)(v & 0xFF);
  s.grip.left_s = (uint8_t)(v >> 8);
}

void habit_service_grip_capture(uint8_t grip, uint8_t seconds) {
  if (grip == 0 || seconds == 0) {
    return;
  }
  pbl_sys_work_call(prv_grip_kbg, (void *)(uintptr_t)(grip | ((uint32_t)seconds << 8)));
}

void habit_service_grip_forget(void) {
  pbl_sys_work_call(prv_grip_kbg, (void *)(uintptr_t)0);
}

void habit_service_reset_calibration(void) {
  pbl_sys_work_call(prv_reset_calibration_kbg, nullptr);
}

static void prv_set_vibrate_kbg(void *on) {
  prv_load_settings();
  s.resp.cfg.mode = on ? HabitResponseMode_Vibrate : HabitResponseMode_Off;
  const uint8_t v = on ? 1 : 0;
  prv_settings_set(KEY_VIBRATE, &v, 1);
}

void habit_service_set_vibrate(bool vibrate) {
  pbl_sys_work_call(prv_set_vibrate_kbg, (void *)(uintptr_t)vibrate);
}

static void prv_set_sleep_alerts_kbg(void *on) {
  prv_load_settings();
  s.alerts_while_asleep = on != NULL;
  const uint8_t v = on ? 1 : 0;
  prv_settings_set(KEY_SLEEP_ALERTS, &v, 1);
}

void habit_service_set_alerts_while_asleep(bool on) {
  pbl_sys_work_call(prv_set_sleep_alerts_kbg, (void *)(uintptr_t)on);
}

static void prv_set_beacon_kbg(void *on) {
  prv_load_settings();
  s.beacon = on != NULL;
  const uint8_t v = on ? 1 : 0;
  prv_settings_set(KEY_BEACON, &v, 1);
  prv_beacon_update();
}

void habit_service_set_beacon(bool on) {
  pbl_sys_work_call(prv_set_beacon_kbg, (void *)(uintptr_t)on);
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
  pbl_sys_work_call(prv_set_sound_kbg, (void *)(uintptr_t)on);
}

static void prv_set_model_kbg(void *on) {
  prv_load_settings();
  s.det.cfg.use_model = on != NULL;
  const uint8_t v = on ? 1 : 0;
  prv_settings_set(KEY_MODEL, &v, 1);
}

void habit_service_set_model(bool on) {
  pbl_sys_work_call(prv_set_model_kbg, (void *)(uintptr_t)on);
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
  pbl_sys_work_call(prv_set_posture_kbg, (void *)(uintptr_t)on);
}

static void prv_posture_pair_kbg(void *unused) {
  prv_load_settings();
  pbl_mutex_lock(&s_scan_mutex, PBL_FOREVER);
  memset(&s_scan, 0, sizeof(s_scan));
  s.posture_pairing = true;
  pbl_mutex_unlock(&s_scan_mutex);
}

void habit_service_posture_pair(void) {
  pbl_sys_work_call(prv_posture_pair_kbg, nullptr);
}

static uint8_t s_tag_arg[6];

static void prv_posture_set_tag_kbg(void *unused) {
  prv_load_settings();
  for (int i = 0; i < 6; i++) {
    s.posture_tag[i] = s_tag_arg[5 - i];  // stored as on air, least significant byte first
  }
  prv_settings_set(KEY_POSTURE_TAG, s.posture_tag, sizeof(s.posture_tag));
  posture_forget_reference(&s.posture);
  posture_scan_reset(&s.scanner);
  s.posture_pairing = false;
}

void habit_service_posture_set_tag(const uint8_t mac_msb_first[6]) {
  memcpy(s_tag_arg, mac_msb_first, 6);
  pbl_sys_work_call(prv_posture_set_tag_kbg, nullptr);
}

static void prv_posture_forget_kbg(void *unused) {
  posture_forget_reference(&s.posture);
}

static void prv_posture_upright_kbg(void *unused) {
  posture_set_reference(&s.posture, &s.posture_last_acc);
  PBL_LOG_INFO("posture: upright set by hand");
}

bool habit_service_posture_set_upright(void) {
  const uint32_t now_s = (uint32_t)(pbl_uptime_ms() / 1000);
  if (s.posture_last_s == 0 || now_s - s.posture_last_s > POSTURE_UPRIGHT_MAX_AGE_S) {
    return false;   // no recent reading from the tag
  }
  pbl_sys_work_call(prv_posture_upright_kbg, nullptr);
  return true;
}

void habit_service_posture_forget(void) {
  pbl_sys_work_call(prv_posture_forget_kbg, nullptr);
}

void habit_service_calibrate(HabitRegion region) {
  pbl_sys_work_call(prv_calibrate_kbg, (void *)(uintptr_t)region);
}

static void prv_label_kbg(void *label) {
  prv_log(HabitLogKind_Label, (uint8_t)(uintptr_t)label, 0, 0, 0, 0);
  prv_air(AirKind_Label, (uint8_t)(uintptr_t)label, 0, 0, 0, 0);
}

// a button press right after a habit buzz: a false alarm. The buzz stops, the window is remembered (no episode from
// one like it again in that region) and the label goes to the air log (kind label, a 2, b the region)
static void prv_false_alarm_kbg(void *unused) {
  if (s.playing) {
    prv_stop();
  }
  habit_negatives_add(&s.neg, (HabitRegion)s.alert_region, s.alert_feat);
  prv_settings_set(KEY_NEG, &s.neg, sizeof(s.neg));
  s.false_alarms++;
  prv_log(HabitLogKind_Label, 2, s.alert_region, 0, 0, 0);
  prv_air(AirKind_Label, 2, s.alert_region, 0, 0, 0);
  vibe_pattern_stream_step(VibePatternOwner_Habit, 60, 40);   // a faint tick: got it
  PBL_LOG_INFO("false alarm marked (region %u, %u kept)", s.alert_region, s.neg.n);
}

bool habit_service_button_claim(void) {
  if (!s.running || !s.alert_claimable || (uint32_t)pbl_uptime_ms() - s.alert_at_ms > FALSE_ALARM_WINDOW_MS) {
    return false;
  }
  s.alert_claimable = false;
  pbl_sys_work_call(prv_false_alarm_kbg, nullptr);
  return true;
}

static void prv_forget_false_alarms_kbg(void *unused) {
  habit_negatives_init(&s.neg);
  prv_settings_set(KEY_NEG, &s.neg, sizeof(s.neg));
}

void habit_service_forget_false_alarms(void) {
  pbl_sys_work_call(prv_forget_false_alarms_kbg, nullptr);
}

void habit_service_label(uint8_t label) {
  pbl_sys_work_call(prv_label_kbg, (void *)(uintptr_t)label);
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
  st->posture_scan_mode = (uint8_t)s.scanner.mode;
  st->hr_interval_s = s.hr_interval_s;
}
