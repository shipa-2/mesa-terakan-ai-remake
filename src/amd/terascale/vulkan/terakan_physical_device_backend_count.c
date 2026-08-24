/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#include "terakan_physical_device_backend_count.h"

#include "util/u_math.h"

#include <assert.h>

unsigned
terakan_physical_device_backend_count_to_log2(uint32_t const backend_count)
{
   assert(backend_count != 0);
   return util_logbase2_ceil(backend_count);
}
