/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pbl/services/habit/habit_service.h>
#include <pbl/services/vibe_pattern.h>
#include <pbl/shell/shell.h>

static int prv_cmd_status(const struct pbl_shell *sh, size_t argc, char **argv) {
  HabitServiceStatus st;
  habit_service_get_status(&st);
  pbl_shell_print(sh, "running %d, hand at %s, gravity %d %d %d mG, gross %u fine %u mG",
                  st.running, habit_region_name(st.region_now), st.gravity.x, st.gravity.y,
                  st.gravity.z, st.gross_mg, st.fine_mg);
  pbl_shell_print(sh, "episodes %lu, alerts %lu, smart vibes %lu", (unsigned long)st.episodes,
                  (unsigned long)st.alerts, (unsigned long)st.smart_vibes);
  pbl_shell_print(sh,
                  "baselines: rest %u bpm, sleep %u bpm, rest RMSSD %u ms; heart rate every %u s",
                  st.rest_hr, st.sleep_hr, st.rest_rmssd, st.hr_interval_s);
  pbl_shell_print(sh, "vibrate %d, alerts while asleep %d, calibrated 0x%02x", st.vibrate,
                  st.alerts_while_asleep, st.regions_calibrated);
  pbl_shell_print(sh, "beacon %d (airing %d)", st.beacon, st.beacon_airing);
  pbl_shell_print(sh, "model %d (%s): last score %ld, threshold %ld", st.model, st.model_version,
                  (long)st.model_score, (long)st.model_threshold);
  pbl_shell_print(sh,
                  "posture %d%s, tag %02X:%02X:%02X:%02X:%02X:%02X (battery %u%%), state %u, "
                  "calibrated %d, cos %d, walking %d",
                  st.posture, st.posture_pairing ? " (pairing)" : "", st.posture_tag[0],
                  st.posture_tag[1], st.posture_tag[2], st.posture_tag[3], st.posture_tag[4],
                  st.posture_tag[5], st.posture_battery, st.posture_state, st.posture_calibrated,
                  st.posture_cos_x1000, st.walking);
  static const char *const scan_modes[] = {"active", "lost", "parked"};
  pbl_shell_print(sh,
                  "posture readings %lu, alerts %lu, scans ok %lu failed %lu (last rc %ld), "
                  "scanning %s",
                  (unsigned long)st.posture_readings, (unsigned long)st.posture_alerts,
                  (unsigned long)st.posture_scans_ok, (unsigned long)st.posture_scans_failed,
                  (long)st.posture_last_scan_rc,
                  st.posture_scan_mode < 3 ? scan_modes[st.posture_scan_mode] : "?");
  pbl_shell_print(sh, "sound %d: last minute RMS %d.%d dBFS, peak %d.%d dBFS", st.sound,
                  st.sound_rms_db10 / 10, abs(st.sound_rms_db10 % 10), st.sound_peak_db10 / 10,
                  abs(st.sound_peak_db10 % 10));
  pbl_shell_print(sh, "playing %d (%s), motor %ld", st.playing,
                  st.playing ? vibe_program_get(st.program)->name : "-",
                  (long)vibes_get_vibe_strength());
  return 0;
}

static int prv_program_by_name(const char *name) {
  for (int i = 0; i < VibeProgramCount; i++) {
    if (strcmp(vibe_program_get(i)->name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static int prv_cmd_play(const struct pbl_shell *sh, size_t argc, char **argv) {
  const int p = prv_program_by_name(argv[1]);
  if (p < 0) {
    pbl_shell_error(sh, "unknown program '%s' (calm, sleep, focus, energy, habit_tap, "
                        "habit_pulse, habit_buzz)", argv[1]);
    return -EINVAL;
  }
  long seconds = 0;
  if (argc > 2 && (pbl_shell_strtol(argv[2], &seconds) != 0 || seconds < 0 || seconds > 3600)) {
    pbl_shell_error(sh, "invalid seconds '%s'", argv[2]);
    return -EINVAL;
  }
  habit_service_play((VibeProgramId)p, (uint32_t)seconds * 1000);
  return 0;
}

static int prv_cmd_stop(const struct pbl_shell *sh, size_t argc, char **argv) {
  habit_service_stop();
  return 0;
}

static int prv_cmd_cal(const struct pbl_shell *sh, size_t argc, char **argv) {
  for (int r = HabitRegion_None + 1; r < HabitRegionCount; r++) {
    if (strcmp(habit_region_name(r), argv[1]) == 0) {
      habit_service_calibrate((HabitRegion)r);
      return 0;
    }
  }
  pbl_shell_error(sh, "unknown region '%s' (mouth, nose, face, hair)", argv[1]);
  return -EINVAL;
}

static int prv_cmd_label(const struct pbl_shell *sh, size_t argc, char **argv) {
  long label;
  if (pbl_shell_strtol(argv[1], &label) != 0 || label < 1 || label > 2) {
    pbl_shell_error(sh, "label 1 (habit) or 2 (false alarm)");
    return -EINVAL;
  }
  habit_service_label((uint8_t)label);
  return 0;
}

static int prv_on_off(const struct pbl_shell *sh, const char *arg, bool *out) {
  if (strcmp(arg, "on") == 0) {
    *out = true;
  } else if (strcmp(arg, "off") == 0) {
    *out = false;
  } else {
    pbl_shell_error(sh, "expected on or off");
    return -EINVAL;
  }
  return 0;
}

static int prv_cmd_vibrate(const struct pbl_shell *sh, size_t argc, char **argv) {
  bool on;
  const int rc = prv_on_off(sh, argv[1], &on);
  if (rc == 0) {
    habit_service_set_vibrate(on);
  }
  return rc;
}

static int prv_cmd_sleepalerts(const struct pbl_shell *sh, size_t argc, char **argv) {
  bool on;
  const int rc = prv_on_off(sh, argv[1], &on);
  if (rc == 0) {
    habit_service_set_alerts_while_asleep(on);
  }
  return rc;
}

static int prv_cmd_beacon(const struct pbl_shell *sh, size_t argc, char **argv) {
  bool on;
  const int rc = prv_on_off(sh, argv[1], &on);
  if (rc == 0) {
    habit_service_set_beacon(on);
  }
  return rc;
}

static int prv_cmd_sound(const struct pbl_shell *sh, size_t argc, char **argv) {
  bool on;
  const int rc = prv_on_off(sh, argv[1], &on);
  if (rc == 0) {
    habit_service_set_sound(on);
  }
  return rc;
}

static int prv_cmd_model(const struct pbl_shell *sh, size_t argc, char **argv) {
  bool on;
  const int rc = prv_on_off(sh, argv[1], &on);
  if (rc == 0) {
    habit_service_set_model(on);
  }
  return rc;
}

static int prv_parse_mac(const char *str, uint8_t out[6]) {
  unsigned v[6];
  if (sscanf(str, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
    return -EINVAL;
  }
  for (int i = 0; i < 6; i++) {
    if (v[i] > 0xFF) {
      return -EINVAL;
    }
    out[i] = (uint8_t)v[i];
  }
  return 0;
}

static int prv_cmd_posture(const struct pbl_shell *sh, size_t argc, char **argv) {
  const char *op = argv[1];
  if (strcmp(op, "on") == 0 || strcmp(op, "off") == 0) {
    habit_service_set_posture(strcmp(op, "on") == 0);
  } else if (strcmp(op, "pair") == 0) {
    habit_service_posture_pair();
    pbl_shell_print(sh, "hold the watch next to the tag; the strongest one is paired");
  } else if (strcmp(op, "forget") == 0) {
    habit_service_posture_forget();
  } else if (strcmp(op, "tag") == 0 && argc > 2) {
    uint8_t mac[6];
    if (prv_parse_mac(argv[2], mac) != 0) {
      pbl_shell_error(sh, "address as AA:BB:CC:DD:EE:FF");
      return -EINVAL;
    }
    habit_service_posture_set_tag(mac);
  } else {
    pbl_shell_error(sh, "posture on|off|pair|forget|tag AA:BB:CC:DD:EE:FF");
    return -EINVAL;
  }
  return 0;
}

static int prv_cmd_uncal(const struct pbl_shell *sh, size_t argc, char **argv) {
  habit_service_reset_calibration();
  return 0;
}

static const struct pbl_shell_cmd sub_habit[] = {
  PBL_SHELL_CMD(status, nullptr, "Detector, counters and baselines", prv_cmd_status),
  PBL_SHELL_CMD_ARG(play, nullptr, "Play <program> [seconds]", prv_cmd_play, 2, 1),
  PBL_SHELL_CMD(stop, nullptr, "Stop the program", prv_cmd_stop),
  PBL_SHELL_CMD_ARG(cal, nullptr, "Calibrate <region>: the hand is there now", prv_cmd_cal, 2, 0),
  PBL_SHELL_CMD_ARG(label, nullptr, "Label the last episode: 1 habit, 2 false alarm",
                    prv_cmd_label, 2, 0),
  PBL_SHELL_CMD_ARG(vibrate, nullptr, "Alerts on|off (off: detect and log only)",
                    prv_cmd_vibrate, 2, 0),
  PBL_SHELL_CMD_ARG(sleepalerts, nullptr, "Alerts while asleep on|off", prv_cmd_sleepalerts, 2,
                    0),
  PBL_SHELL_CMD(uncal, nullptr, "Forget the calibrated regions", prv_cmd_uncal),
  PBL_SHELL_CMD_ARG(posture, nullptr, "Back posture tag: on|off|pair|forget|tag <address>",
                    prv_cmd_posture, 2, 1),
  PBL_SHELL_CMD_ARG(model, nullptr, "Classifier on|off (off: the motion rule alone)",
                    prv_cmd_model, 2, 0),
  PBL_SHELL_CMD_ARG(sound, nullptr, "Sound level (1 s a minute, level only) on|off",
                    prv_cmd_sound, 2, 0),
  PBL_SHELL_CMD_ARG(beacon, nullptr, "Presence beacon on|off (aired while connected)",
                    prv_cmd_beacon, 2, 0),
  PBL_SHELL_SUBCMD_SET_END,
};

PBL_SHELL_CMD_REGISTER(habit, sub_habit, "Habit detection and vibes", nullptr);
