/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! Sound level from microphone frames: RMS and peak in dBFS (tenths of a dB), DC removed per
//! frame. Only the level is kept, never audio. Pure and integer-only.

#pragma once

#include <stddef.h>
#include <stdint.h>

//! Level of silence (and the floor), in tenths of a dBFS
#define SOUND_LEVEL_FLOOR_DB10 (-1000)

typedef struct {
  uint64_t sum_sq;
  uint32_t n;
  uint32_t peak;  //!< largest |sample - frame mean|
} SoundLevelAcc;

void sound_level_reset(SoundLevelAcc *acc);

//! Add one frame of 16-bit samples.
void sound_level_add(SoundLevelAcc *acc, const int16_t *samples, size_t count);

//! RMS level so far, tenths of a dBFS (a full-scale sine is -30, i.e. -3.0 dBFS).
int16_t sound_level_rms_db10(const SoundLevelAcc *acc);

//! Peak level so far, tenths of a dBFS.
int16_t sound_level_peak_db10(const SoundLevelAcc *acc);

//! 100 * log10(x) for x >= 1, to about 0.05 (i.e. tenths of a dB of a power ratio).
int32_t sound_level_log10_x100(uint64_t x);
