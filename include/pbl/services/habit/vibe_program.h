/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! Vibration programs as strength envelopes: a list of linear ramps (strength 0-100), rendered into
//! the fixed-strength steps the vibe pattern service plays (the AW86225 changes gain mid-playback,
//! so a ramp of short steps feels like a smooth swell). Pure and OS-independent.
//!
//! Programs: slow "breathing" swells for calming and sleep (in the spirit of Apollo Neuro's vibes),
//! faster ones for focus / energy, and the escalating alerts for a detected habit.

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint16_t ms;
  uint8_t from;  //!< strength 0-100 at the start of the ramp
  uint8_t to;    //!< ... and at its end
} VibeRamp;

typedef enum {
  VibeProgram_Calm = 0,   //!< ~6 breaths a minute: 4 s swell up, 1 s hold, 5 s down
  VibeProgram_Sleep,      //!< very gentle, ~4 a minute: back to sleep / wind down
  VibeProgram_Focus,      //!< steady gentle pulse, 30 a minute
  VibeProgram_Energy,     //!< brisk pulses
  VibeProgram_HabitTap,   //!< habit alert level 1: two light taps
  VibeProgram_HabitPulse, //!< level 2: three firmer taps
  VibeProgram_HabitBuzz,  //!< level 3: two strong buzzes
  VibeProgramCount,
} VibeProgramId;

typedef struct {
  const char *name;
  const VibeRamp *ramps;
  uint8_t num_ramps;
  bool loops;  //!< repeats until the play time runs out (else plays once)
} VibeProgram;

const VibeProgram *vibe_program_get(VibeProgramId id);

typedef struct {
  const VibeProgram *program;
  uint8_t ramp;
  uint16_t pos_ms;          //!< position inside the current ramp
  uint32_t remaining_ms;    //!< play-time budget left
  uint8_t scale_pct;        //!< overall intensity (the wearer's setting)
  uint8_t floor_pct;        //!< weakest strength the motor is felt at (non-zero strengths map above it)
  uint16_t step_ms;         //!< resolution of the ramps
} VibePlayer;

//! Start `program` for at most `max_ms` (looping programs repeat until then; 0 = one pass).
void vibe_player_start(VibePlayer *p, const VibeProgram *program, uint32_t max_ms,
                       uint8_t scale_pct, uint8_t floor_pct, uint16_t step_ms);

//! Next step to enqueue: duration and the motor strength (0 = off). Consecutive steps of equal
//! strength are merged. Returns false when the program is over.
bool vibe_player_next(VibePlayer *p, uint16_t *ms, uint8_t *strength);

//! Total length of one pass of the program.
uint32_t vibe_program_length_ms(const VibeProgram *program);

//! The motor strength for envelope strength `s` (0-100) after intensity scaling and the floor.
uint8_t vibe_strength_map(uint8_t s, uint8_t scale_pct, uint8_t floor_pct);
