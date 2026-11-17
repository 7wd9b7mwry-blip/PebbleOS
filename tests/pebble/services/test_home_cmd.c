/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */
/* SPDX-License-Identifier: Apache-2.0 */

#include <clar.h>

#include <string.h>

#include <pbl/services/habit/home_cmd.h>

// flags; manufacturer data 0xFFFF: 'Q' v1, issued 0x6550A0B0, vibe (1), sleep (1), 30 min, trial; tag DE AD BE EF
static const uint8_t s_adv[] = {
    0x02, 0x01, 0x06,
    0x12, 0xFF, 0xFF, 0xFF, 0x51, 0x01, 0xB0, 0xA0, 0x50, 0x65, 0x01, 0x01, 0x1E, 0x00, 0x01,
    0xDE, 0xAD, 0xBE, 0xEF,
};

void test_home_cmd__parses_a_command(void) {
  HomeCmd c;
  cl_assert(home_cmd_parse(s_adv, sizeof(s_adv), &c));
  cl_assert_equal_i(c.issued, 0x6550A0B0);
  cl_assert_equal_i(c.op, HomeCmdOp_Vibe);
  cl_assert_equal_i(c.program, 1);
  cl_assert_equal_i(c.minutes, 30);
  cl_assert_equal_i(c.flags, 1);
  cl_assert_equal_i(c.signed_bytes[0], 0x51);
  cl_assert_equal_i(c.signed_bytes[10], 0x01);
  cl_assert_equal_i(c.tag[0], 0xDE);
  cl_assert_equal_i(c.tag[3], 0xEF);
}

void test_home_cmd__ignores_other_adverts(void) {
  HomeCmd c;
  uint8_t air_log[sizeof(s_adv)];
  memcpy(air_log, s_adv, sizeof(air_log));
  air_log[7] = 0x50;  // the watch's own air log
  cl_assert(!home_cmd_parse(air_log, sizeof(air_log), &c));
  cl_assert(!home_cmd_parse(s_adv, 10, &c));  // cut short
  uint8_t ibeacon[] = {0x02, 0x01, 0x06, 0x1A, 0xFF, 0x4C, 0x00, 0x02, 0x15, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                       13, 14, 15, 16, 0, 1, 0, 2, 0xC5};
  cl_assert(!home_cmd_parse(ibeacon, sizeof(ibeacon), &c));
}

void test_home_cmd__runs_each_command_once_while_fresh(void) {
  HomeCmd c;
  cl_assert(home_cmd_parse(s_adv, sizeof(s_adv), &c));
  cl_assert(home_cmd_is_new(&c, 0, c.issued + 10));
  cl_assert(!home_cmd_is_new(&c, c.issued, c.issued + 10));        // already run
  cl_assert(!home_cmd_is_new(&c, 0, c.issued + 301));              // stale
  cl_assert(home_cmd_is_new(&c, 0, c.issued - 60));                // the watch's clock a bit behind
}
