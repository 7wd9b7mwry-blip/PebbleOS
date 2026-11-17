/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

/**
 * @defgroup bluetooth_observer Observer
 * @ingroup bluetooth
 * @brief Passive LE scanning in short bursts, for reading broadcasting sensors (beacons).
 *
 * The watch keeps advertising / staying connected to the phone; whether the controller can scan
 * while connected as a peripheral is controller-dependent (an error is returned if not).
 * @{
 */

/**
 * @brief Called for each advertising report during a burst, on the Bluetooth host task: copy what
 * is needed and return quickly.
 * @param addr Advertiser address, least significant byte first (as on air).
 * @param rssi Signal strength in dBm.
 * @param data Advertising data (AD structures).
 * @param len Length of @p data.
 * @param ctx Context passed to pbl_bt_observer_scan().
 */
typedef void (*pbl_bt_observer_report_cb)(const uint8_t addr[6], int8_t rssi, const uint8_t *data,
                                          uint8_t len, void *ctx);

/**
 * @brief Called when a burst ends (on the Bluetooth host task).
 * @param ctx Context passed to pbl_bt_observer_scan().
 */
typedef void (*pbl_bt_observer_done_cb)(void *ctx);

/**
 * @brief Scan passively for @p duration_ms, reporting every advertisement (no duplicate filter).
 * @return 0, or a negative NimBLE error code (e.g. busy, or not supported while connected).
 */
int pbl_bt_observer_scan(uint32_t duration_ms, pbl_bt_observer_report_cb report,
                         pbl_bt_observer_done_cb done, void *ctx);

/** @brief Stop a burst in progress, if any. */
void pbl_bt_observer_stop(void);

/** @} */
