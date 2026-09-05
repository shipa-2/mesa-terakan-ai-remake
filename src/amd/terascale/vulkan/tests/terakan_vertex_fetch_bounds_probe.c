/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* A characterization tool, not a pass/fail test, which is why it is built but not run by
 * bin/terakan-test.
 *
 * It answers the question the TODO in terakan_vertex_input.c asks: what exactly does the hardware
 * bounds-check a vertex fetch against? The answer decides whether the driver has to shrink the
 * fetch SIZE itself, and by how much. An earlier attempt to shrink it by the element size minus
 * four broke dEQP-VK.rasterization.depth_bias_control, and the truncation has been disabled ever
 * since with the rule left unknown.
 *
 * The binding SIZE is set by creating VkBuffers of exactly the wanted size over one allocation, so
 * only core Vulkan 1.0 is needed. One instance-rate attribute is fetched, so all three vertices of
 * the single instance read the same element and the binding can be smaller than one element. The
 * vertex shader passes what it fetched to a 1x1 R32G32B32A32_UINT target, which is read back: the
 * source is filled so that word i reads 0xC0DE00ii, making it obvious which bytes came back, and a
 * rejected fetch reads as zeros.
 *
 * What it measured on Caicos, as the smallest accepted SIZE for a 16-byte 32_32_32_32 attribute at
 * attribute offset O, with the element occupying [O, O+16):
 *
 *    O =  0 -> 16     O =  2 ->  6     O =  4 ->  8     O =  6 -> 10
 *    O =  8 -> 16     O = 12 -> 16     O = 16 -> 32
 *
 * and for smaller elements, 32_32 at O = 0 -> 8 and O = 4 -> 8, 32 at O = 0 -> 4 and O = 4 -> 8.
 *
 * Two conclusions hold firmly. The check is not element-complete: at O = 4 a binding of 8 bytes is
 * accepted and returns bytes [4, 20), twelve bytes past the end of the binding, and the same
 * happens at O = 8, O = 12 and for 32_32 at O = 4. With robustBufferAccess advertised, that is both
 * a specification violation and a real out-of-bounds read. And the check is not "only the first
 * four bytes", the guess the comment in terakan_physical_device.c records: at O = 0 a binding of 4
 * bytes is rejected, and nothing is returned until 16.
 *
 * What the rule actually is remains unexplained. The thresholds are not even monotonic in O -- O =
 * 0 needs 16 while O = 2 needs only 6 -- which suggests something structural changes in the emitted
 * fetch between those two cases rather than a single arithmetic bound. Setting
 * TERAKAN_DEBUG_DISABLE_MEGA_FETCH_COALESCING made no difference to any threshold, so coalescing is
 * not it. Pinning it down needs the ISA documentation for the fetch bounds check, and until it is
 * pinned down the driver cannot shrink SIZE without repeating the over-truncation that broke dEQP.
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
#include "terakan_vertex_fetch_bounds.vert.spv.h"
};
static const uint32_t fragment_spirv[] = {
#include "terakan_vertex_fetch_bounds.frag.spv.h"

};

int main(void){
   size_t const vsz=sizeof(vertex_spirv), fsz=sizeof(fragment_spirv);
   const uint32_t *vcode=vertex_spirv, *fcode=fragment_spirv;
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
   VkPhysicalDeviceFeatures feat={.robustBufferAccess=VK_TRUE};
   VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.pEnabledFeatures=&feat};
   VkDevice dev; CK(vkCreateDevice(pd,&dci,NULL,&dev)); VkQueue q; vkGetDeviceQueue(dev,0,0,&q);

   /* One allocation holding the vertex data, with several differently sized VkBuffers over it. */
   VkDeviceSize sizes[40]; for(unsigned z=0;z<40;++z) sizes[z]=z+1;
   unsigned const size_count=40;
   VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=4096,.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
   VkBuffer big; CK(vkCreateBuffer(dev,&bci,NULL,&big));
   VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev,big,&mr);
   VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,
     .memoryTypeIndex=mt(pd,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   VkDeviceMemory vmem; CK(vkAllocateMemory(dev,&mai,NULL,&vmem)); CK(vkBindBufferMemory(dev,big,vmem,0));
   uint32_t*vmap; CK(vkMapMemory(dev,vmem,0,VK_WHOLE_SIZE,0,(void**)&vmap));
   for(uint32_t i=0;i<1024;++i) vmap[i]=0xC0DE0000u|i;
   VkBuffer vb[64];
   for(unsigned i=0;i<size_count;++i){ VkBufferCreateInfo c=bci; c.size=sizes[i];
      CK(vkCreateBuffer(dev,&c,NULL,&vb[i])); CK(vkBindBufferMemory(dev,vb[i],vmem,0)); }

   /* 1x1 R32G32B32A32_UINT target, read back through a host-visible buffer. */
   VkImageCreateInfo ici2={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
     .format=VK_FORMAT_R32G32B32A32_UINT,.extent={1,1,1},.mipLevels=1,.arrayLayers=1,
     .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
     .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
     .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage img; CK(vkCreateImage(dev,&ici2,NULL,&img));
   VkMemoryRequirements imr; vkGetImageMemoryRequirements(dev,img,&imr);
   VkMemoryAllocateInfo imai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=imr.size,
     .memoryTypeIndex=mt(pd,imr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
   VkDeviceMemory imem; CK(vkAllocateMemory(dev,&imai,NULL,&imem)); CK(vkBindImageMemory(dev,img,imem,0));
   VkImageViewCreateInfo ivci={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=img,
     .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R32G32B32A32_UINT,
     .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
   VkImageView iv; CK(vkCreateImageView(dev,&ivci,NULL,&iv));
   VkBufferCreateInfo rbci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=16,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   VkBuffer rb; CK(vkCreateBuffer(dev,&rbci,NULL,&rb));
   VkMemoryRequirements rmr; vkGetBufferMemoryRequirements(dev,rb,&rmr);
   VkMemoryAllocateInfo rmai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=rmr.size,
     .memoryTypeIndex=mt(pd,rmr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
   VkDeviceMemory rmem; CK(vkAllocateMemory(dev,&rmai,NULL,&rmem)); CK(vkBindBufferMemory(dev,rb,rmem,0));
   uint32_t*rmap; CK(vkMapMemory(dev,rmem,0,VK_WHOLE_SIZE,0,(void**)&rmap));

   VkAttachmentDescription att={.format=VK_FORMAT_R32G32B32A32_UINT,.samples=VK_SAMPLE_COUNT_1_BIT,
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
   VkShaderModuleCreateInfo vs={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=vsz,.pCode=vcode};
   VkShaderModuleCreateInfo fs={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=fsz,.pCode=fcode};
   VkShaderModule vm,fm; CK(vkCreateShaderModule(dev,&vs,NULL,&vm)); CK(vkCreateShaderModule(dev,&fs,NULL,&fm));
   VkPipelineShaderStageCreateInfo st[2]={
     {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vm,.pName="main"},
     {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fm,.pName="main"}};
   /* Instance rate so all three vertices of the single instance fetch element 0, letting the
    * binding be smaller than one element without the draw needing more of them. */
   struct cfg { const char*name; VkFormat fmt; uint32_t attr_offset; uint32_t elem_bytes; uint32_t first_instance; };
   struct cfg const cfgs[]={
     {"32_32_32_32 offset 0",VK_FORMAT_R32G32B32A32_UINT,0,16,0},
     {"32_32_32_32 offset 2",VK_FORMAT_R32G32B32A32_UINT,2,16,0},
     {"32_32_32_32 offset 4",VK_FORMAT_R32G32B32A32_UINT,4,16,0},
     {"32_32_32_32 offset 6",VK_FORMAT_R32G32B32A32_UINT,6,16,0},
     {"32_32_32_32 offset 8",VK_FORMAT_R32G32B32A32_UINT,8,16,0},
     {"32_32_32_32 offset 12",VK_FORMAT_R32G32B32A32_UINT,12,16,0},
     {"32_32_32_32 offset 16",VK_FORMAT_R32G32B32A32_UINT,16,16,0},
     {"32_32 offset 0",VK_FORMAT_R32G32_UINT,0,8,0},
     {"32_32 offset 4",VK_FORMAT_R32G32_UINT,4,8,0},
     {"32 offset 0",VK_FORMAT_R32_UINT,0,4,0},
     {"32 offset 4",VK_FORMAT_R32_UINT,4,4,0},
   };
   unsigned const cfg_count=sizeof(cfgs)/sizeof(*cfgs);
   VkVertexInputBindingDescription vib={.binding=0,.stride=32,.inputRate=VK_VERTEX_INPUT_RATE_INSTANCE};
   VkVertexInputAttributeDescription via={.location=0,.binding=0,.format=VK_FORMAT_R32G32B32A32_UINT,.offset=0};
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
   VkPipeline pipes[16];
   for(unsigned c=0;c<cfg_count;++c){
      via.format=cfgs[c].fmt; via.offset=cfgs[c].attr_offset;
      CK(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gpci,NULL,&pipes[c]));
   }
   VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VkCommandPool cp; CK(vkCreateCommandPool(dev,&cpi,NULL,&cp));
   VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.commandBufferCount=1};
   VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev,&cbai,&cmd));
   VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fen; CK(vkCreateFence(dev,&fci,NULL,&fen));

   printf("0xC0DExxxx is a source word; 00000000 means the fetch was rejected as out of bounds.\n");
   for(unsigned c=0;c<cfg_count;++c){
   uint32_t const need = cfgs[c].first_instance*32 + cfgs[c].attr_offset + cfgs[c].elem_bytes;
   printf("\n%s: the element occupies bytes [%u,%u), so staying in bounds needs size >= %u\n",
          cfgs[c].name, need-cfgs[c].elem_bytes, need, need);
   int accepted_prev=-1;
   for(unsigned i=0;i<size_count;++i){
      rmap[0]=rmap[1]=rmap[2]=rmap[3]=0xEEEEEEEEu;
      CK(vkResetCommandBuffer(cmd,0));
      VkCommandBufferBeginInfo bgi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      CK(vkBeginCommandBuffer(cmd,&bgi));
      VkClearValue cv={.color={.uint32={0x11111111u,0x11111111u,0x11111111u,0x11111111u}}};
      VkRenderPassBeginInfo rbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=rp,.framebuffer=fb,
        .renderArea={{0,0},{1,1}},.clearValueCount=1,.pClearValues=&cv};
      vkCmdBeginRenderPass(cmd,&rbi,VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipes[c]);
      VkDeviceSize off=0; vkCmdBindVertexBuffers(cmd,0,1,&vb[i],&off);
      vkCmdDraw(cmd,3,1,0,cfgs[c].first_instance);
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
      if(i==0||accepted_prev!=(rmap[0]!=0))
         printf("  size %-3llu %s%s\n",(unsigned long long)sizes[i],rmap[0]!=0?"ACCEPTED":"rejected",
                rmap[0]!=0 && (unsigned long long)sizes[i] < need ? "   <-- READS PAST THE BINDING" : "");
      accepted_prev = rmap[0]!=0;
   }
   }
   vkDeviceWaitIdle(dev);
   return 0;
}
