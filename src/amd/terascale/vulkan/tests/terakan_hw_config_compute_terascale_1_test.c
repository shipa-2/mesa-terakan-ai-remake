/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_hw_config_compute_terascale_1.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                           \
   do {                                                                                            \
      if (!(condition)) {                                                                          \
         fprintf(stderr, "CHECK failed at %s:%u: %s\n", __FILE__, __LINE__, #condition);           \
         abort();                                                                                  \
      }                                                                                            \
   } while (0)

int
main(void)
{
   CHECK(terakan_hw_config_compute_terascale_1_packet_dwords() == 0);
   return 0;
}
