/*
 * Copyright © 2026 Terakan contributors
 * SPDX-License-Identifier: MIT
 */

/* A characterization tool, not a pass/fail test, which is why it is built but not run by
 * bin/terakan-test.
 *
 * dEQP-VK.api.image_clearing.*.clear_depth_stencil_image is the whole of what api.image_clearing
 * still fails, and the axis that decides it is the image size: 200x180 and 55x21x11 pass
 * throughout, while 1x33, 64x11, 33x128 and 32x29x3 fail, every format failing at 1x33. The
 * message says a texel that should have been cleared still holds its old value, and dEQP does not
 * say which texel.
 *
 * So this clears a depth image of each size to a known value, reads every texel back, and prints
 * the ones that did not take it -- as a coordinate range and a count, so the shape of what the
 * clear missed is visible rather than just its existence.
 *
 * What it found is not dEQP's failure but a neighbouring one, and the two select opposite sizes.
 * With a single mip level -- which is what dEQP creates here -- the clear misses the tail of the
 * two largest images and covers the four small ones completely:
 *
 *     200x180  1568 of 36000 texels left at the fill value, rows 160..175 from x 128 and rows
 *              176..179 from x 96
 *     55x21x11 1678 of 12705, in the last two depth slices
 *
 * `TERAKAN_PROBE_MIPS=1` gives the same images a full mip chain and every size comes back clean,
 * which makes the mip count the deciding variable and the control for it. The depth descriptor is
 * identical either way -- `size=0x0000b018` with pitch 200 and height 184, `slice_tile_max=574`
 * for 36800 texels -- and so is the memory requirement, 147200 bytes, exactly the aligned slice.
 * Forcing linear images changes nothing, and neither does the barrier: the missing texels hold
 * exactly the value the fill copy wrote, so the copy reached them and the draw did not.
 *
 * dEQP's own failures are elsewhere and this does not reproduce them. The sequence
 * here is dEQP's, ingredient by ingredient: the image carries a full mip chain while only level
 * zero is cleared, it is filled from a buffer of zeroes first rather than by a clear, the clear
 * and both copies use `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL`, the barriers around the clear are
 * image barriers naming transfer access on both sides -- which is what Vulkan calls a clear and
 * not the depth write this driver implements it as -- and the readback takes every level in one
 * command at dEQP's four-byte-aligned offsets. Every size passes.
 *
 * The clear itself was compared directly: printing the rectangle and the whole depth/stencil
 * descriptor from inside the driver gives byte-identical lines for the failing dEQP case and the
 * passing run here -- `rect=64x11 size=0x00000807 slice=0x0000000f zinfo=0x00002023`. So the
 * defect is not in what the clear is asked to do, and not in the command sequence around it
 * either. What is left is the state the rest of the process leaves behind, which is where the
 * order dependence this family has always shown points.
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
   };

   for(unsigned c=0;c<sizeof(cases)/sizeof(*cases);++c){
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
      mai.allocationSize = mr.size + bind_offset;
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
      VkImageMemoryBarrier between={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=img,.subresourceRange=whole};
      vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&between);
      VkClearDepthStencilValue const second={.depth=0.1f};
      vkCmdClearDepthStencilImage(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&second,1,&level_zero);
      VkImageMemoryBarrier stored={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=img,.subresourceRange=whole};
      vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&stored);
      vkCmdCopyImageToBuffer(cmd,img,VK_IMAGE_LAYOUT_GENERAL,rb,levels,regions);
      VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
      CK(vkEndCommandBuffer(cmd));
      CK(vkResetFences(dev,1,&fen));
      VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
      CK(vkQueueSubmit(q,1,&si,fen));
      if(vkWaitForFences(dev,1,&fen,VK_TRUE,10000000000ull)!=VK_SUCCESS){fprintf(stderr,"%s: fence wait failed\n",cases[c].name);return 1;}

      unsigned wrong=0, stale=0;
      uint32_t min_x=~0u,max_x=0,min_y=~0u,max_y=0,min_z=~0u,max_z=0;
      for(uint32_t z=0;z<d;++z) for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x){
         float const v=map[(z*h+y)*w+x];
         if(v>=0.0999f&&v<=0.1001f) continue;
         ++wrong; if(v>=0.749f&&v<=0.751f) ++stale;
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
         printf("%-10s %ux%ux%u: %u of %llu texels not cleared (%u still the first clear's value),"
                " x %u..%u, y %u..%u, z %u..%u\n",
                cases[c].name,w,h,d,wrong,(unsigned long long)texels,stale,
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
