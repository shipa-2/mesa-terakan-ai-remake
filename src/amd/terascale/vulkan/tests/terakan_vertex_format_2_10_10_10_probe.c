/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* A characterization tool, not a pass/fail test, which is why it is built but not run by
 * bin/terakan-test.
 *
 * dEQP-VK.pipeline.*.vertex_input fails on a2r10g10b10_sscaled_pack32 and on nothing else in the
 * packed 2_10_10_10 family: the unorm, snorm and uscaled members of the same format all pass. That
 * narrows the fault to signed-and-scaled, but says nothing about which of the four components is
 * wrong or by how much, and the driver has an unimplemented `TODO(Triang3l): Signed 2_10_10_10 and
 * 10_10_10_2 alpha fixup on certain chips` that may or may not be the same thing -- Gallium r600
 * carries such a fixup, but applies it only to the NORM number format, which passes here.
 *
 * So this fetches one 2_10_10_10 attribute per draw and prints what came back next to what the
 * Vulkan specification says it should be, for the four number types the shader can read as a
 * float, over packed words chosen to cover both signs of every field: the two-bit alpha at each of
 * its four values, and the ten-bit components at zero, one, the largest positive value, the sign
 * boundary and all ones.
 *
 * Read the table by comparing the number types against each other rather than in isolation. A
 * component that is right under uscaled and wrong under sscaled is a sign-extension fault; one
 * that is wrong under both is a field-position fault; alpha wrong on its own is the fixup the TODO
 * names.
 */

#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terakan_test_device.h"
#define CK(e) do { VkResult r=(e); if(r){fprintf(stderr,"%s -> %d\n",#e,r);return 1;} } while(0)
static uint32_t mt(VkPhysicalDevice p,uint32_t bits,VkMemoryPropertyFlags f){
   VkPhysicalDeviceMemoryProperties m; vkGetPhysicalDeviceMemoryProperties(p,&m);
   for(uint32_t i=0;i<m.memoryTypeCount;++i) if((bits&(1u<<i))&&(m.memoryTypes[i].propertyFlags&f)==f) return i;
   return UINT32_MAX; }
static const uint32_t vertex_spirv[] = {
#include "terakan_vertex_format_2_10_10_10.vert.spv.h"
};
static const uint32_t fragment_spirv[] = {
#include "terakan_vertex_format_2_10_10_10.frag.spv.h"

};

/* A2R10G10B10 packs alpha in bits 31:30, then red, green and blue downwards. */
static uint32_t pack(uint32_t a,uint32_t r,uint32_t g,uint32_t b){
   return ((a&3u)<<30)|((r&0x3FFu)<<20)|((g&0x3FFu)<<10)|(b&0x3FFu); }
static int sext10(uint32_t v){ v&=0x3FFu; return v<512u ? (int)v : (int)v-1024; }
static int sext2(uint32_t v){ v&=3u; return v<2u ? (int)v : (int)v-4; }

enum number_type { UNORM, SNORM, USCALED, SSCALED };
static const char *const number_type_names[]={"unorm","snorm","uscaled","sscaled"};
static const VkFormat number_type_formats[]={
   VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_SNORM_PACK32,
   VK_FORMAT_A2R10G10B10_USCALED_PACK32, VK_FORMAT_A2R10G10B10_SSCALED_PACK32};

/* What the specification says the component decodes to. */
static float expect(enum number_type t,uint32_t raw,unsigned bits){
   int const s = bits==2 ? sext2(raw) : sext10(raw);
   unsigned const u = bits==2 ? (raw&3u) : (raw&0x3FFu);
   float const smax = bits==2 ? 1.f : 511.f, umax = bits==2 ? 3.f : 1023.f;
   switch(t){
   case UNORM:   return (float)u/umax;
   case SNORM:   return (float)s/smax < -1.f ? -1.f : (float)s/smax;
   case USCALED: return (float)u;
   default:      return (float)s;
   }
}

int main(void){
   VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
   VkInstance inst; CK(vkCreateInstance(&ici,NULL,&inst));
   uint32_t n=8; VkPhysicalDevice pds[8]; CK(vkEnumeratePhysicalDevices(inst,&n,pds));
   VkPhysicalDevice pd=VK_NULL_HANDLE; VkPhysicalDeviceProperties pr;
   for(uint32_t i=0;i<n;++i){ vkGetPhysicalDeviceProperties(pds[i],&pr);
      if(terakan_test_device_matches(pr.deviceName)){pd=pds[i];break;} }
   if(!pd){fprintf(stderr,"no device\n");return 1;}
   fprintf(stderr,"device=%s\n",pr.deviceName);
   float pri=1.f; VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&pri};
   VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
   VkDevice dev; CK(vkCreateDevice(pd,&dci,NULL,&dev)); VkQueue q; vkGetDeviceQueue(dev,0,0,&q);

   /* One packed word per instance, selected by firstInstance, so a draw picks its own input. */
   struct input { uint32_t a,r,g,b; };
   struct input const inputs[]={
      {0,0,0,0}, {1,1,1,1}, {0,511,511,511}, {0,512,512,512}, {0,1023,1023,1023},
      {1,0,511,512}, {2,1,512,1023}, {3,1023,1,0},
   };
   unsigned const input_count=sizeof(inputs)/sizeof(*inputs);

   VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=4096,.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
   VkBuffer vb; CK(vkCreateBuffer(dev,&bci,NULL,&vb));
   VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,vb,&mr);
   VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,
     .memoryTypeIndex=mt(pd,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   VkDeviceMemory vmem; CK(vkAllocateMemory(dev,&mai,NULL,&vmem)); CK(vkBindBufferMemory(dev,vb,vmem,0));
   uint32_t*vmap; CK(vkMapMemory(dev,vmem,0,VK_WHOLE_SIZE,0,(void**)&vmap));
   for(unsigned i=0;i<input_count;++i) vmap[i]=pack(inputs[i].a,inputs[i].r,inputs[i].g,inputs[i].b);

   VkImageCreateInfo ici2={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
     .format=VK_FORMAT_R32G32B32A32_SFLOAT,.extent={1,1,1},.mipLevels=1,.arrayLayers=1,
     .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
     .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
     .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage img; CK(vkCreateImage(dev,&ici2,NULL,&img));
   VkMemoryRequirements imr; vkGetImageMemoryRequirements(dev,img,&imr);
   VkMemoryAllocateInfo imai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=imr.size,
     .memoryTypeIndex=mt(pd,imr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
   VkDeviceMemory imem; CK(vkAllocateMemory(dev,&imai,NULL,&imem)); CK(vkBindImageMemory(dev,img,imem,0));
   VkImageViewCreateInfo ivci={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=img,
     .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R32G32B32A32_SFLOAT,
     .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
   VkImageView iv; CK(vkCreateImageView(dev,&ivci,NULL,&iv));
   VkBufferCreateInfo rbci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=16,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   VkBuffer rb; CK(vkCreateBuffer(dev,&rbci,NULL,&rb));
   VkMemoryRequirements rmr; vkGetBufferMemoryRequirements(dev,rb,&rmr);
   VkMemoryAllocateInfo rmai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=rmr.size,
     .memoryTypeIndex=mt(pd,rmr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   VkDeviceMemory rmem; CK(vkAllocateMemory(dev,&rmai,NULL,&rmem)); CK(vkBindBufferMemory(dev,rb,rmem,0));
   float*rmap; CK(vkMapMemory(dev,rmem,0,VK_WHOLE_SIZE,0,(void**)&rmap));

   VkAttachmentDescription att={.format=VK_FORMAT_R32G32B32A32_SFLOAT,.samples=VK_SAMPLE_COUNT_1_BIT,
     .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
     .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
     .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference ar={.attachment=0,.layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&ar};
   VkRenderPassCreateInfo rpci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&att,.subpassCount=1,.pSubpasses=&sp};
   VkRenderPass rp; CK(vkCreateRenderPass(dev,&rpci,NULL,&rp));
   VkFramebufferCreateInfo fbci={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=rp,.attachmentCount=1,.pAttachments=&iv,.width=1,.height=1,.layers=1};
   VkFramebuffer fb; CK(vkCreateFramebuffer(dev,&fbci,NULL,&fb));
   VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev,&plci,NULL,&pl));
   VkShaderModuleCreateInfo vs={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(vertex_spirv),.pCode=vertex_spirv};
   VkShaderModuleCreateInfo fs={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(fragment_spirv),.pCode=fragment_spirv};
   VkShaderModule vm,fm; CK(vkCreateShaderModule(dev,&vs,NULL,&vm)); CK(vkCreateShaderModule(dev,&fs,NULL,&fm));
   VkPipelineShaderStageCreateInfo st[2]={
     {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vm,.pName="main"},
     {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fm,.pName="main"}};
   VkVertexInputBindingDescription vib={.binding=0,.stride=4,.inputRate=VK_VERTEX_INPUT_RATE_INSTANCE};
   VkVertexInputAttributeDescription via={.location=0,.binding=0,.format=number_type_formats[0],.offset=0};
   VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
     .vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&vib,
     .vertexAttributeDescriptionCount=1,.pVertexAttributeDescriptions=&via};
   VkPipelineInputAssemblyStateCreateInfo iasm={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp={0,0,1,1,0,1}; VkRect2D sc={{0,0},{1,1}};
   VkPipelineViewportStateCreateInfo vps={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&vp,.scissorCount=1,.pScissors=&sc};
   VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.lineWidth=1.f};
   VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba={.colorWriteMask=0xF};
   VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&cba};
   VkGraphicsPipelineCreateInfo gpci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=st,
     .pVertexInputState=&vi,.pInputAssemblyState=&iasm,.pViewportState=&vps,.pRasterizationState=&rs,
     .pMultisampleState=&ms,.pColorBlendState=&cb,.layout=pl,.renderPass=rp};
   VkPipeline pipes[4];
   for(unsigned t=0;t<4;++t){ via.format=number_type_formats[t];
      CK(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gpci,NULL,&pipes[t])); }
   VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VkCommandPool cp; CK(vkCreateCommandPool(dev,&cpi,NULL,&cp));
   VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.commandBufferCount=1};
   VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev,&cbai,&cmd));
   VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fen; CK(vkCreateFence(dev,&fci,NULL,&fen));

   printf("a2r10g10b10, one packed word per row. Each cell is r,g,b,a as fetched; a cell that\n"
          "differs from the specification is followed by the value it should have had.\n");
   for(unsigned t=0;t<4;++t){
      printf("\n%s\n",number_type_names[t]);
      for(unsigned i=0;i<input_count;++i){
         rmap[0]=rmap[1]=rmap[2]=rmap[3]=-1e30f;
         CK(vkResetCommandBuffer(cmd,0));
         VkCommandBufferBeginInfo bgi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
         CK(vkBeginCommandBuffer(cmd,&bgi));
         VkClearValue cv={.color={.float32={-1e30f,-1e30f,-1e30f,-1e30f}}};
         VkRenderPassBeginInfo rbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=rp,.framebuffer=fb,
           .renderArea={{0,0},{1,1}},.clearValueCount=1,.pClearValues=&cv};
         vkCmdBeginRenderPass(cmd,&rbi,VK_SUBPASS_CONTENTS_INLINE);
         vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipes[t]);
         VkDeviceSize off=0; vkCmdBindVertexBuffers(cmd,0,1,&vb,&off);
         vkCmdDraw(cmd,3,1,0,i);
         vkCmdEndRenderPass(cmd);
         VkImageMemoryBarrier ib={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
           .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
           .oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
           .image=img,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
         vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&ib);
         VkBufferImageCopy bic={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={1,1,1}};
         vkCmdCopyImageToBuffer(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,rb,1,&bic);
         VkMemoryBarrier hb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
         vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&hb,0,NULL,0,NULL);
         CK(vkEndCommandBuffer(cmd));
         CK(vkResetFences(dev,1,&fen));
         VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
         CK(vkQueueSubmit(q,1,&si,fen)); CK(vkWaitForFences(dev,1,&fen,VK_TRUE,5000000000ull));
         uint32_t const raw[4]={inputs[i].r,inputs[i].g,inputs[i].b,inputs[i].a};
         unsigned const bits[4]={10,10,10,2};
         printf("  a=%u r=%4u g=%4u b=%4u ->",inputs[i].a,inputs[i].r,inputs[i].g,inputs[i].b);
         for(unsigned c=0;c<4;++c){
            float const want=expect((enum number_type)t,raw[c],bits[c]);
            float const got=rmap[c];
            float const d=got-want;
            printf("  %c=%g",("rgba")[c],got);
            if(!(d>-1e-3f&&d<1e-3f)) printf("(want %g)",want);
         }
         printf("\n");
      }
   }
   vkDeviceWaitIdle(dev);
   return 0;
}
