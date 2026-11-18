/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! Vibes: start one of the habit service's vibration programs by hand (the Apollo-style calm / sleep swells, focus,
//! energy) for a set time, or stop the one playing. The same programs also start on their own from heart rate
//! (smart vibes); this is for trying them and for using them on purpose.

#include "vibes.h"

#include <pbl/i18n/i18n.h>
#include <pbl/services/habit/habit_service.h>
#include <pbl/services/habit/vibe_program.h>
#include <pbl/util/size.h>

#include <applib/app.h>
#include <applib/ui/app_window_stack.h>
#include <applib/ui/simple_menu_layer.h>
#include <applib/ui/window.h>
#include <applib/app_malloc.h>
#include <resource/resource_ids.auto.h>

typedef struct {
  VibeProgramId program;
  uint32_t minutes;  //!< 0: stop
} VibesChoice;

static const VibesChoice s_choices[] = {
    {VibeProgram_Calm, 5},     {VibeProgram_Calm, 15},  {VibeProgram_Relax, 10}, {VibeProgram_Meditate, 10},
    {VibeProgram_Sleep, 15},   {VibeProgram_Sleep, 30}, {VibeProgram_Focus, 10}, {VibeProgram_Energy, 15},
    {VibeProgram_Calm, 0},
};

static const SimpleMenuItem s_items[] = {
    {.title = "Calm", .subtitle = "5 min, a wave a breath"},
    {.title = "Calm", .subtitle = "15 min"},
    {.title = "Relax", .subtitle = "10 min, slower, softer"},
    {.title = "Meditate", .subtitle = "10 min, 5.5 breaths/min"},
    {.title = "Sleep", .subtitle = "15 min, very gentle"},
    {.title = "Sleep", .subtitle = "30 min"},
    {.title = "Focus", .subtitle = "10 min, light ripples"},
    {.title = "Energy", .subtitle = "15 min, brisk swells"},
    {.title = "Stop", .subtitle = "End the one playing"},
};

typedef struct {
  Window window;
  SimpleMenuLayer *menu;
  SimpleMenuItem items[ARRAY_LENGTH(s_items)];
  SimpleMenuSection section;
} VibesData;

static void prv_select(int index, void *context) {
  const VibesChoice *c = &s_choices[index];
  if (c->minutes == 0) {
    habit_service_stop();
  } else {
    habit_service_play(c->program, c->minutes * 60 * 1000);
  }
}

static void prv_window_load(Window *window) {
  VibesData *data = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  for (unsigned i = 0; i < ARRAY_LENGTH(s_items); i++) {
    data->items[i] = s_items[i];
    data->items[i].callback = prv_select;
  }
  data->section = (SimpleMenuSection){.items = data->items, .num_items = ARRAY_LENGTH(s_items)};
  data->menu = simple_menu_layer_create(layer_get_bounds_by_value(root), window, &data->section, 1, data);
  layer_add_child(root, simple_menu_layer_get_layer(data->menu));
}

static void prv_window_unload(Window *window) {
  VibesData *data = window_get_user_data(window);
  simple_menu_layer_destroy(data->menu);
}

static void prv_main(void) {
  VibesData *data = app_zalloc_check(sizeof(VibesData));
  window_init(&data->window, WINDOW_NAME("Vibes"));
  window_set_user_data(&data->window, data);
  window_set_window_handlers(&data->window, &(WindowHandlers){
                                                .load = prv_window_load,
                                                .unload = prv_window_unload,
                                            });
  app_window_stack_push(&data->window, true);
  app_event_loop();
}

const PebbleProcessMd *vibes_app_get_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
      .common =
          {
              .main_func = &prv_main,
              // UUID: 6b1f2c4e-8a3d-4f7b-9c21-5e0a7d93b4c8
              .uuid = {0x6b, 0x1f, 0x2c, 0x4e, 0x8a, 0x3d, 0x4f, 0x7b, 0x9c, 0x21, 0x5e, 0x0a, 0x7d, 0x93, 0xb4,
                       0xc8},
          },
      /// The app that plays the calming / focus vibration programs.
      .name = pbl_i18n_noop("Vibes"),
      .icon_resource_id = RESOURCE_ID_HEART_TINY,
  };
  return &s_app_info.common;
}
