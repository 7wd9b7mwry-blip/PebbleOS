/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! LSM6DSO FIFO word demultiplexing, kept free of device I/O so it can be unit tested.
//!
//! Each FIFO word is a tag byte (TAG_SENSOR in bits 7:3) and six data bytes. With the gyroscope
//! batched as well, accelerometer and gyroscope words interleave.

#pragma once

#include <stddef.h>
#include <stdint.h>

/** @brief FIFO word size in bytes (tag byte plus sample). */
#define LSM6DSO_FIFO_WORD_BYTES 7
/** @brief TAG_SENSOR of a gyroscope sample. */
#define LSM6DSO_FIFO_TAG_GYRO_NC 0x01U
/** @brief TAG_SENSOR of an accelerometer sample. */
#define LSM6DSO_FIFO_TAG_ACCEL_NC 0x02U

/**
 * @brief Split FIFO words in place.
 *
 * Accelerometer words are moved to the front of @p words, keeping the 7-byte word layout and
 * their order; the data bytes of gyroscope words are copied to @p gyro (6 bytes each, at most
 * @p gyro_max). Other words (timestamps, configuration changes) are dropped.
 *
 * @param words FIFO words as read.
 * @param num_words Number of words.
 * @param gyro Output for the gyroscope samples' data bytes.
 * @param gyro_max Capacity of @p gyro, in samples.
 * @param num_gyro Number of gyroscope samples written to @p gyro.
 * @return Number of accelerometer words now at the front of @p words.
 */
size_t lsm6dso_fifo_split(uint8_t *words, size_t num_words, uint8_t *gyro, size_t gyro_max,
                          size_t *num_gyro);
