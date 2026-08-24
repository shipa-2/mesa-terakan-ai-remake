/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_physical_device_backend_count.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                          \
   do {                                                                                            \
      if (!(condition)) {                                                                          \
         fprintf(stderr, "CHECK failed at %s:%u: %s\n", __FILE__, __LINE__, #condition);           \
         abort();                                                                                  \
      }                                                                                            \
   } while (0)

int
main(void)
{
   /* The power-of-two counts every publicly documented R600/R700 chip actually reports. */
   CHECK(terakan_physical_device_backend_count_to_log2(1) == 0);
   CHECK(terakan_physical_device_backend_count_to_log2(2) == 1);
   CHECK(terakan_physical_device_backend_count_to_log2(4) == 2);

   /* Non-power-of-two inputs round up rather than down, so a real count is never
    * underrepresented.
    */
   CHECK(terakan_physical_device_backend_count_to_log2(3) == 2);
   CHECK(terakan_physical_device_backend_count_to_log2(5) == 3);

   return 0;
}
