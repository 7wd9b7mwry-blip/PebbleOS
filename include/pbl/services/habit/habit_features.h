/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! Motion features of a 3 s window at 10 Hz (accelerometer in mG, gyroscope in 0.1 deg/s; the
//! watch averages its 25 Hz samples into 100 ms bins) for the habit classifier. 10 Hz is the rate
//! of the public training data (CMI, Helios IMU): the watch's faster detail would otherwise look
//! unlike anything the model was trained on. Orientation-invariant on purpose: where the hand is comes from the wearer's
//! calibrated regions (habit_detect); these describe how it moves (picking / pulling / scratching
//! versus drinking, glasses, phone, waving...), so a model trained on another wrist device
//! transfers. Integer-only; the training pipeline runs this same code (tools/habit/features_cli.c).

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <pbl/services/habit/habit_detect.h>

#define HABIT_FEAT_HZ 10
#define HABIT_FEAT_WINDOW 30  // 3 s, about one gesture
#define HABIT_FEAT_HOP 10     // a new window every second

typedef enum {
  HabitFeat_HpMagMean = 0,   //!< high-passed acceleration magnitude, mean (mG)
  HabitFeat_HpMagStd,        //!< ... standard deviation
  HabitFeat_HpMagMax,        //!< ... max
  HabitFeat_SlowMotion,      //!< mean change of the smoothed acceleration per sample (0.1 mG)
  HabitFeat_Jerk,            //!< mean |a[i] - a[i-1]| (L1, mG)
  HabitFeat_GyroMagMean,     //!< rotation speed, mean (0.1 dps)
  HabitFeat_GyroMagStd,
  HabitFeat_GyroMagMax,
  HabitFeat_GyroHpMean,      //!< rotation jitter: mean |gyro - smoothed gyro| (L1, 0.1 dps)
  HabitFeat_GravTurn,        //!< forearm turn between the first and last 0.4 s (1000 - cos x1000)
  HabitFeat_GravPath,        //!< summed forearm turn over 0.2 s steps (1000 - cos x1000)
  HabitFeat_HpZcr,           //!< zero crossings of the high-passed acceleration, 3 axes
  HabitFeat_GyroZcr,         //!< zero crossings of the (mean-removed) gyroscope, 3 axes
  HabitFeat_Repetition,      //!< max autocorrelation of |hp| over lags 2-6 (1.7-5 Hz), x1000
  HabitFeat_GyroToAcc,       //!< rotation jitter per unit of acceleration jitter, x1000
  HabitFeat_GravityError,    //!< | mean |a| - 1000 | (mG)
  HabitFeatCount,
} HabitFeature;

//! Compute the features of `n` (= HABIT_FEAT_WINDOW) consecutive samples, oldest first.
//! `gyro` may be NULL (all rotation features are then 0).
void habit_features_compute(const HabitVec *acc, const HabitVec *gyro, size_t n,
                            int32_t out[HabitFeatCount]);

const char *habit_feature_name(HabitFeature f);
