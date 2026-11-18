/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! Habits: the habit service's settings that need the wearer, on the watch (the console isn't reachable on a
//! production watch).
//!  - Calibrate a region (mouth, nose, face, hair): Select, then hold the hand there; after a 3 s countdown a buzz,
//!    the forearm's direction 1.5 s later is that region's (the first replaces the generic "hand at the head").
//!  - Forget the calibration.
//!  - Grips (holding a controller, holding a phone): Select, hold it the way it is used; after the countdown and a
//!    buzz, 20 s of it; a double buzz when done. Once per position (sitting, reclined, lying); the server keeps them.
//!  - Posture: "upright now" from the back tag's latest reading (else it is learned when walking); forget it.
//!  - Alerts: pause an hour, or on again; "that was a habit" labels the last few seconds (a missed one).
//!  - Settings (persisted by the service): habit buzzes, alerts while asleep, back posture, the classifier, sound
//!    level, the presence beacon; each row shows On / Off and Select flips it.
//! A false alarm is still a button press right after the buzz (habit_service_button_claim).

#include "habits.h"

#include <stdio.h>
#include <string.h>

#include <pbl/i18n/i18n.h>
#include <pbl/services/habit/habit_service.h>
#include <pbl/services/habit/posture.h>
#include <pbl/util/size.h>

#include <applib/app.h>
#include <applib/app_malloc.h>
#include <applib/app_timer.h>
#include <applib/fonts/fonts.h>
#include <applib/ui/app_window_stack.h>
#include <applib/ui/simple_menu_layer.h>
#include <applib/ui/text_layer.h>
#include <applib/ui/vibes.h>
#include <applib/ui/window.h>
#include <resource/resource_ids.auto.h>

#define COUNTDOWN_S 3
#define HOLD_MS 1500
#define SAVED_MS 1500
#define GRIP_SETTLE_MS 1000   // after the buzz, before the grip is taken
#define GRIP_S 20

enum {
  ITEM_MOUTH, ITEM_NOSE, ITEM_FACE, ITEM_HAIR, ITEM_FORGET,
  ITEM_GRIP_PAD, ITEM_GRIP_PHONE, ITEM_GRIP_FORGET,
  ITEM_UPRIGHT, ITEM_POSTURE_FORGET,
  ITEM_PAUSE, ITEM_RESUME, ITEM_HABIT,
  ITEM_VIBRATE, ITEM_ASLEEP, ITEM_POSTURE, ITEM_MODEL, ITEM_SOUND, ITEM_BEACON,
  ITEM_COUNT
};

static const HabitRegion s_region_of[] = {HabitRegion_Mouth, HabitRegion_Nose, HabitRegion_Face, HabitRegion_Hair};
static const char *const s_region_title[] = {"Mouth", "Nose", "Face", "Hair"};
static const char *const s_region_where[] = {"at your mouth", "at your nose", "on your face", "in your hair"};

typedef enum { CalReady, CalCountdown, CalHold, CalGrip, CalSaved } CalStep;

static const char *const s_grip_title[] = {"Controller", "Phone"};
static const char *const s_grip_how[] = {"Hold the controller as you play", "Hold the phone as you use it"};

typedef struct {
  Window window;
  SimpleMenuLayer *menu;
  SimpleMenuItem items[ITEM_COUNT];
  SimpleMenuSection sections[5];
  char sub[ITEM_COUNT][40];
  AppTimer *refresh_timer;
  // the calibration window
  Window cal_window;
  TextLayer *cal_text;
  char cal_buf[96];
  int cal_item;
  CalStep cal_step;
  int countdown;
  AppTimer *timer;
} HabitsData;

static HabitsData *s_data;

// ---- the menu's subtitles from the service's status

static void prv_refresh(HabitsData *d) {
  HabitServiceStatus st;
  habit_service_get_status(&st);
  for (int i = 0; i < 4; i++) {
    const bool cal = st.regions_calibrated & (1u << s_region_of[i]);
    snprintf(d->sub[i], sizeof(d->sub[i]), "%s", cal ? "Calibrated" : "Not yet");
  }
  snprintf(d->sub[ITEM_FORGET], sizeof(d->sub[0]), "%s",
           st.regions_calibrated ? "Back to the default" : "Using the default");
  const char *state = "no tag";
  if (st.posture) {
    switch (st.posture_state) {
      case PostureState_Upright: state = "upright"; break;
      case PostureState_Slouched: state = "slouched"; break;
      case PostureState_Reclined: state = "reclined"; break;
      default: state = st.posture_calibrated ? "waiting" : "not set: walk or set it";
    }
  }
  snprintf(d->sub[ITEM_GRIP_PAD], sizeof(d->sub[0]), "%s", "Hold it 20 s, per position");
  snprintf(d->sub[ITEM_GRIP_PHONE], sizeof(d->sub[0]), "%s", "Hold it 20 s, per position");
  snprintf(d->sub[ITEM_GRIP_FORGET], sizeof(d->sub[0]), "%s", "Start the grips over");
  snprintf(d->sub[ITEM_UPRIGHT], sizeof(d->sub[0]), "Now: %s", state);
  snprintf(d->sub[ITEM_POSTURE_FORGET], sizeof(d->sub[0]), "%s", "Learn it again walking");
  snprintf(d->sub[ITEM_PAUSE], sizeof(d->sub[0]), "%s", "No habit buzzes for 1 h");
  snprintf(d->sub[ITEM_RESUME], sizeof(d->sub[0]), "%s", "Buzzes on again");
  snprintf(d->sub[ITEM_HABIT], sizeof(d->sub[0]), "%s", "Mark the last few seconds");
  const bool on[] = {st.vibrate, st.alerts_while_asleep, st.posture, st.model, st.sound, st.beacon};
  static const char *const what[] = {"Buzz on a habit", "Habit alerts in bed", "Read the back card",
                                     "Classifier (off: motion rule)", "Sound level a minute", "Room presence"};
  for (int i = 0; i < 6; i++) {
    snprintf(d->sub[ITEM_VIBRATE + i], sizeof(d->sub[0]), "%s: %s", on[i] ? "On" : "Off", what[i]);
  }
  if (d->menu) {
    layer_mark_dirty(simple_menu_layer_get_layer(d->menu));
  }
}

static void prv_refresh_later(void *ctx) {
  HabitsData *d = ctx;
  d->refresh_timer = NULL;
  prv_refresh(d);
}

// ---- calibration

static bool prv_is_grip(const HabitsData *d) {
  return d->cal_item == ITEM_GRIP_PAD || d->cal_item == ITEM_GRIP_PHONE;
}

static void prv_cal_show(HabitsData *d) {
  const int i = d->cal_item;
  if (prv_is_grip(d)) {
    const int g = i - ITEM_GRIP_PAD;
    switch (d->cal_step) {
      case CalReady:
        snprintf(d->cal_buf, sizeof(d->cal_buf), "%s\n\n%s, then press Select", s_grip_title[g], s_grip_how[g]);
        break;
      case CalCountdown:
        snprintf(d->cal_buf, sizeof(d->cal_buf), "%s\n\n%d", s_grip_title[g], d->countdown);
        break;
      case CalHold: case CalGrip:
        snprintf(d->cal_buf, sizeof(d->cal_buf), "Keep holding it...\n\n%d s", GRIP_S);
        break;
      case CalSaved:
        snprintf(d->cal_buf, sizeof(d->cal_buf), "%s saved\n\nAgain in another position for more", s_grip_title[g]);
        break;
    }
    text_layer_set_text(d->cal_text, d->cal_buf);
    return;
  }
  switch (d->cal_step) {
    case CalReady:
      snprintf(d->cal_buf, sizeof(d->cal_buf), "%s\n\nPress Select, then hold your hand %s", s_region_title[i],
               s_region_where[i]);
      break;
    case CalCountdown:
      snprintf(d->cal_buf, sizeof(d->cal_buf), "Hand %s\n\n%d", s_region_where[i], d->countdown);
      break;
    case CalHold: case CalGrip:
      snprintf(d->cal_buf, sizeof(d->cal_buf), "Hold still...");
      break;
    case CalSaved:
      snprintf(d->cal_buf, sizeof(d->cal_buf), "%s saved", s_region_title[i]);
      break;
  }
  text_layer_set_text(d->cal_text, d->cal_buf);
}

static void prv_cal_tick(void *ctx) {
  HabitsData *d = ctx;
  d->timer = NULL;
  if (d->cal_step == CalCountdown) {
    if (--d->countdown > 0) {
      d->timer = app_timer_register(1000, prv_cal_tick, d);
    } else {
      vibes_short_pulse();
      d->cal_step = CalHold;
      d->timer = app_timer_register(prv_is_grip(d) ? GRIP_SETTLE_MS : HOLD_MS, prv_cal_tick, d);
    }
  } else if (d->cal_step == CalHold && prv_is_grip(d)) {
    habit_service_grip_capture((uint8_t)(d->cal_item - ITEM_GRIP_PAD + 1), GRIP_S);
    d->cal_step = CalGrip;
    d->timer = app_timer_register(GRIP_S * 1000 + 1500, prv_cal_tick, d);
  } else if (d->cal_step == CalGrip) {
    vibes_double_pulse();
    d->cal_step = CalSaved;
    d->timer = app_timer_register(SAVED_MS * 2, prv_cal_tick, d);
  } else if (d->cal_step == CalHold) {
    habit_service_calibrate(s_region_of[d->cal_item]);
    vibes_double_pulse();
    d->cal_step = CalSaved;
    d->timer = app_timer_register(SAVED_MS, prv_cal_tick, d);
  } else if (d->cal_step == CalSaved) {
    app_window_stack_remove(&d->cal_window, true);
    return;
  }
  prv_cal_show(d);
}

static void prv_cal_select(ClickRecognizerRef r, void *ctx) {
  HabitsData *d = s_data;
  if (d->cal_step != CalReady) {
    return;
  }
  d->cal_step = CalCountdown;
  d->countdown = COUNTDOWN_S;
  prv_cal_show(d);
  d->timer = app_timer_register(1000, prv_cal_tick, d);
}

static void prv_cal_clicks(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_cal_select);
}

static void prv_cal_load(Window *w) {
  HabitsData *d = window_get_user_data(w);
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds_by_value(root);
  d->cal_text = text_layer_create(GRect(8, b.size.h / 4, b.size.w - 16, b.size.h / 2 + 20));
  text_layer_set_font(d->cal_text, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(d->cal_text, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(d->cal_text));
  prv_cal_show(d);
}

static void prv_cal_unload(Window *w) {
  HabitsData *d = window_get_user_data(w);
  if (d->timer) {
    app_timer_cancel(d->timer);
    d->timer = NULL;
  }
  text_layer_destroy(d->cal_text);
  d->cal_text = NULL;
  prv_refresh(d);
}

static void prv_calibrate(HabitsData *d, int item) {
  d->cal_item = item;
  d->cal_step = CalReady;
  window_init(&d->cal_window, WINDOW_NAME("Habits calibrate"));
  window_set_user_data(&d->cal_window, d);
  window_set_click_config_provider(&d->cal_window, prv_cal_clicks);
  window_set_window_handlers(&d->cal_window, &(WindowHandlers){.load = prv_cal_load, .unload = prv_cal_unload});
  app_window_stack_push(&d->cal_window, true);
}

// ---- the menu

static void prv_select(int index, void *ctx) {
  HabitsData *d = ctx;
  switch (index) {
    case ITEM_MOUTH: case ITEM_NOSE: case ITEM_FACE: case ITEM_HAIR:
      prv_calibrate(d, index);
      return;
    case ITEM_FORGET:
      habit_service_reset_calibration();
      break;
    case ITEM_GRIP_PAD: case ITEM_GRIP_PHONE:
      prv_calibrate(d, index);
      return;
    case ITEM_GRIP_FORGET:
      habit_service_grip_forget();
      vibes_double_pulse();
      break;
    case ITEM_UPRIGHT:
      if (habit_service_posture_set_upright()) {
        vibes_double_pulse();
      } else {
        vibes_long_pulse();
        snprintf(d->sub[ITEM_UPRIGHT], sizeof(d->sub[0]), "%s", "No reading from the tag");
        layer_mark_dirty(simple_menu_layer_get_layer(d->menu));
        return;
      }
      break;
    case ITEM_POSTURE_FORGET:
      habit_service_posture_forget();
      break;
    case ITEM_PAUSE:
      habit_service_set_quiet(60);
      break;
    case ITEM_RESUME:
      habit_service_set_quiet(0);
      break;
    case ITEM_HABIT:
      habit_service_label(1);
      break;
    case ITEM_VIBRATE: case ITEM_ASLEEP: case ITEM_POSTURE: case ITEM_MODEL: case ITEM_SOUND: case ITEM_BEACON: {
      HabitServiceStatus st;
      habit_service_get_status(&st);
      switch (index) {
        case ITEM_VIBRATE: habit_service_set_vibrate(!st.vibrate); break;
        case ITEM_ASLEEP: habit_service_set_alerts_while_asleep(!st.alerts_while_asleep); break;
        case ITEM_POSTURE: habit_service_set_posture(!st.posture); break;
        case ITEM_MODEL: habit_service_set_model(!st.model); break;
        case ITEM_SOUND: habit_service_set_sound(!st.sound); break;
        default: habit_service_set_beacon(!st.beacon); break;
      }
      break;
    }
  }
  vibes_short_pulse();
  prv_refresh(d);
  // the service applies a change on its own task: show it again a moment later
  if (!d->refresh_timer) {
    d->refresh_timer = app_timer_register(400, prv_refresh_later, d);
  }
}

static void prv_select_cal(int index, void *ctx);
static void prv_select_grip(int index, void *ctx);
static void prv_select_posture(int index, void *ctx);
static void prv_select_alerts(int index, void *ctx);
static void prv_select_settings(int index, void *ctx);

static void prv_window_load(Window *window) {
  HabitsData *d = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  static const char *const titles[ITEM_COUNT] = {
      "Mouth", "Nose", "Face", "Hair", "Forget calibration",
      "Holding controller", "Holding phone", "Forget grips",
      "Upright now", "Forget upright",
      "Pause alerts", "Alerts on", "That was a habit",
      "Habit buzzes", "While asleep", "Posture", "Classifier", "Sound level", "Beacon",
  };
  for (int i = 0; i < ITEM_COUNT; i++) {
    d->items[i] = (SimpleMenuItem){.title = titles[i], .subtitle = d->sub[i], .callback = i < ITEM_GRIP_PAD ? prv_select_cal : i < ITEM_UPRIGHT ? prv_select_grip : i < ITEM_PAUSE ? prv_select_posture
                                   : i < ITEM_VIBRATE ? prv_select_alerts : prv_select_settings};
  }
  d->sections[0] = (SimpleMenuSection){.title = "Calibrate", .items = &d->items[ITEM_MOUTH], .num_items = 5};
  d->sections[1] = (SimpleMenuSection){.title = "Grips", .items = &d->items[ITEM_GRIP_PAD], .num_items = 3};
  d->sections[2] = (SimpleMenuSection){.title = "Posture", .items = &d->items[ITEM_UPRIGHT], .num_items = 2};
  d->sections[3] = (SimpleMenuSection){.title = "Alerts", .items = &d->items[ITEM_PAUSE], .num_items = 3};
  d->sections[4] = (SimpleMenuSection){.title = "Settings", .items = &d->items[ITEM_VIBRATE], .num_items = 6};
  prv_refresh(d);
  d->menu = simple_menu_layer_create(layer_get_bounds_by_value(root), window, d->sections, 5, d);
  layer_add_child(root, simple_menu_layer_get_layer(d->menu));
}

// SimpleMenuLayer gives the row within its section: back to the item
static void prv_select_cal(int index, void *ctx) { prv_select(ITEM_MOUTH + index, ctx); }
static void prv_select_grip(int index, void *ctx) { prv_select(ITEM_GRIP_PAD + index, ctx); }
static void prv_select_posture(int index, void *ctx) { prv_select(ITEM_UPRIGHT + index, ctx); }
static void prv_select_alerts(int index, void *ctx) { prv_select(ITEM_PAUSE + index, ctx); }
static void prv_select_settings(int index, void *ctx) { prv_select(ITEM_VIBRATE + index, ctx); }

static void prv_window_appear(Window *window) {
  prv_refresh(window_get_user_data(window));
}

static void prv_window_unload(Window *window) {
  HabitsData *d = window_get_user_data(window);
  if (d->refresh_timer) {
    app_timer_cancel(d->refresh_timer);
    d->refresh_timer = NULL;
  }
  simple_menu_layer_destroy(d->menu);
  d->menu = NULL;
}

static void prv_main(void) {
  HabitsData *d = app_zalloc_check(sizeof(HabitsData));
  s_data = d;
  window_init(&d->window, WINDOW_NAME("Habits"));
  window_set_user_data(&d->window, d);
  window_set_window_handlers(&d->window, &(WindowHandlers){
                                             .load = prv_window_load,
                                             .appear = prv_window_appear,
                                             .unload = prv_window_unload,
                                         });
  app_window_stack_push(&d->window, true);
  app_event_loop();
  app_free(d);
  s_data = NULL;
}

const PebbleProcessMd *habits_app_get_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
      .common =
          {
              .main_func = &prv_main,
              // UUID: 2d7a9e41-5c6b-4f08-8e3d-a1b2c4d6e8f0
              .uuid = {0x2d, 0x7a, 0x9e, 0x41, 0x5c, 0x6b, 0x4f, 0x08, 0x8e, 0x3d, 0xa1, 0xb2, 0xc4, 0xd6, 0xe8,
                       0xf0},
          },
      /// The app that calibrates the habit detection and the back posture.
      .name = pbl_i18n_noop("Habits"),
      .icon_resource_id = RESOURCE_ID_HEART_TINY,
  };
  return &s_app_info.common;
}
