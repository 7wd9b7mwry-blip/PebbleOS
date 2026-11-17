/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

//! Home commands: Home Assistant -> watch without a phone notification. A home ESPHome device (the bathroom MSR-2)
//! advertises the latest command for a few minutes; the habit service listens in short scan bursts and runs a command
//! once (newer `issued` only, at most HOME_CMD_MAX_AGE_S old by the watch's clock), if its tag checks out:
//! AES-128-CMAC with the key shared with the device (set once: "PDB key <32 hex>"), first 4 bytes.
//!
//! On air: manufacturer data, company 0xFFFF: 'Q' (0x51), version 1, issued (u32 unix s), op (u8), program (u8),
//! minutes (u16), flags (u8: bit 0 trial), tag (4 bytes) over 'Q' .. flags. Little endian. Pure: the caller does the
//! CMAC.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HOME_CMD_MAGIC 0x51
#define HOME_CMD_VERSION 1
#define HOME_CMD_SIGNED_LEN 11
#define HOME_CMD_TAG_LEN 4
#define HOME_CMD_MAX_AGE_S 300

typedef enum {
  HomeCmdOp_Vibe = 1,     //!< play `program` for `minutes`
  HomeCmdOp_Stop = 2,     //!< stop the program playing
  HomeCmdOp_Quiet = 3,    //!< habit alerts off for `minutes` (0: on again)
  HomeCmdOp_Holdout = 4,  //!< trial holdout share: `minutes` is the percent
} HomeCmdOp;

typedef struct {
  uint32_t issued;
  uint8_t op;
  uint8_t program;
  uint16_t minutes;
  uint8_t flags;
  uint8_t signed_bytes[HOME_CMD_SIGNED_LEN];  //!< what the tag covers
  uint8_t tag[HOME_CMD_TAG_LEN];
} HomeCmd;

//! Find and parse a home command in an advertisement's data (AD structures).
bool home_cmd_parse(const uint8_t *data, size_t len, HomeCmd *out);

//! Whether to run it: newer than the last one run, and fresh by the watch's clock.
bool home_cmd_is_new(const HomeCmd *cmd, uint32_t last_issued, uint32_t now);
