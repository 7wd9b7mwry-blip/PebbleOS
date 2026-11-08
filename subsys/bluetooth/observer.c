/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/bluetooth/observer.h>
#include <pbl/logging/logging.h>

#include <host/ble_gap.h>
#include <host/ble_hs.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

static pbl_bt_observer_report_cb s_report;
static pbl_bt_observer_done_cb s_done;
static void *s_ctx;

static int prv_disc_event(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
    case BLE_GAP_EVENT_DISC:
      if (s_report) {
        s_report(event->disc.addr.val, event->disc.rssi, event->disc.data,
                 event->disc.length_data, s_ctx);
      }
      break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
      if (s_done) {
        s_done(s_ctx);
      }
      break;
    default:
      break;
  }
  return 0;
}

int pbl_bt_observer_scan(uint32_t duration_ms, pbl_bt_observer_report_cb report,
                         pbl_bt_observer_done_cb done, void *ctx) {
  uint8_t own_addr_type;
  int rc = ble_hs_id_infer_auto(0, &own_addr_type);
  if (rc != 0) {
    return -rc;
  }
  // passive, continuous within the burst (window = interval), every report delivered
  const struct ble_gap_disc_params params = {
      .itvl = 0x0060,    // 60 ms
      .window = 0x0060,  // 60 ms
      .filter_policy = 0,
      .limited = 0,
      .passive = 1,
      .filter_duplicates = 0,
  };
  s_report = report;
  s_done = done;
  s_ctx = ctx;
  rc = ble_gap_disc(own_addr_type, (int32_t)duration_ms, &params, prv_disc_event, nullptr);
  if (rc != 0) {
    PBL_LOG_DBG("observer: scan not started (%d)", rc);
    s_report = nullptr;
    s_done = nullptr;
    return -rc;
  }
  return 0;
}

void pbl_bt_observer_stop(void) {
  if (ble_gap_disc_active()) {
    ble_gap_disc_cancel();
  }
}
