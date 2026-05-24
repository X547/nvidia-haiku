/*
 * SPDX-License-Identifier: MIT
 *
 * Same-process opaque-fd memory round-trip: allocate exportable memory,
 * write a pattern, vkGetMemoryFdKHR, then vkAllocateMemory(import) and
 * verify the pattern is visible through the imported mapping.
 */
#include "VulkanFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <cstdint>

using namespace vkr;


namespace {

bool opaqueFdRoundTripSupported(VkPhysicalDevice pdev)
{
   VkPhysicalDeviceExternalBufferInfo info{};
   info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
   info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

   VkExternalBufferProperties props{};
   props.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;

   vkGetPhysicalDeviceExternalBufferProperties(pdev, &info, &props);
   const auto feats = props.externalMemoryProperties.externalMemoryFeatures;
   return (feats & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) &&
          (feats & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
}

constexpr uint8_t pattern_byte(size_t i)
{
   return static_cast<uint8_t>((i * 31u + 7u) & 0xffu);
}

} // namespace


TEST_CASE("opaque-fd memory export/import round-trip",
          "[external_memory][opaque_fd]")
{
   DeviceInit init{};
   init.deviceExtensions = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME };
   TestFixture fix(init);

   if (!opaqueFdRoundTripSupported(fix.physicalDevice())) {
      SKIP("opaque-fd not exportable+importable on this device");
   }

   auto vkGetMemoryFdKHR =
      fix.loadDeviceFn<PFN_vkGetMemoryFdKHR>("vkGetMemoryFdKHR");
   REQUIRE(vkGetMemoryFdKHR != nullptr);

   constexpr VkDeviceSize SIZE = 64 * 1024;
   constexpr VkMemoryPropertyFlags REQ_PROPS =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

   /* Use a dummy external buffer to obtain a memoryTypeBits mask that's
    * known to be valid for buffer-bound exportable allocations. */
   VkExternalMemoryBufferCreateInfo extBufInfo{};
   extBufInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
   extBufInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

   VkBufferCreateInfo bci{};
   bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   bci.pNext = &extBufInfo;
   bci.size = SIZE;
   bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
   bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

   VkBuffer rawProbe = VK_NULL_HANDLE;
   REQUIRE(vkCreateBuffer(fix.device(), &bci, nullptr, &rawProbe) == VK_SUCCESS);
   Buffer probe(fix.device(), rawProbe);

   VkMemoryRequirements memReqs{};
   vkGetBufferMemoryRequirements(fix.device(), probe.get(), &memReqs);

   const uint32_t typeIdx = fix.findMemoryType(memReqs.memoryTypeBits, REQ_PROPS);
   if (typeIdx == UINT32_MAX) {
      SKIP("no HOST_VISIBLE|HOST_COHERENT memory type compatible with the probe buffer");
   }

   /* ---------- Export side: alloc + populate + GetMemoryFd ---------- */

   VkExportMemoryAllocateInfo exportInfo{};
   exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
   exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

   VkMemoryAllocateInfo allocInfo{};
   allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   allocInfo.pNext = &exportInfo;
   allocInfo.allocationSize = SIZE;
   allocInfo.memoryTypeIndex = typeIdx;

   VkDeviceMemory rawExporter = VK_NULL_HANDLE;
   REQUIRE(vkAllocateMemory(fix.device(), &allocInfo, nullptr, &rawExporter) ==
           VK_SUCCESS);
   DeviceMemory exporter(fix.device(), rawExporter);

   {
      void* ptr = nullptr;
      REQUIRE(vkMapMemory(fix.device(), exporter, 0, SIZE, 0, &ptr) == VK_SUCCESS);
      MappedMemory map(fix.device(), exporter, ptr);
      auto* bytes = static_cast<uint8_t*>(ptr);
      for (size_t i = 0; i < SIZE; i++)
         bytes[i] = pattern_byte(i);
   }

   VkMemoryGetFdInfoKHR getFdInfo{};
   getFdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
   getFdInfo.memory = exporter;
   getFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

   int rawFd = -1;
   REQUIRE(vkGetMemoryFdKHR(fix.device(), &getFdInfo, &rawFd) == VK_SUCCESS);
   REQUIRE(rawFd >= 0);
   UniqueFd fd(rawFd);

   /* ---------- Import side: alloc(import) + verify ---------- */

   VkImportMemoryFdInfoKHR importInfo{};
   importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
   importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
   importInfo.fd = fd.get(); /* ownership transfers iff alloc succeeds */

   VkMemoryAllocateInfo importAlloc{};
   importAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   importAlloc.pNext = &importInfo;
   importAlloc.allocationSize = SIZE;
   importAlloc.memoryTypeIndex = typeIdx;

   VkDeviceMemory rawImporter = VK_NULL_HANDLE;
   const VkResult importRes =
      vkAllocateMemory(fix.device(), &importAlloc, nullptr, &rawImporter);
   if (importRes == VK_SUCCESS) {
      /* Vulkan now owns the fd. */
      (void)fd.release();
   }
   REQUIRE(importRes == VK_SUCCESS);
   DeviceMemory importer(fix.device(), rawImporter);

   {
      void* ptr = nullptr;
      REQUIRE(vkMapMemory(fix.device(), importer, 0, SIZE, 0, &ptr) == VK_SUCCESS);
      MappedMemory map(fix.device(), importer, ptr);
      const auto* bytes = static_cast<const uint8_t*>(ptr);
      bool ok = true;
      size_t firstBad = 0;
      for (size_t i = 0; i < SIZE; i++) {
         if (bytes[i] != pattern_byte(i)) {
            ok = false;
            firstBad = i;
            break;
         }
      }
      INFO("first mismatched byte index: " << firstBad);
      REQUIRE(ok);
   }
}
