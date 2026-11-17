/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/habit/home_cmd.h>

#include <string.h>

bool home_cmd_parse(const uint8_t *data, size_t len, HomeCmd *out) {
  size_t i = 0;
  while (i + 1 < len) {
    const uint8_t n = data[i];
    if (n == 0 || i + 1 + n > len) {
      return false;
    }
    const uint8_t *p = &data[i + 1];
    // type, company 0xFFFF, then the command
    if (p[0] == 0xFF && n >= 3 + HOME_CMD_SIGNED_LEN + HOME_CMD_TAG_LEN && p[1] == 0xFF && p[2] == 0xFF &&
        p[3] == HOME_CMD_MAGIC && p[4] == HOME_CMD_VERSION) {
      const uint8_t *c = &p[3];
      memcpy(out->signed_bytes, c, HOME_CMD_SIGNED_LEN);
      out->issued = (uint32_t)c[2] | ((uint32_t)c[3] << 8) | ((uint32_t)c[4] << 16) | ((uint32_t)c[5] << 24);
      out->op = c[6];
      out->program = c[7];
      out->minutes = (uint16_t)(c[8] | (c[9] << 8));
      out->flags = c[10];
      memcpy(out->tag, &c[HOME_CMD_SIGNED_LEN], HOME_CMD_TAG_LEN);
      return true;
    }
    i += 1 + n;
  }
  return false;
}

bool home_cmd_is_new(const HomeCmd *cmd, uint32_t last_issued, uint32_t now) {
  if (cmd->issued <= last_issued) {
    return false;
  }
  const uint32_t age = now >= cmd->issued ? now - cmd->issued : cmd->issued - now;
  return age <= HOME_CMD_MAX_AGE_S;
}
