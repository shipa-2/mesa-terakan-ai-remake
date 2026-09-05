/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Which physical device the driver's own tests run on.
 *
 * The tests are written once and run on every generation. Evergreen and Northern Islands are
 * expected to pass all of them; TeraScale 1 (R600/R700) runs the same tests and reports what it
 * cannot do yet as ordinary failures rather than being excluded from the run. That makes the gap
 * between the two a list that a test run produces, instead of something to be guessed at.
 *
 * Each test used to carry its own copy of this loop, and each copy excluded TeraScale 1 by name.
 * The selection lives here now so that changing it is one edit rather than fifty.
 *
 * `TERAKAN_TEST_DEVICE` picks between devices on a machine that has more than one: it is matched
 * as a substring of the device name, so `TERAKAN_TEST_DEVICE="TeraScale 1"` takes the older part
 * and a chip name takes a particular one. Without it, TeraScale 1 is passed over and the newer
 * generation is used, which keeps an ordinary run on a mixed machine measuring what it always
 * measured.
 *
 * A run asked for on TeraScale 1 will get through device creation, allocation and pipeline
 * compilation and then fail at submission with `VK_ERROR_DEVICE_LOST`, because
 * `terakan_queue_submit` refuses to submit that generation unless
 * `TERAKAN_DEBUG_TERASCALE_1_SUBMIT=1` is set. That is the honest report of where the port stands,
 * and it is deliberately the safe one: submitting real work on that path has locked the adapter
 * and needed a reboot, so the guard is not something a test run should lift on its own.
 */

#ifndef TERAKAN_TEST_DEVICE_H
#define TERAKAN_TEST_DEVICE_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static inline bool
terakan_test_device_matches(char const * const device_name)
{
   if (strstr(device_name, "(Terakan)") == NULL) {
      return false;
   }
   char const * const wanted = getenv("TERAKAN_TEST_DEVICE");
   if (wanted == NULL || wanted[0] == '\0') {
      return strstr(device_name, "TeraScale 1") == NULL;
   }
   return strstr(device_name, wanted) != NULL;
}

#endif
