#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#define VK_CHECK(expression)                                                                       \
   do {                                                                                            \
      VkResult const check_result = (expression);                                                  \
      if (check_result != VK_SUCCESS) {                                                            \
         std::fprintf(stderr, "%s failed with VkResult %d\n", #expression, check_result);          \
         return 1;                                                                                 \
      }                                                                                            \
   } while (false)

static uint32_t
find_memory_type(VkPhysicalDevice physical_device, uint32_t bits, VkMemoryPropertyFlags flags)
{
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
   for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) &&
          (properties.memoryTypes[i].propertyFlags & flags) == flags) {
         return i;
      }
   }
   return UINT32_MAX;
}

static std::vector<uint32_t>
read_spirv(char const *path)
{
   std::ifstream file(path, std::ios::binary | std::ios::ate);
   if (!file)
      return {};
   std::streamsize const byte_count = file.tellg();
   if (byte_count <= 0 || byte_count % sizeof(uint32_t))
      return {};
   file.seekg(0);
   std::vector<uint32_t> words(static_cast<size_t>(byte_count) / sizeof(uint32_t));
   file.read(reinterpret_cast<char *>(words.data()), byte_count);
   return file ? words : std::vector<uint32_t>{};
}

struct Buffer {
   VkBuffer buffer = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   uint8_t *mapping = nullptr;
};

static VkResult
create_host_buffer(VkDevice device, VkPhysicalDevice physical_device, VkDeviceSize size,
                   VkBufferUsageFlags usage, Buffer &out)
{
   VkBufferCreateInfo const info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkResult result = vkCreateBuffer(device, &info, nullptr, &out.buffer);
   if (result != VK_SUCCESS)
      return result;
   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
   uint32_t const memory_type =
      find_memory_type(physical_device, requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (memory_type == UINT32_MAX)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   VkMemoryAllocateInfo const allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   result = vkAllocateMemory(device, &allocation, nullptr, &out.memory);
   if (result == VK_SUCCESS)
      result = vkBindBufferMemory(device, out.buffer, out.memory, 0);
   if (result == VK_SUCCESS)
      result = vkMapMemory(device, out.memory, 0, size, 0,
                           reinterpret_cast<void **>(&out.mapping));
   return result;
}

int
main(int argc, char **argv)
{
   bool const force_zero_first_instance =
      argc == 5 && std::strcmp(argv[4], "--force-zero-first-instance") == 0;
   bool const rgba16f = argc == 5 && std::strcmp(argv[4], "--rgba16f") == 0;
   bool const d24s8 = argc == 5 && std::strcmp(argv[4], "--d24s8") == 0;
   if (argc < 4 || argc > 5 ||
       (argc == 5 && !force_zero_first_instance && !rgba16f && !d24s8)) {
      std::fprintf(stderr,
                   "usage: %s COLOR_VERT_SPV DEPTH_VERT_SPV FRAG_SPV "
                   "[--force-zero-first-instance|--rgba16f|--d24s8]\n",
                   argv[0]);
      return 2;
   }
   std::vector<uint32_t> const color_vertex_spirv = read_spirv(argv[1]);
   std::vector<uint32_t> const depth_vertex_spirv = read_spirv(argv[2]);
   std::vector<uint32_t> const fragment_spirv = read_spirv(argv[3]);
   if (color_vertex_spirv.empty() || depth_vertex_spirv.empty() || fragment_spirv.empty()) {
      std::fprintf(stderr, "Unable to read shader SPIR-V\n");
      return 2;
   }

   VkApplicationInfo const application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "terakan-first-instance-dynamic-ssbo-test",
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
      if (!std::strstr(properties.deviceName, "Terakan"))
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
      return 1;
   }
   std::fprintf(stderr, "device=%s queue_family=%u\n", properties.deviceName, queue_family);

   float const priority = 1.0F;
   VkDeviceQueueCreateInfo const queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = queue_family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkDeviceCreateInfo const device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   VkDevice device;
   VK_CHECK(vkCreateDevice(physical_device, &device_info, nullptr, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, queue_family, 0, &queue);

   constexpr uint32_t target_width = 1024;
   constexpr uint32_t target_height = 1024;
   constexpr VkOffset2D sample_positions[] = {
      {0, 0},       {63, 127},   {255, 63},   {511, 255},
      {1023, 511},  {767, 767},  {31, 1000},  {1000, 31},
      {0, 1023},    {127, 900},  {300, 700},  {512, 512},
      {1023, 1023}, {900, 127},  {64, 600},   {600, 64},
   };

   VkDeviceSize const record_size = 64;
   VkDeviceSize const dynamic_alignment =
      std::max<VkDeviceSize>(record_size, properties.limits.minStorageBufferOffsetAlignment);
   VkDeviceSize const descriptor_offset = 0;
   /* Unity commonly suballocates per-draw data far into a large dynamic buffer. Keep this large
    * enough to catch address truncation that a one-alignment-unit offset would miss. */
   VkDeviceSize const dynamic_offset = 720896;
   VkDeviceSize const records_offset = descriptor_offset + dynamic_offset;
   VkDeviceSize const storage_range = 720896;
   VkDeviceSize const uniform_descriptor_offset = descriptor_offset + dynamic_offset + storage_range;
   VkDeviceSize const uniform_dynamic_offset = dynamic_alignment;
   VkDeviceSize const uniform_data_offset = uniform_descriptor_offset + uniform_dynamic_offset;
   VkDeviceSize const storage_size = uniform_data_offset + dynamic_alignment;
   Buffer storage;
   VK_CHECK(create_host_buffer(device, physical_device, storage_size,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               storage));
   std::memset(storage.mapping, 0xCD, storage_size);
   std::array<std::array<float, 4>, 4> const colors = {{
      {{1.0F, 0.0F, 0.0F, 1.0F}},
      {{0.0F, 1.0F, 0.0F, 1.0F}},
      {{0.0F, 0.0F, 1.0F, 1.0F}},
      {{1.0F, 1.0F, 0.0F, 1.0F}},
   }};
   for (uint32_t i = 0; i < colors.size(); ++i)
      std::memcpy(storage.mapping + records_offset + i * record_size, colors[i].data(),
                  sizeof(colors[i]));
   std::array<float, 4> const point_position = {{0.0F, 0.0F, 0.1234567F, 1.0F}};
   std::memset(storage.mapping + uniform_data_offset, 0, sizeof(point_position));
   Buffer uniform_upload;
   VK_CHECK(create_host_buffer(device, physical_device, sizeof(point_position),
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT, uniform_upload));
   std::memcpy(uniform_upload.mapping, point_position.data(), sizeof(point_position));

   Buffer readback;
   VkDeviceSize const readback_pixel_size = rgba16f ? sizeof(uint64_t) : sizeof(uint32_t);
   VK_CHECK(create_host_buffer(device, physical_device, 16 * readback_pixel_size,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback));
   std::memset(readback.mapping, 0xA5, 16 * readback_pixel_size);

   VkDeviceSize const index_buffer_offset = 16;
   Buffer index_buffer;
   VK_CHECK(create_host_buffer(device, physical_device,
                               index_buffer_offset + 4 * sizeof(uint16_t),
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer));
   std::memset(index_buffer.mapping, 0xA5,
               index_buffer_offset + 4 * sizeof(uint16_t));
   uint16_t const indices[] = {0, 1, 2, 3};
   std::memcpy(index_buffer.mapping + index_buffer_offset, indices, sizeof(indices));

   Buffer vertex_buffers[2];
   VK_CHECK(create_host_buffer(device, physical_device, 4 * 4 * sizeof(uint16_t),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffers[0]));
   VK_CHECK(create_host_buffer(device, physical_device, 4 * 2 * sizeof(uint16_t),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffers[1]));
   uint16_t * const packed_positions =
      reinterpret_cast<uint16_t *>(vertex_buffers[0].mapping);
   uint16_t * const packed_uvs = reinterpret_cast<uint16_t *>(vertex_buffers[1].mapping);
   for (uint32_t i = 0; i < 4; ++i) {
      packed_positions[4 * i + 0] = 32768;
      packed_positions[4 * i + 1] = 0xA5A5;
      packed_positions[4 * i + 2] = 0x5A5A;
      packed_positions[4 * i + 3] = UINT16_MAX;
      packed_uvs[2 * i + 0] = 32768;
      packed_uvs[2 * i + 1] = 0xA5A5;
   }

   VkImage image;
   VkImageCreateInfo const image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = rgba16f ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {target_width, target_height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VK_CHECK(vkCreateImage(device, &image_info, nullptr, &image));
   VkMemoryRequirements image_requirements;
   vkGetImageMemoryRequirements(device, image, &image_requirements);
   uint32_t const image_memory_type =
      find_memory_type(physical_device, image_requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (image_memory_type == UINT32_MAX) {
      std::fprintf(stderr, "No device-local image memory\n");
      return 1;
   }
   VkMemoryAllocateInfo const image_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = image_requirements.size,
      .memoryTypeIndex = image_memory_type,
   };
   VkDeviceMemory image_memory;
   VK_CHECK(vkAllocateMemory(device, &image_allocation, nullptr, &image_memory));
   VK_CHECK(vkBindImageMemory(device, image, image_memory, 0));

   VkImageView image_view;
   VkImageViewCreateInfo const view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = image_info.format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   VK_CHECK(vkCreateImageView(device, &view_info, nullptr, &image_view));

   VkImage depth_image;
   VkFormat const depth_format =
      d24s8 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
   VkImageCreateInfo depth_image_info = image_info;
   depth_image_info.format = depth_format;
   depth_image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
   VK_CHECK(vkCreateImage(device, &depth_image_info, nullptr, &depth_image));
   VkMemoryRequirements depth_requirements;
   vkGetImageMemoryRequirements(device, depth_image, &depth_requirements);
   VkMemoryAllocateInfo const depth_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = depth_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device, depth_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
   };
   VkDeviceMemory depth_memory;
   VK_CHECK(vkAllocateMemory(device, &depth_allocation, nullptr, &depth_memory));
   VK_CHECK(vkBindImageMemory(device, depth_image, depth_memory, 0));
   VkImageViewCreateInfo depth_view_info = view_info;
   depth_view_info.image = depth_image;
   depth_view_info.format = depth_format;
   depth_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
   VkImageView depth_view;
   VK_CHECK(vkCreateImageView(device, &depth_view_info, nullptr, &depth_view));

   VkImage intervening_depth_image;
   VK_CHECK(vkCreateImage(device, &depth_image_info, nullptr, &intervening_depth_image));
   VkMemoryRequirements intervening_depth_requirements;
   vkGetImageMemoryRequirements(device, intervening_depth_image, &intervening_depth_requirements);
   VkMemoryAllocateInfo const intervening_depth_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = intervening_depth_requirements.size,
      .memoryTypeIndex = find_memory_type(physical_device,
                                          intervening_depth_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
   };
   VkDeviceMemory intervening_depth_memory;
   VK_CHECK(vkAllocateMemory(device, &intervening_depth_allocation, nullptr,
                             &intervening_depth_memory));
   VK_CHECK(vkBindImageMemory(device, intervening_depth_image, intervening_depth_memory, 0));
   VkImageViewCreateInfo intervening_depth_view_info = depth_view_info;
   intervening_depth_view_info.image = intervening_depth_image;
   VkImageView intervening_depth_view;
   VK_CHECK(vkCreateImageView(device, &intervening_depth_view_info, nullptr,
                              &intervening_depth_view));

   VkAttachmentDescription const attachments[] = {
      {.format = image_info.format, .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {.format = depth_format, .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
   };
   VkAttachmentReference const color_reference = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   };
   VkAttachmentReference const depth_reference = {
      .attachment = 1,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
   };
   VkSubpassDescription const subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_reference,
      .pDepthStencilAttachment = &depth_reference,
   };
   VkAttachmentDescription const prepass_attachment = attachments[1];
   VkAttachmentReference const prepass_depth_reference = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
   };
   VkSubpassDescription const prepass_subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .pDepthStencilAttachment = &prepass_depth_reference,
   };
   VkRenderPassCreateInfo const prepass_render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &prepass_attachment,
      .subpassCount = 1,
      .pSubpasses = &prepass_subpass,
   };
   VkRenderPass prepass_render_pass;
   VK_CHECK(vkCreateRenderPass(device, &prepass_render_pass_info, nullptr,
                               &prepass_render_pass));
   VkAttachmentDescription color_attachments[] = {attachments[0], attachments[1]};
   color_attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
   color_attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   color_attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   color_attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
   color_attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
   VkRenderPassCreateInfo color_render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 2,
      .pAttachments = color_attachments,
      .subpassCount = 1,
      .pSubpasses = &subpass,
   };
   VkRenderPass render_pass;
   VK_CHECK(vkCreateRenderPass(device, &color_render_pass_info, nullptr, &render_pass));
   VkImageView const framebuffer_attachments[] = {image_view, depth_view};
   VkFramebufferCreateInfo const framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 2,
      .pAttachments = framebuffer_attachments,
      .width = target_width,
      .height = target_height,
      .layers = 1,
   };
   VkFramebuffer framebuffer;
   VK_CHECK(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer));
   VkFramebufferCreateInfo prepass_framebuffer_info = framebuffer_info;
   prepass_framebuffer_info.renderPass = prepass_render_pass;
   prepass_framebuffer_info.attachmentCount = 1;
   prepass_framebuffer_info.pAttachments = &depth_view;
   VkFramebuffer prepass_framebuffer;
   VK_CHECK(vkCreateFramebuffer(device, &prepass_framebuffer_info, nullptr,
                                &prepass_framebuffer));
   prepass_framebuffer_info.pAttachments = &intervening_depth_view;
   VkFramebuffer intervening_depth_framebuffer;
   VK_CHECK(vkCreateFramebuffer(device, &prepass_framebuffer_info, nullptr,
                                &intervening_depth_framebuffer));

   VkDescriptorSetLayoutBinding const descriptor_bindings[] = {
      {
         .binding = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
      },
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
      },
   };
   VkDescriptorSetLayoutCreateInfo const descriptor_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = descriptor_bindings,
   };
   VkDescriptorSetLayout descriptor_layout;
   VK_CHECK(vkCreateDescriptorSetLayout(device, &descriptor_layout_info, nullptr,
                                         &descriptor_layout));
   VkDescriptorSetLayoutCreateInfo const empty_descriptor_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
   };
   VkDescriptorSetLayout empty_descriptor_layout;
   VK_CHECK(vkCreateDescriptorSetLayout(device, &empty_descriptor_layout_info, nullptr,
                                         &empty_descriptor_layout));
   VkPushConstantRange const push_constant_range = {
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
      .offset = 0,
      .size = sizeof(uint32_t),
   };
   VkDescriptorSetLayout const pipeline_descriptor_layouts[] = {
      empty_descriptor_layout, descriptor_layout};
   VkPipelineLayoutCreateInfo const pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 2,
      .pSetLayouts = pipeline_descriptor_layouts,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_constant_range,
   };
   VkPipelineLayout pipeline_layout;
   VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout));

   VkDescriptorPoolSize const pool_sizes[] = {
      {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .descriptorCount = 1},
      {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1},
   };
   VkDescriptorPoolCreateInfo const descriptor_pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 2,
      .poolSizeCount = 2,
      .pPoolSizes = pool_sizes,
   };
   VkDescriptorPool descriptor_pool;
   VK_CHECK(vkCreateDescriptorPool(device, &descriptor_pool_info, nullptr, &descriptor_pool));
   VkDescriptorSetLayout const allocated_descriptor_layouts[] = {
      empty_descriptor_layout, descriptor_layout};
   VkDescriptorSetAllocateInfo const descriptor_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptor_pool,
      .descriptorSetCount = 2,
      .pSetLayouts = allocated_descriptor_layouts,
   };
   VkDescriptorSet descriptor_sets[2];
   VK_CHECK(vkAllocateDescriptorSets(device, &descriptor_allocate_info, descriptor_sets));
   VkDescriptorBufferInfo const buffer_infos[] = {{
      .buffer = storage.buffer,
      .offset = descriptor_offset,
      .range = storage_range,
   }, {
      .buffer = storage.buffer,
      .offset = uniform_data_offset,
      .range = sizeof(point_position),
   }};
   VkWriteDescriptorSet const descriptor_writes[] = {
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_sets[1],
         .dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
         .pBufferInfo = &buffer_infos[0],
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = descriptor_sets[1],
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .pBufferInfo = &buffer_infos[1],
      },
   };
   vkUpdateDescriptorSets(device, 2, descriptor_writes, 0, nullptr);

   VkShaderModule modules[3];
   std::array<std::vector<uint32_t> const *, 3> const shader_code = {
      &color_vertex_spirv, &depth_vertex_spirv, &fragment_spirv};
   for (uint32_t i = 0; i < 3; ++i) {
      VkShaderModuleCreateInfo const module_info = {
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = shader_code[i]->size() * sizeof(uint32_t),
         .pCode = shader_code[i]->data(),
      };
      VK_CHECK(vkCreateShaderModule(device, &module_info, nullptr, &modules[i]));
   }
   VkPipelineShaderStageCreateInfo const stages[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = modules[0],
         .pName = "main",
      },
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = modules[2],
         .pName = "main",
      },
   };
   VkPipelineShaderStageCreateInfo const depth_stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = modules[1],
      .pName = "main",
   };
   VkVertexInputBindingDescription const vertex_bindings[] = {
      {.binding = 0, .stride = 4 * sizeof(uint16_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
      {.binding = 1, .stride = 2 * sizeof(uint16_t), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
   };
   VkVertexInputAttributeDescription const vertex_attributes[] = {
      {.location = 0, .binding = 0, .format = VK_FORMAT_R16G16B16A16_UNORM, .offset = 0},
      {.location = 4, .binding = 1, .format = VK_FORMAT_R16G16_UNORM, .offset = 0},
   };
   VkPipelineVertexInputStateCreateInfo const vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 2,
      .pVertexBindingDescriptions = vertex_bindings,
      .vertexAttributeDescriptionCount = 2,
      .pVertexAttributeDescriptions = vertex_attributes,
   };
   VkPipelineInputAssemblyStateCreateInfo const input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
   };
   VkPipelineViewportStateCreateInfo const viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1,
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
   VkPipelineDepthStencilStateCreateInfo const depth_equal = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_FALSE,
      .depthCompareOp = VK_COMPARE_OP_EQUAL,
   };
   VkPipelineDepthStencilStateCreateInfo const depth_write = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_ALWAYS,
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
   VkDynamicState const dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo const dynamic_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dynamic_states,
   };
   VkGraphicsPipelineCreateInfo const pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterization,
      .pMultisampleState = &multisample,
      .pDepthStencilState = &depth_equal,
      .pColorBlendState = &color_blend,
      .pDynamicState = &dynamic_state,
      .layout = pipeline_layout,
      .renderPass = render_pass,
   };
   VkPipeline pipeline;
   VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                      &pipeline));
   VkPipelineColorBlendStateCreateInfo prepass_color_blend = color_blend;
   prepass_color_blend.attachmentCount = 0;
   prepass_color_blend.pAttachments = nullptr;
   VkGraphicsPipelineCreateInfo prepass_pipeline_info = pipeline_info;
   prepass_pipeline_info.stageCount = 1;
   prepass_pipeline_info.pStages = &depth_stage;
   prepass_pipeline_info.renderPass = prepass_render_pass;
   prepass_pipeline_info.pDepthStencilState = &depth_write;
   prepass_pipeline_info.pColorBlendState = &prepass_color_blend;
   VkPipeline prepass_pipeline;
   VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &prepass_pipeline_info, nullptr,
                                      &prepass_pipeline));
   VkPipelineInputAssemblyStateCreateInfo triangle_input_assembly = input_assembly;
   triangle_input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   VkGraphicsPipelineCreateInfo triangle_pipeline_info = pipeline_info;
   triangle_pipeline_info.pInputAssemblyState = &triangle_input_assembly;
   VkPipeline triangle_pipeline;
   VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &triangle_pipeline_info, nullptr,
                                      &triangle_pipeline));
   VkGraphicsPipelineCreateInfo triangle_prepass_pipeline_info = prepass_pipeline_info;
   triangle_prepass_pipeline_info.pInputAssemblyState = &triangle_input_assembly;
   VkPipeline triangle_prepass_pipeline;
   VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &triangle_prepass_pipeline_info,
                                      nullptr, &triangle_prepass_pipeline));

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
   VkBufferCopy const uniform_copy = {
      .srcOffset = 0,
      .dstOffset = uniform_data_offset,
      .size = sizeof(point_position),
   };
   vkCmdCopyBuffer(command_buffer, uniform_upload.buffer, storage.buffer, 1, &uniform_copy);
   VkBufferMemoryBarrier const uniform_ready = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = storage.buffer,
      .offset = uniform_data_offset,
      .size = sizeof(point_position),
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0, nullptr, 1,
                        &uniform_ready, 0, nullptr);
   VkClearValue const clears[] = {
      {.color = {.uint32 = {0, 0, 0, 0}}},
      {.depthStencil = {.depth = 1.0F, .stencil = 0}},
   };
   VkRenderPassBeginInfo const prepass_begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = prepass_render_pass,
      .framebuffer = prepass_framebuffer,
      .renderArea = {{0, 0}, {target_width, target_height}},
      .clearValueCount = 1,
      .pClearValues = &clears[1],
   };
   vkCmdBeginRenderPass(command_buffer, &prepass_begin, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, prepass_pipeline);
   VkBuffer const vertex_buffer_handles[] = {vertex_buffers[0].buffer, vertex_buffers[1].buffer};
   VkDeviceSize const vertex_buffer_offsets[] = {0, 0};
   vkCmdBindVertexBuffers(command_buffer, 0, 2, vertex_buffer_handles, vertex_buffer_offsets);
   uint32_t const dynamic_offsets[] = {static_cast<uint32_t>(dynamic_offset)};
   vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 2,
                           descriptor_sets, 1, dynamic_offsets);
   uint32_t object_base = 1;
   vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                      sizeof(object_base), &object_base);
   for (uint32_t i = 0; i < 8; ++i) {
      VkOffset2D const position = sample_positions[i];
      VkViewport const viewport = {static_cast<float>(position.x),
                                   static_cast<float>(position.y), 1.0F, 1.0F, 0.0F, 1.0F};
      VkRect2D const scissor = {position, {1, 1}};
      vkCmdSetViewport(command_buffer, 0, 1, &viewport);
      vkCmdSetScissor(command_buffer, 0, 1, &scissor);
      vkCmdDraw(command_buffer, 1, 1, 0, 0);
   }
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                     triangle_prepass_pipeline);
   vkCmdBindIndexBuffer(command_buffer, index_buffer.buffer, index_buffer_offset,
                        VK_INDEX_TYPE_UINT16);
   object_base = 0x80000000u;
   vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                      sizeof(object_base), &object_base);
   for (uint32_t i = 0; i < 8; ++i) {
      VkOffset2D const position = sample_positions[8 + i];
      VkViewport const viewport = {static_cast<float>(position.x),
                                   static_cast<float>(position.y), 1.0F, 1.0F, 0.0F, 1.0F};
      VkRect2D const scissor = {position, {1, 1}};
      vkCmdSetViewport(command_buffer, 0, 1, &viewport);
      vkCmdSetScissor(command_buffer, 0, 1, &scissor);
      if (i < 4)
         vkCmdDraw(command_buffer, 3, 1, 0, i);
      else
         vkCmdDrawIndexed(command_buffer, 3, 1, 0, 0, i);
   }
   vkCmdEndRenderPass(command_buffer);
   VkRenderPassBeginInfo intervening_depth_begin = prepass_begin;
   intervening_depth_begin.framebuffer = intervening_depth_framebuffer;
   vkCmdBeginRenderPass(command_buffer, &intervening_depth_begin, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                     triangle_prepass_pipeline);
   VkViewport const intervening_viewport = {
      0.0F, 0.0F, static_cast<float>(target_width), static_cast<float>(target_height), 0.0F, 1.0F};
   VkRect2D const intervening_scissor = {{0, 0}, {target_width, target_height}};
   vkCmdSetViewport(command_buffer, 0, 1, &intervening_viewport);
   vkCmdSetScissor(command_buffer, 0, 1, &intervening_scissor);
   vkCmdDraw(command_buffer, 3, 1, 0, 0);
   vkCmdEndRenderPass(command_buffer);
   VkImageMemoryBarrier const depth_reuse_barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = depth_image,
      .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                           0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(command_buffer,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &depth_reuse_barrier);
   VkBufferMemoryBarrier uniform_rewrite = uniform_ready;
   uniform_rewrite.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
   uniform_rewrite.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                        &uniform_rewrite, 0, nullptr);
   vkCmdCopyBuffer(command_buffer, uniform_upload.buffer, storage.buffer, 1, &uniform_copy);
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0, nullptr, 1,
                        &uniform_ready, 0, nullptr);
   VkRenderPassBeginInfo color_begin = prepass_begin;
   color_begin.renderPass = render_pass;
   color_begin.framebuffer = framebuffer;
   color_begin.clearValueCount = 2;
   color_begin.pClearValues = clears;
   vkCmdBeginRenderPass(command_buffer, &color_begin, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   object_base = 1;
   vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                      sizeof(object_base), &object_base);
   for (uint32_t i = 0; i < 4; ++i) {
      VkOffset2D const position = sample_positions[i];
      VkViewport const viewport = {
         .x = static_cast<float>(position.x),
         .y = static_cast<float>(position.y),
         .width = 1.0F,
         .height = 1.0F,
         .minDepth = 0.0F,
         .maxDepth = 1.0F,
      };
      VkRect2D const scissor = {position, {1, 1}};
      vkCmdSetViewport(command_buffer, 0, 1, &viewport);
      vkCmdSetScissor(command_buffer, 0, 1, &scissor);
      vkCmdDraw(command_buffer, 1, 1, 0, force_zero_first_instance ? 0 : i);
   }
   vkCmdBindIndexBuffer(command_buffer, index_buffer.buffer, index_buffer_offset,
                        VK_INDEX_TYPE_UINT16);
   object_base = 0;
   vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                      sizeof(object_base), &object_base);
   for (uint32_t i = 0; i < 4; ++i) {
      VkOffset2D const position = sample_positions[4 + i];
      VkViewport const viewport = {
         .x = static_cast<float>(position.x),
         .y = static_cast<float>(position.y),
         .width = 1.0F,
         .height = 1.0F,
         .minDepth = 0.0F,
         .maxDepth = 1.0F,
      };
      VkRect2D const scissor = {position, {1, 1}};
      vkCmdSetViewport(command_buffer, 0, 1, &viewport);
      vkCmdSetScissor(command_buffer, 0, 1, &scissor);
      vkCmdDrawIndexed(command_buffer, 1, 1, i, 0, 0);
   }
   vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, triangle_pipeline);
   object_base = 0x80000000u;
   vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                      sizeof(object_base), &object_base);
   for (uint32_t i = 0; i < 8; ++i) {
      VkOffset2D const position = sample_positions[8 + i];
      VkViewport const viewport = {static_cast<float>(position.x),
                                   static_cast<float>(position.y), 1.0F, 1.0F, 0.0F, 1.0F};
      VkRect2D const scissor = {position, {1, 1}};
      vkCmdSetViewport(command_buffer, 0, 1, &viewport);
      vkCmdSetScissor(command_buffer, 0, 1, &scissor);
      if (i < 4)
         vkCmdDraw(command_buffer, 3, 1, 0, force_zero_first_instance ? 0 : i);
      else
         vkCmdDrawIndexed(command_buffer, 3, 1, 0, 0,
                          force_zero_first_instance ? 0 : i);
   }
   vkCmdEndRenderPass(command_buffer);
   VkImageMemoryBarrier const image_ready = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                        &image_ready);
   VkBufferImageCopy copies[16];
   for (uint32_t i = 0; i < 16; ++i) {
      copies[i] = {
         .bufferOffset = i * readback_pixel_size,
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageOffset = {sample_positions[i].x, sample_positions[i].y, 0},
         .imageExtent = {1, 1, 1},
      };
   }
   vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          readback.buffer, 16, copies);
   VkMemoryBarrier const host_ready = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_ready, 0, nullptr, 0, nullptr);
   VK_CHECK(vkEndCommandBuffer(command_buffer));
   VkSubmitInfo const submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE));
   VK_CHECK(vkQueueWaitIdle(queue));

   uint32_t const expected[] = {
      0xFF00FF00u, 0xFFFF0000u, 0xFF00FFFFu, 0xFF0000FFu,
      0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFF00FFFFu,
      0xFF172EB9u, 0xFF172EB9u, 0xFF172EB9u, 0xFF172EB9u,
      0xFF172EB9u, 0xFF172EB9u, 0xFF172EB9u, 0xFF172EB9u,
   };
   bool passed = true;
   if (rgba16f) {
      uint64_t const expected_rgba16f[] = {
         0x3c0000003c000000ull, 0x3c003c0000000000ull,
         0x3c0000003c003c00ull, 0x3c00000000003c00ull,
         0x3c00000000003c00ull, 0x3c0000003c000000ull,
         0x3c003c0000000000ull, 0x3c0000003c003c00ull,
      };
      uint64_t const *actual = reinterpret_cast<uint64_t const *>(readback.mapping);
      for (uint32_t i = 0; i < 8; ++i) {
         std::printf("pixel[%u]=0x%016llx expected=0x%016llx %s\n", i,
                     static_cast<unsigned long long>(actual[i]),
                     static_cast<unsigned long long>(expected_rgba16f[i]),
                     actual[i] == expected_rgba16f[i] ? "PASS" : "FAIL");
         passed &= actual[i] == expected_rgba16f[i];
      }
   } else {
      uint32_t const *actual = reinterpret_cast<uint32_t const *>(readback.mapping);
      for (uint32_t i = 0; i < 16; ++i) {
         std::printf("pixel[%u]=0x%08x expected=0x%08x %s\n", i, actual[i], expected[i],
                     actual[i] == expected[i] ? "PASS" : "FAIL");
         passed &= actual[i] == expected[i];
      }
   }
   std::printf("descriptor_offset=%llu dynamic_offset=%llu uniform_dynamic_offset=%llu "
               "record_stride=%llu\n",
               static_cast<unsigned long long>(descriptor_offset),
               static_cast<unsigned long long>(dynamic_offset),
               static_cast<unsigned long long>(uniform_dynamic_offset),
               static_cast<unsigned long long>(record_size));

   vkDeviceWaitIdle(device);
   vkDestroyCommandPool(device, command_pool, nullptr);
   vkDestroyPipeline(device, pipeline, nullptr);
   vkDestroyPipeline(device, prepass_pipeline, nullptr);
   vkDestroyPipeline(device, triangle_pipeline, nullptr);
   vkDestroyPipeline(device, triangle_prepass_pipeline, nullptr);
   vkDestroyShaderModule(device, modules[0], nullptr);
   vkDestroyShaderModule(device, modules[1], nullptr);
   vkDestroyShaderModule(device, modules[2], nullptr);
   vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
   vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
   vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
   vkDestroyDescriptorSetLayout(device, empty_descriptor_layout, nullptr);
   vkDestroyFramebuffer(device, framebuffer, nullptr);
   vkDestroyFramebuffer(device, prepass_framebuffer, nullptr);
   vkDestroyFramebuffer(device, intervening_depth_framebuffer, nullptr);
   vkDestroyRenderPass(device, render_pass, nullptr);
   vkDestroyRenderPass(device, prepass_render_pass, nullptr);
   vkDestroyImageView(device, image_view, nullptr);
   vkDestroyImageView(device, depth_view, nullptr);
   vkDestroyImageView(device, intervening_depth_view, nullptr);
   vkDestroyImage(device, image, nullptr);
   vkFreeMemory(device, image_memory, nullptr);
   vkDestroyImage(device, depth_image, nullptr);
   vkFreeMemory(device, depth_memory, nullptr);
   vkDestroyImage(device, intervening_depth_image, nullptr);
   vkFreeMemory(device, intervening_depth_memory, nullptr);
   vkDestroyBuffer(device, storage.buffer, nullptr);
   vkUnmapMemory(device, storage.memory);
   vkFreeMemory(device, storage.memory, nullptr);
   vkDestroyBuffer(device, uniform_upload.buffer, nullptr);
   vkUnmapMemory(device, uniform_upload.memory);
   vkFreeMemory(device, uniform_upload.memory, nullptr);
   vkDestroyBuffer(device, readback.buffer, nullptr);
   vkUnmapMemory(device, readback.memory);
   vkFreeMemory(device, readback.memory, nullptr);
   vkDestroyBuffer(device, index_buffer.buffer, nullptr);
   vkUnmapMemory(device, index_buffer.memory);
   vkFreeMemory(device, index_buffer.memory, nullptr);
   for (Buffer const &vertex_buffer : vertex_buffers) {
      vkDestroyBuffer(device, vertex_buffer.buffer, nullptr);
      vkUnmapMemory(device, vertex_buffer.memory);
      vkFreeMemory(device, vertex_buffer.memory, nullptr);
   }
   vkDestroyDevice(device, nullptr);
   vkDestroyInstance(instance, nullptr);
   return passed ? 0 : 1;
}
