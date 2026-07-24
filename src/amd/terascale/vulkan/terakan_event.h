/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_EVENT_H
#define TERAKAN_EVENT_H

#include "terakan_bo.h"

#include "vk_object.h"

struct terakan_event {
   struct vk_object_base base;
   struct terakan_bo *bo;
   uint32_t *status;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(terakan_event, base, VkEvent, VK_OBJECT_TYPE_EVENT)

#endif
