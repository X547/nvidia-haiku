/*
 * SPDX-License-Identifier: MIT
 *
 * VK_EXT_external_memory_host: import an application-owned page-aligned host
 * buffer, bind it to a VkBuffer, and exercise it from the GPU in both
 * directions via the copy queue:
 *
 *   read  — CPU fills the host buffer, GPU copies it into a peer buffer, CPU
 *           verifies the peer (proves the GPU read the imported host pages).
 *   write — CPU fills the peer, GPU copies it into the host buffer, CPU
 *           verifies its own original pointer (proves the GPU wrote the same
 *           physical pages the application still sees).
 */
#include "VulkanFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>

using namespace vkr;


namespace {

using CommandPool = DeviceChild<VkCommandPool, vkDestroyCommandPool>;

bool hostImportSupported(VkPhysicalDevice pdev)
{
   VkPhysicalDeviceExternalBufferInfo info{};
   info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
   info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;

   VkExternalBufferProperties props{};
   props.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;

   vkGetPhysicalDeviceExternalBufferProperties(pdev, &info, &props);
   return props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
}

VkDeviceSize minImportAlignment(VkPhysicalDevice pdev)
{
   VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps{};
   hostProps.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;

   VkPhysicalDeviceProperties2 props2{};
   props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
   props2.pNext = &hostProps;

   vkGetPhysicalDeviceProperties2(pdev, &props2);
   return hostProps.minImportedHostPointerAlignment;
}

constexpr uint8_t patternA(size_t i)
{
   return static_cast<uint8_t>((i * 31u + 7u) & 0xffu);
}

constexpr uint8_t patternB(size_t i)
{
   return static_cast<uint8_t>((i * 13u + 0x5au) & 0xffu);
}

} // namespace


TEST_CASE("external_memory_host import round-trip via copy queue",
          "[external_memory][host_pointer]")
{
   DeviceInit init{};
   init.deviceExtensions = { VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME };
   TestFixture fix(init);

   if (!hostImportSupported(fix.physicalDevice())) {
      SKIP("host-pointer import not supported on this device");
   }

   /* The reported alignment must be a power of two (Vulkan guarantee). */
   const VkDeviceSize reqAlign = minImportAlignment(fix.physicalDevice());
   REQUIRE(reqAlign != 0);
   REQUIRE((reqAlign & (reqAlign - 1)) == 0);

   auto vkGetMemoryHostPointerPropertiesEXT =
      fix.loadDeviceFn<PFN_vkGetMemoryHostPointerPropertiesEXT>(
         "vkGetMemoryHostPointerPropertiesEXT");
   REQUIRE(vkGetMemoryHostPointerPropertiesEXT != nullptr);

   constexpr VkDeviceSize SIZE = 64 * 1024;

   /* ---------- Create the host-backed buffer + import its memory ---------- */

   VkExternalMemoryBufferCreateInfo extBufInfo{};
   extBufInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
   extBufInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;

   VkBufferCreateInfo bci{};
   bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   bci.pNext = &extBufInfo;
   bci.size = SIZE;
   bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

   VkBuffer rawHostBuf = VK_NULL_HANDLE;
   REQUIRE(vkCreateBuffer(fix.device(), &bci, nullptr, &rawHostBuf) == VK_SUCCESS);
   Buffer hostBuf(fix.device(), rawHostBuf);

   VkMemoryRequirements hostReqs{};
   vkGetBufferMemoryRequirements(fix.device(), hostBuf.get(), &hostReqs);

   /* Allocate the application buffer: aligned for import and large enough for
    * the (possibly padded) buffer memory requirement. */
   const VkDeviceSize importAlign =
      std::max<VkDeviceSize>(reqAlign, 4096);
   const VkDeviceSize hostAllocSize =
      (hostReqs.size + importAlign - 1) & ~(importAlign - 1);

   void* hostPtr = nullptr;
   REQUIRE(posix_memalign(&hostPtr, importAlign, hostAllocSize) == 0);
   REQUIRE(hostPtr != nullptr);
   auto* hostBytes = static_cast<uint8_t*>(hostPtr);

   VkMemoryHostPointerPropertiesEXT hostPtrProps{};
   hostPtrProps.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
   REQUIRE(vkGetMemoryHostPointerPropertiesEXT(
              fix.device(),
              VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
              hostPtr, &hostPtrProps) == VK_SUCCESS);

   const uint32_t hostTypeIdx = fix.findMemoryType(
      hostReqs.memoryTypeBits & hostPtrProps.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   if (hostTypeIdx == UINT32_MAX) {
      free(hostPtr);
      SKIP("no host-visible memory type valid for host-pointer import");
   }

   VkImportMemoryHostPointerInfoEXT importInfo{};
   importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
   importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
   importInfo.pHostPointer = hostPtr;

   VkMemoryAllocateInfo hostAlloc{};
   hostAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   hostAlloc.pNext = &importInfo;
   hostAlloc.allocationSize = hostAllocSize;
   hostAlloc.memoryTypeIndex = hostTypeIdx;

   VkDeviceMemory rawHostMem = VK_NULL_HANDLE;
   const VkResult importRes =
      vkAllocateMemory(fix.device(), &hostAlloc, nullptr, &rawHostMem);
   if (importRes != VK_SUCCESS) {
      free(hostPtr);
      REQUIRE(importRes == VK_SUCCESS);
   }
   DeviceMemory hostMem(fix.device(), rawHostMem);
   REQUIRE(vkBindBufferMemory(fix.device(), hostBuf.get(), hostMem, 0) == VK_SUCCESS);

   /* ---------- Peer buffer: ordinary host-visible memory ---------- */

   VkBufferCreateInfo peerBci{};
   peerBci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   peerBci.size = SIZE;
   peerBci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   peerBci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

   VkBuffer rawPeerBuf = VK_NULL_HANDLE;
   REQUIRE(vkCreateBuffer(fix.device(), &peerBci, nullptr, &rawPeerBuf) == VK_SUCCESS);
   Buffer peerBuf(fix.device(), rawPeerBuf);

   VkMemoryRequirements peerReqs{};
   vkGetBufferMemoryRequirements(fix.device(), peerBuf.get(), &peerReqs);
   const uint32_t peerTypeIdx = fix.findMemoryType(
      peerReqs.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (peerTypeIdx == UINT32_MAX) {
      free(hostPtr);
      SKIP("no HOST_VISIBLE|HOST_COHERENT memory type for the peer buffer");
   }

   VkMemoryAllocateInfo peerAlloc{};
   peerAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   peerAlloc.allocationSize = peerReqs.size;
   peerAlloc.memoryTypeIndex = peerTypeIdx;

   VkDeviceMemory rawPeerMem = VK_NULL_HANDLE;
   REQUIRE(vkAllocateMemory(fix.device(), &peerAlloc, nullptr, &rawPeerMem) == VK_SUCCESS);
   DeviceMemory peerMem(fix.device(), rawPeerMem);
   REQUIRE(vkBindBufferMemory(fix.device(), peerBuf.get(), peerMem, 0) == VK_SUCCESS);

   void* peerMap = nullptr;
   REQUIRE(vkMapMemory(fix.device(), peerMem, 0, SIZE, 0, &peerMap) == VK_SUCCESS);
   MappedMemory peerMapping(fix.device(), peerMem, peerMap);
   auto* peerBytes = static_cast<uint8_t*>(peerMap);

   /* ---------- Command pool + a copy helper ---------- */

   VkCommandPoolCreateInfo poolInfo{};
   poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   poolInfo.queueFamilyIndex = fix.queueFamilyIndex();

   VkCommandPool rawPool = VK_NULL_HANDLE;
   REQUIRE(vkCreateCommandPool(fix.device(), &poolInfo, nullptr, &rawPool) == VK_SUCCESS);
   CommandPool pool(fix.device(), rawPool);

   auto runCopy = [&](VkBuffer src, VkBuffer dst) {
      VkCommandBufferAllocateInfo cbai{};
      cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      cbai.commandPool = pool.get();
      cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      cbai.commandBufferCount = 1;
      VkCommandBuffer cmd = VK_NULL_HANDLE;
      REQUIRE(vkAllocateCommandBuffers(fix.device(), &cbai, &cmd) == VK_SUCCESS);

      VkCommandBufferBeginInfo begin{};
      begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      REQUIRE(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);

      VkBufferCopy region{};
      region.size = SIZE;
      vkCmdCopyBuffer(cmd, src, dst, 1, &region);
      REQUIRE(vkEndCommandBuffer(cmd) == VK_SUCCESS);

      VkSubmitInfo submit{};
      submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &cmd;
      REQUIRE(vkQueueSubmit(fix.queue(), 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
      REQUIRE(vkQueueWaitIdle(fix.queue()) == VK_SUCCESS);

      vkFreeCommandBuffers(fix.device(), pool.get(), 1, &cmd);
   };

   /* ---------- Read: host buffer -> peer ---------- */

   for (size_t i = 0; i < SIZE; i++)
      hostBytes[i] = patternA(i);
   memset(peerBytes, 0xcc, SIZE);

   runCopy(hostBuf.get(), peerBuf.get());

   {
      bool ok = true;
      size_t firstBad = 0;
      for (size_t i = 0; i < SIZE; i++) {
         if (peerBytes[i] != patternA(i)) { ok = false; firstBad = i; break; }
      }
      INFO("read: first mismatched byte index: " << firstBad);
      REQUIRE(ok);
   }

   /* ---------- Write: peer -> host buffer ---------- */

   for (size_t i = 0; i < SIZE; i++)
      peerBytes[i] = patternB(i);
   memset(hostBytes, 0, SIZE);

   runCopy(peerBuf.get(), hostBuf.get());

   {
      bool ok = true;
      size_t firstBad = 0;
      for (size_t i = 0; i < SIZE; i++) {
         if (hostBytes[i] != patternB(i)) { ok = false; firstBad = i; break; }
      }
      INFO("write: first mismatched byte index: " << firstBad);
      REQUIRE(ok);
   }

   /* The imported memory references hostPtr; free Vulkan memory before the
    * backing allocation. */
   hostMem.reset();
   free(hostPtr);
}
