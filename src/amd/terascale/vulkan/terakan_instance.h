/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef TERAKAN_INSTANCE_H
#define TERAKAN_INSTANCE_H

#include "vk_instance.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(VK_USE_PLATFORM_WAYLAND_KHR) || defined(VK_USE_PLATFORM_XCB_KHR) ||                    \
   defined(VK_USE_PLATFORM_XLIB_KHR) || defined(VK_USE_PLATFORM_WIN32_KHR)
#define TERAKAN_USE_WSI_PLATFORM
#endif

#define TERAKAN_API_VERSION VK_MAKE_API_VERSION(0, 1, 0, VK_HEADER_VERSION)

/* "Debug" options are intended to be potentially useful to both driver developers and users, mainly
 * for pinpointing the causes of issues (especially those observable in real applications), as well
 * as for performance analysis.
 * They are available in release builds.
 */

enum {
   TERAKAN_DEBUG_STARTUP = (uint64_t)1 << 0,
};

/* "Development test" options are provided primarily for unit or regression testing of very specific
 * scenarios inside the driver, and generally offer nothing valuable to users.
 * They are eliminated from release builds, and not intended to be documented outside the code
 * itself, as their usage may result in significant performance costs or things done very wrong
 * ways, and they may be checked in frequently called functions, while providing no advantages to
 * driver users.
 */

#ifndef NDEBUG
#define TERAKAN_DEVTEST
#endif

#ifdef TERAKAN_DEVTEST
enum {
   /* Switch to a new indirect buffer when beginning or ending a query.
    *
    * This causes query sample accumulation operations to be performed for all queries with both a
    * beginning and an end, allowing for debugging of whether accumulation is performed correctly,
    * and that query results are not affected by other work potentially executed by the GPU between
    * indirect buffer submissions if a command buffer ends up being split into multiple indirect
    * buffers when a query is active in it.
    *
    * In addition, this may be used to potentially observe how the GPU switching to other work
    * between indirect buffer submissions may affect the stability of timestamp queries, especially
    * when performing multiple measurements of the time it takes to perform the same amount of work.
    *
    * Also see `devtest_split_indirect_buffer_after_actions`, which can be used to introduce more
    * split points within a query.
    */
   TERAKAN_DEVTEST_SPLIT_INDIRECT_BUFFER_AT_QUERY_BEGIN_END = (uint64_t)1 << 0,
};
#endif

struct terakan_instance;

typedef void (*terakan_instance_destroy_fn)(struct terakan_instance * instance);

/* Partially implemented by the winsys. */
struct terakan_instance {
   struct vk_instance vk;

   terakan_instance_destroy_fn destroy_fn;

   uint64_t debug_flags;

#ifdef TERAKAN_DEVTEST
   uint64_t devtest_flags;

   /* For testing multiple indirect buffer submissions for one Vulkan command buffer.
    *
    * If > 0, this is the number of draws or dispatches (done by the application or internally
    * within the driver) after which a new indirect buffer will be started, causing the hardware
    * state configuration to be reapplied in the new indirect buffer, and samples for queries active
    * during the split to be accumulated from multiple indirect buffer submissions.
    *
    * For query debugging, also see `TERAKAN_DEVTEST_SPLIT_INDIRECT_BUFFER_AT_QUERY_BEGIN_END`.
    * This may be useful for splitting the command buffer even further.
    * Specifically for pipeline statistics query sample accumulation, a good test case is Sascha
    * Willems's pipeline statistics example, which, as of the commit
    * bb2f03ad5059c3f92ffaed4e2a38980c42efb07d, draws each object on the grid using a separate draw
    * command.
    */
   int64_t devtest_split_indirect_buffer_after_actions;
#endif

   /* Binding allocation in the physical device limits. */
   /* From 4 to 8. The rest of UAV bindings will be used for storage images. */
   uint32_t max_per_stage_storage_buffers;
   /* Uniform buffers, sampled images and input attachments are allocated from one range. */
   uint32_t max_per_stage_uniform_buffers;
   uint32_t max_per_stage_sampled_images;
   uint32_t max_per_stage_input_attachments;
};

VK_DEFINE_HANDLE_CASTS(terakan_instance, vk.base, VkInstance, VK_OBJECT_TYPE_INSTANCE)

void terakan_instance_finish(struct terakan_instance * instance);

/* The winsys must set the physical device enumeration function after initializing the instance
 * base.
 */
VkResult terakan_instance_init(struct terakan_instance * instance,
                               VkInstanceCreateInfo const * create_info,
                               terakan_instance_destroy_fn destroy_fn,
                               VkAllocationCallbacks const * allocator);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_INSTANCE_H */
