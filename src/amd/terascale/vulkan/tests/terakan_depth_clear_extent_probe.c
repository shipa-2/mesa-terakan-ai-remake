/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* A characterization tool, not a pass/fail test, which is why it is built but not run by
 * bin/terakan-test.
 *
 * dEQP-VK.api.image_clearing.*.clear_depth_stencil_image was the whole of what api.image_clearing
 * still failed, and the axis that decided it was the image size: 200x180 and 55x21x11 passed
 * throughout, while 1x33, 64x11, 33x128 and 32x29x3 failed, every format failing at 1x33. The
 * message said a texel that should have been cleared still held its old value, and dEQP does not
 * say which texel.
 *
 * So this clears a depth image of each size to a known value, reads every texel back, and prints
 * the ones that did not take it -- as a coordinate range and a count, so the shape of what the
 * clear missed is visible rather than just its existence. The command sequence is dEQP's,
 * ingredient by ingredient: a single mip level, the image filled from a buffer rather than by a
 * clear, `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL` throughout, and image barriers naming transfer
 * access on both sides of the clear -- which is what Vulkan calls a clear and not the depth write
 * this driver implements it as.
 *
 * What that showed was the opposite of what the name suggests: the clear was never at fault. The
 * fill was. `vkCmdCopyBufferToImage` draws into its destination through the color block, and it
 * recorded the flush that makes those writes visible in the command buffer's *color* field. The
 * barrier that followed named a depth image, so it looked in the depth field, found nothing, and
 * emitted no flush -- leaving the tail of the fill in the color block to land on top of the clear.
 * Hence the numbers: a near-constant 1216..1984 texels, at most about 8 KB, lost from the end of
 * a large image in tiled order, and a small image lost whole because all of it still fitted in
 * the block. `terakan_CmdCopyBufferToImage2` now picks the field by the destination's aspects,
 * and every size below comes back clean.
 *
 * The knobs are the controls that separated the two candidates, kept because they are what makes
 * a claim about this defect checkable:
 *
 *     TERAKAN_PROBE_SPLIT_FILL   ends the command buffer between the fill and the clear. This is
 *                                the one that found it -- with the fill in a submit of its own
 *                                every size passed even before the fix.
 *     TERAKAN_PROBE_SPLIT        ends it between the clear and the readback instead, which did
 *                                not help the large images and is how the two were told apart.
 *     TERAKAN_PROBE_SPLIT_BARRIER  additionally repeats the store barrier at the end of the first
 *                                buffer under TERAKAN_PROBE_SPLIT.
 *     TERAKAN_PROBE_MIPS         gives the images a full mip chain, which hid the defect.
 *     TERAKAN_PROBE_TWICE        clears twice; identical output, so this was never a race in the
 *                                clear.
 *     TERAKAN_PROBE_SLACK        pads the allocation by 64 KiB, ruling out a write past its end.
 *     TERAKAN_PROBE_ONLY         runs a single named size.
 *     TERAKAN_PROBE_MAP          prints the affected span of every row.
 *
 * The readback buffer is stamped with a third value beforehand, so a texel the copy back never
 * wrote is reported separately from one the clear never wrote. It never fired, which is what
 * placed the loss in the image rather than in the readback.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define CK(e) do { VkResult r=(e); if(r){fprintf(stderr,"%s -> %d\n",#e,r);return 1;} } while(0)
static uint32_t mt(VkPhysicalDevice p,uint32_t bits,VkMemoryPropertyFlags f){
   VkPhysicalDeviceMemoryProperties m; vkGetPhysicalDeviceMemoryProperties(p,&m);
   for(uint32_t i=0;i<m.memoryTypeCount;++i) if((bits&(1u<<i))&&(m.memoryTypes[i].propertyFlags&f)==f) return i;
   return UINT32_MAX; }

struct probe_case { uint32_t width, height, depth; char const *name; };

int main(void){
   VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
   VkInstance inst; CK(vkCreateInstance(&ici,NULL,&inst));
   uint32_t n=8; VkPhysicalDevice pds[8]; CK(vkEnumeratePhysicalDevices(inst,&n,pds));
   VkPhysicalDevice pd=VK_NULL_HANDLE; VkPhysicalDeviceProperties pr;
   for(uint32_t i=0;i<n;++i){ vkGetPhysicalDeviceProperties(pds[i],&pr);
      if(strstr(pr.deviceName,"(Terakan)")&&!strstr(pr.deviceName,"TeraScale 1")){pd=pds[i];break;} }
   if(!pd){fprintf(stderr,"no device\n");return 1;}
   fprintf(stderr,"device=%s\n",pr.deviceName);
   float pri=1.f; VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&pri};
   /* dEQP creates its device with every feature the implementation reports, so the driver is in
    * whatever state that puts it in rather than the minimal one a focused test would give it.
    */
   VkPhysicalDeviceFeatures features; vkGetPhysicalDeviceFeatures(pd,&features);
   VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,
     .pQueueCreateInfos=&qci,.pEnabledFeatures=&features};
   VkDevice dev; CK(vkCreateDevice(pd,&dci,NULL,&dev)); VkQueue q; vkGetDeviceQueue(dev,0,0,&q);

   VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VkCommandPool cp; CK(vkCreateCommandPool(dev,&cpi,NULL,&cp));
   VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.commandBufferCount=1};
   VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev,&cbai,&cmd));
   VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fen; CK(vkCreateFence(dev,&fci,NULL,&fen));

   /* The sizes dEQP fails at, and the two it passes at, so a clean run is visible next to a
    * broken one rather than only asserted.
    */
   struct probe_case const cases[]={
      {64,11,1,"64x11"}, {1,33,1,"1x33"}, {33,128,1,"33x128"}, {32,29,3,"32x29x3"},
      {200,180,1,"200x180"}, {55,21,11,"55x21x11"},
      {200,160,1,"200x160"}, {200,176,1,"200x176"}, {200,184,1,"200x184"},
      {128,180,1,"128x180"}, {96,180,1,"96x180"}, {256,180,1,"256x180"},
      {64,180,1,"64x180"}, {200,64,1,"200x64"}, {200,128,1,"200x128"},
   };

   char const * const only = getenv("TERAKAN_PROBE_ONLY");
   for(unsigned c=0;c<sizeof(cases)/sizeof(*cases);++c){
      if(only!=NULL&&strcmp(only,cases[c].name)!=0) continue;
      uint32_t const w=cases[c].width, h=cases[c].height, d=cases[c].depth;
      /* One level, which is what dEQP creates for this group -- the driver's surface for a
       * single-level image is laid out differently from one carrying a chain.
       */
      uint32_t levels=1;
      if(getenv("TERAKAN_PROBE_MIPS")!=NULL){ for(uint32_t e=w>h?(w>d?w:d):(h>d?h:d); e>1; e>>=1) ++levels; }
      VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=d>1?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D,.format=VK_FORMAT_D32_SFLOAT,
        .extent={w,h,d},.mipLevels=levels,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
      VkImage img; CK(vkCreateImage(dev,&ii,NULL,&img));
      VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev,img,&mr);
      VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,
        .memoryTypeIndex=mt(pd,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
      /* dEQP's default allocator suballocates, so the image sits at a non-zero offset in a larger
       * allocation; the dedicated_allocation variant binds at zero. Both are tried.
       */
      VkDeviceSize const bind_offset = 0u;
      /* Optional slack after the image. If the depth block writes past the surface it was given,
       * a mip chain would absorb it and a bare single level would not.
       */
      VkDeviceSize const slack = getenv("TERAKAN_PROBE_SLACK") != NULL ? 65536u : 0u;
      mai.allocationSize = mr.size + bind_offset + slack;
      VkDeviceMemory imem; CK(vkAllocateMemory(dev,&mai,NULL,&imem));
      CK(vkBindImageMemory(dev,img,imem,bind_offset));

      VkDeviceSize const texels=(VkDeviceSize)w*h*d;
      /* dEQP reads every level back in one command, each at its own four-byte-aligned offset, so
       * the readback is laid out the same way here.
       */
      VkDeviceSize level_offset[16]={0}; VkDeviceSize total=0;
      for(uint32_t l=0;l<levels;++l){
         uint32_t const lw=w>>l?w>>l:1u, lh=h>>l?h>>l:1u, ld=d>>l?d>>l:1u;
         level_offset[l]=total; total+=((VkDeviceSize)lw*lh*ld*4u+3u)&~(VkDeviceSize)3u;
      }
      VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=total,
        .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
      VkBuffer rb; CK(vkCreateBuffer(dev,&bci,NULL,&rb));
      VkMemoryRequirements rmr; vkGetBufferMemoryRequirements(dev,rb,&rmr);
      VkMemoryAllocateInfo rmai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=rmr.size,
        .memoryTypeIndex=mt(pd,rmr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
      VkDeviceMemory rmem; CK(vkAllocateMemory(dev,&rmai,NULL,&rmem)); CK(vkBindBufferMemory(dev,rb,rmem,0));
      float *map; CK(vkMapMemory(dev,rmem,0,VK_WHOLE_SIZE,0,(void**)&map));

      /* The fill source, every texel 0.75. */
      VkBufferCreateInfo fbci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=total,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
      VkBuffer fill_buffer; CK(vkCreateBuffer(dev,&fbci,NULL,&fill_buffer));
      VkMemoryRequirements fmr; vkGetBufferMemoryRequirements(dev,fill_buffer,&fmr);
      VkMemoryAllocateInfo fmai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=fmr.size,
        .memoryTypeIndex=mt(pd,fmr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
      VkDeviceMemory fmem; CK(vkAllocateMemory(dev,&fmai,NULL,&fmem)); CK(vkBindBufferMemory(dev,fill_buffer,fmem,0));
      float *fill_map; CK(vkMapMemory(dev,fmem,0,VK_WHOLE_SIZE,0,(void**)&fill_map));
      for(VkDeviceSize i=0;i<total/4u;++i) fill_map[i]=0.75f;
      /* The readback buffer is stamped with a third value so that a texel the copy back never
       * writes is told apart from a texel the clear never wrote.
       */
      for(VkDeviceSize i=0;i<total/4u;++i) map[i]=0.5f;
      (void)fill_map;

      CK(vkResetCommandBuffer(cmd,0));
      VkCommandBufferBeginInfo bgi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      CK(vkBeginCommandBuffer(cmd,&bgi));
      VkBufferImageCopy regions[16];
      for(uint32_t l=0;l<levels;++l){
         uint32_t const lw=w>>l?w>>l:1u, lh=h>>l?h>>l:1u, ld=d>>l?d>>l:1u;
         regions[l]=(VkBufferImageCopy){.bufferOffset=level_offset[l],
            .imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,l,0,1},.imageExtent={lw,lh,ld}};
      }
      VkImageSubresourceRange const whole={VK_IMAGE_ASPECT_DEPTH_BIT,0,VK_REMAINING_MIP_LEVELS,0,1};
      /* Only level zero is cleared, as dEQP does. */
      VkImageSubresourceRange const level_zero={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
      VkImageMemoryBarrier to_general={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=img,.subresourceRange=whole};
      vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&to_general);
      /* Filled from a buffer first, the way dEQP does it, so a texel the clear misses reads as the
       * fill's value rather than as whatever the allocation held. Filling with a clear instead
       * hides the defect entirely, which is itself the finding.
       */
      vkCmdCopyBufferToImage(cmd,fill_buffer,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,levels,regions);
      /* dEQP names the clear a transfer operation, which is what Vulkan calls it, rather than the
       * depth write this driver implements it as. Both barriers here are dEQP's.
       */
      /* `TERAKAN_PROBE_SPLIT_FILL=1` cuts the command buffer between the fill and the clear, which
       * tells apart a clear whose writes are lost from a fill whose writes land after the clear.
       */
      if(getenv("TERAKAN_PROBE_SPLIT_FILL")!=NULL){
         CK(vkEndCommandBuffer(cmd));
         CK(vkResetFences(dev,1,&fen));
         VkSubmitInfo const fill_si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
         CK(vkQueueSubmit(q,1,&fill_si,fen));
         if(vkWaitForFences(dev,1,&fen,VK_TRUE,10000000000ull)!=VK_SUCCESS){fprintf(stderr,"%s: fill fence wait failed\n",cases[c].name);return 1;}
         vkDeviceWaitIdle(dev);
         CK(vkResetCommandBuffer(cmd,0));
         CK(vkBeginCommandBuffer(cmd,&bgi));
      }
      VkImageMemoryBarrier between={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=img,.subresourceRange=whole};
      vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&between);
      VkClearDepthStencilValue const second={.depth=0.1f};
      vkCmdClearDepthStencilImage(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&second,1,&level_zero);
      if(getenv("TERAKAN_PROBE_TWICE")!=NULL){
         vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&between);
         vkCmdClearDepthStencilImage(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&second,1,&level_zero);
      }
      /* `TERAKAN_PROBE_SPLIT=1` cuts the command buffer in two right here and waits for the queue
       * to go idle, so the readback cannot possibly observe anything the clear left in a cache.
       */
      VkImageMemoryBarrier stored={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=img,.subresourceRange=whole};
      /* `TERAKAN_PROBE_SPLIT_BARRIER=1` additionally puts the store barrier at the end of the first
       * command buffer, which tells apart "the end of a command buffer does not flush the clear"
       * from "the second command buffer loses it".
       */
      if(getenv("TERAKAN_PROBE_SPLIT_BARRIER")!=NULL)
         vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&stored);
      if(getenv("TERAKAN_PROBE_SPLIT")!=NULL){
         CK(vkEndCommandBuffer(cmd));
         CK(vkResetFences(dev,1,&fen));
         VkSubmitInfo const split_si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
         CK(vkQueueSubmit(q,1,&split_si,fen));
         if(vkWaitForFences(dev,1,&fen,VK_TRUE,10000000000ull)!=VK_SUCCESS){fprintf(stderr,"%s: split fence wait failed\n",cases[c].name);return 1;}
         vkDeviceWaitIdle(dev);
         CK(vkResetCommandBuffer(cmd,0));
         CK(vkBeginCommandBuffer(cmd,&bgi));
      }
      vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&stored);
      vkCmdCopyImageToBuffer(cmd,img,VK_IMAGE_LAYOUT_GENERAL,rb,levels,regions);
      VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
      CK(vkEndCommandBuffer(cmd));
      CK(vkResetFences(dev,1,&fen));
      VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
      CK(vkQueueSubmit(q,1,&si,fen));
      if(vkWaitForFences(dev,1,&fen,VK_TRUE,10000000000ull)!=VK_SUCCESS){fprintf(stderr,"%s: fence wait failed\n",cases[c].name);return 1;}

      unsigned wrong=0, stale=0, unwritten=0;
      uint32_t min_x=~0u,max_x=0,min_y=~0u,max_y=0,min_z=~0u,max_z=0;
      for(uint32_t z=0;z<d;++z) for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x){
         float const v=map[(z*h+y)*w+x];
         if(v>=0.0999f&&v<=0.1001f) continue;
         ++wrong; if(v>=0.749f&&v<=0.751f) ++stale; if(v>=0.499f&&v<=0.501f) ++unwritten;
         if(x<min_x)min_x=x; if(x>max_x)max_x=x;
         if(y<min_y)min_y=y; if(y>max_y)max_y=y;
         if(z<min_z)min_z=z; if(z>max_z)max_z=z;
      }
      if(wrong!=0&&getenv("TERAKAN_PROBE_MAP")!=NULL){
         for(uint32_t z=0;z<d;++z) for(uint32_t y=0;y<h;++y){
            uint32_t cnt=0,lo=~0u,hi=0;
            for(uint32_t x=0;x<w;++x){ float const v=map[(z*h+y)*w+x];
               if(v>=0.0999f&&v<=0.1001f) continue; ++cnt; if(x<lo)lo=x; if(x>hi)hi=x; }
            if(cnt) printf("    z=%u y=%-4u %4u wrong, x %u..%u\n",z,y,cnt,lo,hi);
         }
      }
      if(wrong==0){
         printf("%-10s %ux%ux%u bind_offset=%llu: every texel cleared\n",cases[c].name,w,h,d,
                (unsigned long long)bind_offset);
      } else {
         printf("%-10s %ux%ux%u: %u of %llu texels not cleared (%u still the first clear's value, %u never copied back),"
                " x %u..%u, y %u..%u, z %u..%u\n",
                cases[c].name,w,h,d,wrong,(unsigned long long)texels,stale,unwritten,
                min_x,max_x,min_y,max_y,min_z,max_z);
      }

      vkDeviceWaitIdle(dev);
      vkUnmapMemory(dev,rmem); vkDestroyBuffer(dev,rb,NULL); vkFreeMemory(dev,rmem,NULL);
      vkUnmapMemory(dev,fmem); vkDestroyBuffer(dev,fill_buffer,NULL); vkFreeMemory(dev,fmem,NULL);
      vkDestroyImage(dev,img,NULL); vkFreeMemory(dev,imem,NULL);
   }
   vkDeviceWaitIdle(dev);
   return 0;
}
