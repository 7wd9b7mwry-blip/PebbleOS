/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
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
  pbl_shell_print(sh, "baselines: rest %u bpm, sleep %u bpm, rest RMSSD %u ms", st.rest_hr,
                  st.sleep_hr, st.rest_rmssd);
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

static const struct pbl_shell_cmd sub_habit[] = {
  PBL_SHELL_CMD(status, nullptr, "Detector, counters and baselines", prv_cmd_status),
  PBL_SHELL_CMD_ARG(play, nullptr, "Play <program> [seconds]", prv_cmd_play, 2, 1),
  PBL_SHELL_CMD(stop, nullptr, "Stop the program", prv_cmd_stop),
  PBL_SHELL_CMD_ARG(cal, nullptr, "Calibrate <region>: the hand is there now", prv_cmd_cal, 2, 0),
  PBL_SHELL_CMD_ARG(label, nullptr, "Label the last episode: 1 habit, 2 false alarm",
                    prv_cmd_label, 2, 0),
  PBL_SHELL_SUBCMD_SET_END,
};

PBL_SHELL_CMD_REGISTER(habit, sub_habit, "Habit detection and vibes", nullptr);
