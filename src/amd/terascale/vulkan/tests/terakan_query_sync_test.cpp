/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* Query pool reuse and cross-stage ordering, for occlusion, timestamp and pipeline-statistics
 * queries at once.
 *
 * Query pool memory is written by the ME -- through CP DMA for the availability, through
 * EVENT_WRITE for the samples -- while vkCmdCopyQueryPoolResults reads it from a vertex shader in a
 * meta draw. The ME runs far ahead of the shader pipeline, so a vkCmdResetQueryPool recorded right
 * after a copy of the same queries can land in the middle of that copy unless something waits in
 * between. Copy results, reset, use the same queries again, copy again is not an exotic pattern; it
 * is what every application that keeps one query pool across frames does.
 *
 * Several generations of a whole poolful of each of the three queries are recorded back to back
 * into a single command buffer, each query with a different, known answer: the occlusion query is
 * fed a scissor whose area is unique to its (generation, query) pair, and the timestamps must come
 * out ordered. Each generation's results are copied out before the next generation resets the
 * queries, so every one of those copies is recorded directly in front of a reset of what it reads.
 * A query whose occlusion count is another query's answer, or which reads back as unavailable, is
 * that ordering being lost.
 *
 * Note what this does and does not demonstrate. It found two real bugs -- the destination UAV of
 * vkCmdCopyQueryPoolResults described a colour surface the kernel rejected outright, and the
 * pipeline-statistics destination offsets were built in VkQueryPipelineStatisticFlags bit order
 * while the copy shader reads them in hardware counter order -- and it holds both fixed. It does
 * not demonstrate the reset-versus-copy ordering: with that wait removed the test still passes on
 * Caicos, at eight generations of sixty-four queries. The wait is kept because the ordering
 * requirement is real and it costs nothing when no copy is outstanding, not because anything here
 * proves the hardware needs it.
 */

#include <vulkan/vulkan.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "terakan_test_device.h"

#define VK_CHECK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_result = (expression);                                                  \
      if (check_result != VK_SUCCESS) {                                                            \
         std::fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_result);          \
         return 1;                                                                                 \
      }                                                                                            \
   } while (false)

namespace {

constexpr uint32_t kTargetWidth = 64;
constexpr uint32_t kTargetHeight = 64;

/* Enough generations that a reset overtaking a copy has many chances to show, without making the
 * command buffer unwieldy.
 */
constexpr uint32_t kGenerations = 8;

/* The full-target quad is six vertices forming two triangles, which is what the pipeline-statistics
 * query must report every generation.
 */
constexpr uint64_t kVerticesPerDraw = 6;
constexpr uint64_t kPrimitivesPerDraw = 2;

/* Every generation uses the whole pool, not one query of it. A copy of a single query is over
 * almost before it starts, which leaves a reset recorded right behind it very little to overtake;
 * a copy of a poolful gives the race room to happen.
 */
constexpr uint32_t kQueriesPerPool = 64;

/* A scissor unique to each generation. Widths and heights are deliberately co-prime-ish so that no
 * two generations share an area, and none is a multiple of another.
 */
VkRect2D
query_scissor(uint32_t const generation, uint32_t const query)
{
   /* Distinct for every (generation, query) pair within the target, so a value that belongs to
    * another generation or another query of the same generation is recognizable as such.
    */
   uint32_t const index = generation * kQueriesPerPool + query;
   uint32_t const width = 1 + index % 61;
   uint32_t const height = 1 + (index / 61 + index % 7) % 59;
   return {{0, 0}, {width, height}};
}

uint64_t
query_area(uint32_t const generation, uint32_t const query)
{
   VkRect2D const scissor = query_scissor(generation, query);
   return (uint64_t)scissor.extent.width * scissor.extent.height;
}

/* Each generation's results land at its own offset, so a copy that never happened reads back as the
 * poison the buffer was filled with rather than as another generation's answer.
 */
constexpr uint64_t kPoison = 0xDEADBEEFDEADBEEFull;

/* Occlusion: count, availability. Statistics: two counters, availability. Timestamp: value,
 * availability.
 */
constexpr VkDeviceSize kOcclusionStride = 2 * sizeof(uint64_t);
constexpr VkDeviceSize kStatisticsStride = 3 * sizeof(uint64_t);
constexpr VkDeviceSize kTimestampStride = 2 * sizeof(uint64_t);
/* The same two counters and the availability, copied without VK_QUERY_RESULT_64_BIT. The 32-bit
 * and 64-bit destination offsets are computed by separate code paths in the driver, so a table that
 * is right in one width can still be wrong in the other.
 */
constexpr VkDeviceSize kStatistics32Stride = 3 * sizeof(uint32_t);

uint32_t const query_sync_vertex_spirv[] = {
#include "terakan_color_msaa_write.vert.spv.h"
};
uint32_t const query_sync_fragment_spirv[] = {
#include "terakan_color_msaa_write.frag.spv.h"

};

uint32_t
find_memory_type(VkPhysicalDevice physical_device, uint32_t bits, VkMemoryPropertyFlags flags)
{
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
   for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
         return i;
   }
   return UINT32_MAX;
}

struct HostBuffer {
   VkBuffer buffer = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   uint64_t * mapping = nullptr;
};

VkResult
create_host_buffer(VkPhysicalDevice physical_device, VkDevice device, VkDeviceSize size,
                   HostBuffer * out)
{
   VkBufferCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkResult result = vkCreateBuffer(device, &info, nullptr, &out->buffer);
   if (result != VK_SUCCESS)
      return result;
   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(device, out->buffer, &requirements);
   VkMemoryAllocateInfo const allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   result = vkAllocateMemory(device, &allocation, nullptr, &out->memory);
   if (result != VK_SUCCESS)
      return result;
   result = vkBindBufferMemory(device, out->buffer, out->memory, 0);
   if (result != VK_SUCCESS)
      return result;
   return vkMapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0,
                      reinterpret_cast<void **>(&out->mapping));
}

} // namespace

int
main()
{
   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-query-sync-test",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo const instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application_info,
   };
   VkInstance instance;
   VK_CHECK(vkCreateInstance(&instance_info, nullptr, &instance));

   uint32_t physical_device_count = 0;
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr));
   std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices.data()));
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   uint32_t queue_family = UINT32_MAX;
   VkPhysicalDeviceProperties properties = {};
   for (VkPhysicalDevice candidate : physical_devices) {
      vkGetPhysicalDeviceProperties(candidate, &properties);
      if (!terakan_test_device_matches(properties.deviceName))
         continue;
      uint32_t family_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
      std::vector<VkQueueFamilyProperties> families(family_count);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
      for (uint32_t i = 0; i < family_count; ++i) {
         if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            physical_device = candidate;
            queue_family = i;
            break;
         }
      }
      if (physical_device != VK_NULL_HANDLE)
         break;
   }
   if (physical_device == VK_NULL_HANDLE) {
      std::fprintf(stderr, "Terakan graphics device not found\n");
      return TERAKAN_TEST_DEVICE_NOT_FOUND_STATUS;
   }
   /* A timestamp period of zero would mean the queue family does not support timestamps at all. */
   uint32_t family_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
   std::vector<VkQueueFamilyProperties> families(family_count);
   vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
   uint32_t const timestamp_valid_bits = families[queue_family].timestampValidBits;
   std::fprintf(stderr, "device=%s queue_family=%u generations=%u timestamp_valid_bits=%u\n",
                properties.deviceName, queue_family, kGenerations, timestamp_valid_bits);
   if (timestamp_valid_bits == 0) {
      std::fprintf(stderr, "the queue family reports no valid timestamp bits\n");
      return 1;
   }

   float const priority = 1.0F;
   VkDeviceQueueCreateInfo const queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkPhysicalDeviceFeatures enabled_features = {};
   enabled_features.occlusionQueryPrecise = VK_TRUE;
   enabled_features.pipelineStatisticsQuery = VK_TRUE;
   VkDeviceCreateInfo const device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .pEnabledFeatures = &enabled_features,
   };
   VkDevice device;
   VK_CHECK(vkCreateDevice(physical_device, &device_info, nullptr, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, queue_family, 0, &queue);

   /* All generations share query index 0 of each pool -- reusing it is the point. */
   VkQueryPoolCreateInfo occlusion_pool_info = {
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_OCCLUSION,
      .queryCount = kQueriesPerPool,
   };
   VkQueryPool occlusion_pool;
   VK_CHECK(vkCreateQueryPool(device, &occlusion_pool_info, nullptr, &occlusion_pool));
   VkQueryPoolCreateInfo statistics_pool_info = {
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS,
      .queryCount = kQueriesPerPool,
      .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT,
   };
   VkQueryPool statistics_pool;
   VK_CHECK(vkCreateQueryPool(device, &statistics_pool_info, nullptr, &statistics_pool));
   VkQueryPoolCreateInfo timestamp_pool_info = {
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_TIMESTAMP,
      .queryCount = kQueriesPerPool,
   };
   VkQueryPool timestamp_pool;
   VK_CHECK(vkCreateQueryPool(device, &timestamp_pool_info, nullptr, &timestamp_pool));

   HostBuffer occlusion_results, statistics_results, statistics32_results, timestamp_results;
   VK_CHECK(create_host_buffer(physical_device, device, kOcclusionStride * kGenerations * kQueriesPerPool,
                               &occlusion_results));
   VK_CHECK(create_host_buffer(physical_device, device, kStatisticsStride * kGenerations * kQueriesPerPool,
                               &statistics_results));
   VK_CHECK(create_host_buffer(physical_device, device, kStatistics32Stride * kGenerations * kQueriesPerPool,
                               &statistics32_results));
   VK_CHECK(create_host_buffer(physical_device, device, kTimestampStride * kGenerations * kQueriesPerPool,
                               &timestamp_results));
   for (uint32_t i = 0; i < kGenerations * kQueriesPerPool * 2; ++i)
      occlusion_results.mapping[i] = kPoison;
   for (uint32_t i = 0; i < kGenerations * kQueriesPerPool * 3; ++i)
      statistics_results.mapping[i] = kPoison;
   for (uint32_t i = 0; i < kGenerations * kQueriesPerPool * 2; ++i)
      timestamp_results.mapping[i] = kPoison;
   uint32_t * const statistics32_mapping =
      reinterpret_cast<uint32_t *>(statistics32_results.mapping);
   for (uint32_t i = 0; i < kGenerations * kQueriesPerPool * 3; ++i)
      statistics32_mapping[i] = (uint32_t)kPoison;

   VkImageCreateInfo const target_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {kTargetWidth, kTargetHeight, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage target;
   VK_CHECK(vkCreateImage(device, &target_info, nullptr, &target));
   VkMemoryRequirements target_requirements;
   vkGetImageMemoryRequirements(device, target, &target_requirements);
   VkMemoryAllocateInfo const target_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = target_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, target_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
   };
   VkDeviceMemory target_memory;
   VK_CHECK(vkAllocateMemory(device, &target_allocation, nullptr, &target_memory));
   VK_CHECK(vkBindImageMemory(device, target, target_memory, 0));
   VkImageViewCreateInfo const target_view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = target,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   VkImageView target_view;
   VK_CHECK(vkCreateImageView(device, &target_view_info, nullptr, &target_view));

   VkAttachmentDescription const attachment = {
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   };
   VkAttachmentReference const attachment_reference = {
      .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   };
   VkSubpassDescription const subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &attachment_reference,
   };
   VkRenderPassCreateInfo const render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
   };
   VkRenderPass render_pass;
   VK_CHECK(vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass));
   VkFramebufferCreateInfo const framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 1,
      .pAttachments = &target_view,
      .width = kTargetWidth,
      .height = kTargetHeight,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   VK_CHECK(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer));

   VkPushConstantRange const push_range = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 4};
   VkPipelineLayoutCreateInfo const pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_range,
   };
   VkPipelineLayout pipeline_layout;
   VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout));

   VkShaderModuleCreateInfo const vertex_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(query_sync_vertex_spirv),
      .pCode = query_sync_vertex_spirv,
   };
   VkShaderModule vertex_module;
   VK_CHECK(vkCreateShaderModule(device, &vertex_module_info, nullptr, &vertex_module));
   VkShaderModuleCreateInfo const fragment_module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(query_sync_fragment_spirv),
      .pCode = query_sync_fragment_spirv,
   };
   VkShaderModule fragment_module;
   VK_CHECK(vkCreateShaderModule(device, &fragment_module_info, nullptr, &fragment_module));
   VkPipelineShaderStageCreateInfo const stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex_module, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment_module, .pName = "main"},
   };
   VkPipelineVertexInputStateCreateInfo const vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
   };
   VkPipelineInputAssemblyStateCreateInfo const input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport const viewport = {0.0F, 0.0F, (float)kTargetWidth, (float)kTargetHeight, 0.0F, 1.0F};
   VkPipelineViewportStateCreateInfo const viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1,
   };
   VkPipelineRasterizationStateCreateInfo const rasterization = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0F,
   };
   VkPipelineMultisampleStateCreateInfo const multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   VkPipelineColorBlendAttachmentState const blend_attachment = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo const color_blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_attachment,
   };
   VkDynamicState const dynamic_states[] = {VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo const dynamic_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 1,
      .pDynamicStates = dynamic_states,
   };
   VkGraphicsPipelineCreateInfo const pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2, .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterization,
      .pMultisampleState = &multisample,
      .pColorBlendState = &color_blend,
      .pDynamicState = &dynamic_state,
      .layout = pipeline_layout,
      .renderPass = render_pass,
   };
   VkPipeline pipeline;
   VK_CHECK(
      vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));

   VkCommandPoolCreateInfo const command_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = queue_family,
   };
   VkCommandPool command_pool;
   VK_CHECK(vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool));
   VkCommandBufferAllocateInfo const command_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command_buffer;
   VK_CHECK(vkAllocateCommandBuffers(device, &command_allocate_info, &command_buffer));
   VkCommandBufferBeginInfo const begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));

   VkQueryResultFlags const result_flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT |
                                           VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
   float const colour[4] = {1.0F, 0.5F, 0.25F, 1.0F};
   VkClearValue const clear_value = {.color = {.float32 = {0.0F, 0.0F, 0.0F, 1.0F}}};

   for (uint32_t generation = 0; generation < kGenerations; ++generation) {
      /* From the second generation on, this reset is recorded directly after the previous
       * generation's copies and must not overtake them.
       */
      vkCmdResetQueryPool(command_buffer, occlusion_pool, 0, kQueriesPerPool);
      vkCmdResetQueryPool(command_buffer, statistics_pool, 0, kQueriesPerPool);
      vkCmdResetQueryPool(command_buffer, timestamp_pool, 0, kQueriesPerPool);

      VkRenderPassBeginInfo const render_begin = {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = render_pass,
         .framebuffer = framebuffer,
         .renderArea = {{0, 0}, {kTargetWidth, kTargetHeight}},
         .clearValueCount = 1,
         .pClearValues = &clear_value,
      };
      vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(colour), colour);
      for (uint32_t query = 0; query < kQueriesPerPool; ++query) {
         VkRect2D const scissor = query_scissor(generation, query);
         vkCmdSetScissor(command_buffer, 0, 1, &scissor);
         vkCmdBeginQuery(command_buffer, occlusion_pool, query, VK_QUERY_CONTROL_PRECISE_BIT);
         vkCmdBeginQuery(command_buffer, statistics_pool, query, 0);
         vkCmdDraw(command_buffer, 6, 1, 0, 0);
         vkCmdEndQuery(command_buffer, statistics_pool, query);
         vkCmdEndQuery(command_buffer, occlusion_pool, query);
      }
      vkCmdEndRenderPass(command_buffer);

      for (uint32_t query = 0; query < kQueriesPerPool; ++query) {
         vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamp_pool,
                             query);
      }

      /* One copy per pool, covering every query at once, so that the resets at the top of the next
       * iteration are recorded directly behind a long-running copy.
       */
      VkDeviceSize const generation_base = (VkDeviceSize)generation * kQueriesPerPool;
      vkCmdCopyQueryPoolResults(command_buffer, occlusion_pool, 0, kQueriesPerPool,
                                occlusion_results.buffer, kOcclusionStride * generation_base,
                                kOcclusionStride, result_flags);
      vkCmdCopyQueryPoolResults(command_buffer, statistics_pool, 0, kQueriesPerPool,
                                statistics_results.buffer, kStatisticsStride * generation_base,
                                kStatisticsStride, result_flags);
      vkCmdCopyQueryPoolResults(command_buffer, statistics_pool, 0, kQueriesPerPool,
                                statistics32_results.buffer, kStatistics32Stride * generation_base,
                                kStatistics32Stride,
                                result_flags & ~(VkQueryResultFlags)VK_QUERY_RESULT_64_BIT);
      vkCmdCopyQueryPoolResults(command_buffer, timestamp_pool, 0, kQueriesPerPool,
                                timestamp_results.buffer, kTimestampStride * generation_base,
                                kTimestampStride, result_flags);
   }

   VkMemoryBarrier const host_ready = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_ready, 0, nullptr, 0, nullptr);
   VK_CHECK(vkEndCommandBuffer(command_buffer));

   VkFenceCreateInfo const fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   VK_CHECK(vkCreateFence(device, &fence_info, nullptr, &fence));
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, fence));
   VkResult const wait_result = vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull);
   if (wait_result != VK_SUCCESS) {
      std::fprintf(stderr, "Fence wait failed with VkResult %d\n", wait_result);
      return 1;
   }

   /* Timestamps are only meaningful in their valid bits; the rest is undefined. */
   uint64_t const timestamp_mask =
      timestamp_valid_bits >= 64 ? UINT64_MAX : ((uint64_t)1 << timestamp_valid_bits) - 1;

   uint32_t failures = 0;
   uint64_t previous_timestamp = 0;
   bool previous_timestamp_valid = false;
   /* Only the first failure of each kind is printed; a lost race tends to spoil many results at
    * once, and a screenful of them says nothing the first one does not.
    */
   bool reported_occlusion = false, reported_statistics = false, reported_statistics32 = false,
        reported_timestamp = false;
   for (uint32_t generation = 0; generation < kGenerations; ++generation) {
      for (uint32_t query = 0; query < kQueriesPerPool; ++query) {
         uint32_t const index = generation * kQueriesPerPool + query;
         uint64_t const * const occlusion = occlusion_results.mapping + 2 * index;
         uint64_t const * const statistics = statistics_results.mapping + 3 * index;
         uint32_t const * const statistics32 = statistics32_mapping + 3 * index;
         uint64_t const * const timestamp = timestamp_results.mapping + 2 * index;

         uint64_t const expected_area = query_area(generation, query);
         if (occlusion[1] == 0 || occlusion[1] == kPoison) {
            if (!reported_occlusion) {
               std::fprintf(stderr,
                            "generation %u query %u: occlusion availability = 0x%" PRIx64 "\n",
                            generation, query, occlusion[1]);
               reported_occlusion = true;
            }
            ++failures;
         } else if (occlusion[0] != expected_area) {
            if (!reported_occlusion) {
               std::fprintf(stderr,
                            "generation %u query %u: occlusion = %" PRIu64 ", expected %" PRIu64
                            "%s\n",
                            generation, query, occlusion[0], expected_area,
                            occlusion[0] == kPoison ? " (the copy never ran)" : "");
               /* Naming whose answer it is separates a lost reset-versus-copy race from a
                * miscounted query.
                */
               for (uint32_t other = 0; other < kGenerations * kQueriesPerPool; ++other) {
                  if (other != index && occlusion[0] == query_area(other / kQueriesPerPool,
                                                                   other % kQueriesPerPool)) {
                     std::fprintf(stderr,
                                  "  that is generation %u query %u's answer: something overtook "
                                  "this copy\n",
                                  other / kQueriesPerPool, other % kQueriesPerPool);
                     break;
                  }
               }
               reported_occlusion = true;
            }
            ++failures;
         }

         if (statistics[2] == 0 || statistics[2] == kPoison) {
            if (!reported_statistics) {
               std::fprintf(stderr,
                            "generation %u query %u: statistics availability = 0x%" PRIx64 "\n",
                            generation, query, statistics[2]);
               reported_statistics = true;
            }
            ++failures;
         } else if (statistics[0] != kVerticesPerDraw || statistics[1] != kPrimitivesPerDraw) {
            if (!reported_statistics) {
               std::fprintf(stderr,
                            "generation %u query %u: statistics = %" PRIu64 " vertices, %" PRIu64
                            " primitives, expected %" PRIu64 " and %" PRIu64 "\n",
                            generation, query, statistics[0], statistics[1], kVerticesPerDraw,
                            kPrimitivesPerDraw);
               reported_statistics = true;
            }
            ++failures;
         }

         if (statistics32[2] == 0 || statistics32[2] == (uint32_t)kPoison) {
            if (!reported_statistics32) {
               std::fprintf(stderr,
                            "generation %u query %u: 32-bit statistics availability = 0x%" PRIx32
                            "\n",
                            generation, query, statistics32[2]);
               reported_statistics32 = true;
            }
            ++failures;
         } else if (statistics32[0] != kVerticesPerDraw || statistics32[1] != kPrimitivesPerDraw) {
            if (!reported_statistics32) {
               std::fprintf(stderr,
                            "generation %u query %u: 32-bit statistics = %" PRIu32
                            " vertices, %" PRIu32 " primitives, expected %" PRIu64 " and %" PRIu64
                            "\n",
                            generation, query, statistics32[0], statistics32[1], kVerticesPerDraw,
                            kPrimitivesPerDraw);
               reported_statistics32 = true;
            }
            ++failures;
         }

         if (timestamp[1] == 0 || timestamp[1] == kPoison) {
            if (!reported_timestamp) {
               std::fprintf(stderr,
                            "generation %u query %u: timestamp availability = 0x%" PRIx64 "\n",
                            generation, query, timestamp[1]);
               reported_timestamp = true;
            }
            ++failures;
         } else {
            uint64_t const value = timestamp[0] & timestamp_mask;
            if (value == 0) {
               if (!reported_timestamp) {
                  std::fprintf(stderr, "generation %u query %u: timestamp is zero\n", generation,
                               query);
                  reported_timestamp = true;
               }
               ++failures;
            } else {
               /* Compare only when the counter has not wrapped within its valid bits, which it can
                * legitimately do between two timestamps.
                */
               if (previous_timestamp_valid && value < previous_timestamp &&
                   previous_timestamp - value < (timestamp_mask >> 1)) {
                  if (!reported_timestamp) {
                     std::fprintf(stderr,
                                  "generation %u query %u: timestamp %" PRIu64
                                  " goes backwards from %" PRIu64 "\n",
                                  generation, query, value, previous_timestamp);
                     reported_timestamp = true;
                  }
                  ++failures;
               }
               previous_timestamp = value;
               previous_timestamp_valid = true;
            }
         }
      }
   }

   std::printf("query_sync generations=%u queries_per_pool=%u failures=%u/%u %s\n", kGenerations,
               kQueriesPerPool, failures, kGenerations * kQueriesPerPool * 4,
               failures == 0 ? "PASS" : "FAIL");

   vkDeviceWaitIdle(device);
   vkDestroyFence(device, fence, nullptr);
   vkDestroyCommandPool(device, command_pool, nullptr);
   vkDestroyPipeline(device, pipeline, nullptr);
   vkDestroyShaderModule(device, vertex_module, nullptr);
   vkDestroyShaderModule(device, fragment_module, nullptr);
   vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
   vkDestroyFramebuffer(device, framebuffer, nullptr);
   vkDestroyRenderPass(device, render_pass, nullptr);
   vkDestroyImageView(device, target_view, nullptr);
   vkDestroyImage(device, target, nullptr);
   vkFreeMemory(device, target_memory, nullptr);
   for (HostBuffer const * results :
        {&occlusion_results, &statistics_results, &statistics32_results, &timestamp_results}) {
      vkUnmapMemory(device, results->memory);
      vkDestroyBuffer(device, results->buffer, nullptr);
      vkFreeMemory(device, results->memory, nullptr);
   }
   vkDestroyQueryPool(device, occlusion_pool, nullptr);
   vkDestroyQueryPool(device, statistics_pool, nullptr);
   vkDestroyQueryPool(device, timestamp_pool, nullptr);
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return failures == 0 ? 0 : 1;
}
