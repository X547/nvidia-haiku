/*
 * SPDX-License-Identifier: MIT
 *
 * Timeline semaphore opaque-fd export/import between two *independent*
 * Vulkan contexts (separate VkInstance + VkDevice on the same GPU), and a
 * check that the shared semaphore actually carries signals across them:
 *
 *   - context A creates an exportable timeline semaphore and exports an fd;
 *   - context B creates a timeline semaphore and permanently imports the fd;
 *   - a CPU signal on A is observed (counter value + wait) on B;
 *   - an unreached value times out on B;
 *   - a GPU signal on A (empty submit) is observed on B;
 *   - a CPU signal on B is observed back on A.
 *
 * If the import/export plumbing or the underlying shared semaphore payload
 * is broken, the cross-context waits would time out instead of succeeding.
 */
#include "VulkanFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace vkr;


namespace {

constexpr uint64_t kSecond = 1000ull * 1000ull * 1000ull;

bool timelineOpaqueFdSupported(VkPhysicalDevice pdev)
{
   VkSemaphoreTypeCreateInfo typeInfo{};
   typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
   typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

   VkPhysicalDeviceExternalSemaphoreInfo info{};
   info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
   info.pNext = &typeInfo;
   info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

   VkExternalSemaphoreProperties props{};
   props.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;

   vkGetPhysicalDeviceExternalSemaphoreProperties(pdev, &info, &props);
   const auto feats = props.externalSemaphoreFeatures;
   return (feats & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) &&
          (feats & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);
}

VkSemaphore createTimeline(VkDevice dev, uint64_t initialValue,
                           bool exportable, Semaphore& holder)
{
   VkSemaphoreTypeCreateInfo typeInfo{};
   typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
   typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
   typeInfo.initialValue = initialValue;

   VkExportSemaphoreCreateInfo exportInfo{};
   exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
   exportInfo.pNext = &typeInfo;
   exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

   VkSemaphoreCreateInfo ci{};
   ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
   ci.pNext = exportable ? static_cast<const void*>(&exportInfo)
                         : static_cast<const void*>(&typeInfo);

   VkSemaphore raw = VK_NULL_HANDLE;
   REQUIRE(vkCreateSemaphore(dev, &ci, nullptr, &raw) == VK_SUCCESS);
   holder = Semaphore(dev, raw);
   return raw;
}

} // namespace


TEST_CASE("timeline semaphore opaque-fd import/export across two contexts",
          "[external_semaphore][opaque_fd][timeline]")
{
   /* timelineSemaphore feature, enabled on both contexts' devices. */
   VkPhysicalDeviceTimelineSemaphoreFeatures tlFeat{};
   tlFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
   tlFeat.timelineSemaphore = VK_TRUE;

   DeviceInit init{};
   init.apiVersion = VK_API_VERSION_1_1;
   init.deviceExtensions = {
      VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
   };
   init.deviceNext = &tlFeat;

   /* Two fully independent contexts (separate instance + device). */
   TestFixture ctxA(init);
   TestFixture ctxB(init);

   if (!timelineOpaqueFdSupported(ctxA.physicalDevice())) {
      SKIP("timeline opaque-fd not exportable+importable on this device");
   }

   auto vkGetSemaphoreFdKHR_A =
      ctxA.loadDeviceFn<PFN_vkGetSemaphoreFdKHR>("vkGetSemaphoreFdKHR");
   auto vkImportSemaphoreFdKHR_B =
      ctxB.loadDeviceFn<PFN_vkImportSemaphoreFdKHR>("vkImportSemaphoreFdKHR");
   auto vkSignalSemaphore_A =
      ctxA.loadDeviceFn<PFN_vkSignalSemaphoreKHR>("vkSignalSemaphoreKHR");
   auto vkSignalSemaphore_B =
      ctxB.loadDeviceFn<PFN_vkSignalSemaphoreKHR>("vkSignalSemaphoreKHR");
   auto vkWaitSemaphores_A =
      ctxA.loadDeviceFn<PFN_vkWaitSemaphoresKHR>("vkWaitSemaphoresKHR");
   auto vkWaitSemaphores_B =
      ctxB.loadDeviceFn<PFN_vkWaitSemaphoresKHR>("vkWaitSemaphoresKHR");
   auto vkGetValue_A =
      ctxA.loadDeviceFn<PFN_vkGetSemaphoreCounterValueKHR>("vkGetSemaphoreCounterValueKHR");
   auto vkGetValue_B =
      ctxB.loadDeviceFn<PFN_vkGetSemaphoreCounterValueKHR>("vkGetSemaphoreCounterValueKHR");
   REQUIRE(vkGetSemaphoreFdKHR_A != nullptr);
   REQUIRE(vkImportSemaphoreFdKHR_B != nullptr);
   REQUIRE(vkSignalSemaphore_A != nullptr);
   REQUIRE(vkSignalSemaphore_B != nullptr);
   REQUIRE(vkWaitSemaphores_A != nullptr);
   REQUIRE(vkWaitSemaphores_B != nullptr);
   REQUIRE(vkGetValue_A != nullptr);
   REQUIRE(vkGetValue_B != nullptr);

   /* ---------- A: exportable timeline (initial 0), export fd ---------- */

   Semaphore semA;
   VkSemaphore semAHandle = createTimeline(ctxA.device(), 0, /*exportable=*/true, semA);

   VkSemaphoreGetFdInfoKHR getFd{};
   getFd.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
   getFd.semaphore = semAHandle;
   getFd.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

   int rawFd = -1;
   REQUIRE(vkGetSemaphoreFdKHR_A(ctxA.device(), &getFd, &rawFd) == VK_SUCCESS);
   REQUIRE(rawFd >= 0);
   UniqueFd fd(rawFd);

   /* ---------- B: timeline (initial 0), permanently import fd ---------- */

   Semaphore semB;
   VkSemaphore semBHandle = createTimeline(ctxB.device(), 0, /*exportable=*/false, semB);

   VkImportSemaphoreFdInfoKHR importInfo{};
   importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
   importInfo.semaphore = semBHandle;
   importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
   importInfo.flags = 0; /* permanent */
   importInfo.fd = fd.get();

   const VkResult importRes = vkImportSemaphoreFdKHR_B(ctxB.device(), &importInfo);
   if (importRes == VK_SUCCESS) {
      (void)fd.release(); /* Vulkan owns the fd on success */
   }
   REQUIRE(importRes == VK_SUCCESS);

   auto waitFor = [](PFN_vkWaitSemaphoresKHR fn, VkDevice dev, VkSemaphore sem,
                     uint64_t value, uint64_t timeoutNs) {
      VkSemaphoreWaitInfo wi{};
      wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
      wi.semaphoreCount = 1;
      wi.pSemaphores = &sem;
      wi.pValues = &value;
      return fn(dev, &wi, timeoutNs);
   };
   auto signalTo = [](PFN_vkSignalSemaphoreKHR fn, VkDevice dev, VkSemaphore sem,
                      uint64_t value) {
      VkSemaphoreSignalInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
      si.semaphore = sem;
      si.value = value;
      return fn(dev, &si);
   };

   /* The import shares A's payload: B starts at A's current value (0). */
   {
      uint64_t v = ~0ull;
      REQUIRE(vkGetValue_B(ctxB.device(), semBHandle, &v) == VK_SUCCESS);
      REQUIRE(v == 0);
   }

   /* ---------- CPU signal on A is observed on B ---------- */
   {
      REQUIRE(signalTo(vkSignalSemaphore_A, ctxA.device(), semAHandle, 7) == VK_SUCCESS);
      REQUIRE(waitFor(vkWaitSemaphores_B, ctxB.device(), semBHandle, 7, 5 * kSecond) == VK_SUCCESS);

      uint64_t v = 0;
      REQUIRE(vkGetValue_B(ctxB.device(), semBHandle, &v) == VK_SUCCESS);
      REQUIRE(v == 7);
   }

   /* A value that hasn't been reached must time out (not falsely signal). */
   REQUIRE(waitFor(vkWaitSemaphores_B, ctxB.device(), semBHandle, 8, kSecond / 20) == VK_TIMEOUT);

   /* ---------- GPU signal on A (empty submit) is observed on B ---------- */
   {
      uint64_t sigValue = 8;
      VkTimelineSemaphoreSubmitInfo tsi{};
      tsi.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
      tsi.signalSemaphoreValueCount = 1;
      tsi.pSignalSemaphoreValues = &sigValue;

      VkSubmitInfo submit{};
      submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit.pNext = &tsi;
      submit.signalSemaphoreCount = 1;
      submit.pSignalSemaphores = &semAHandle;

      REQUIRE(vkQueueSubmit(ctxA.queue(), 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
      REQUIRE(waitFor(vkWaitSemaphores_B, ctxB.device(), semBHandle, 8, 5 * kSecond) == VK_SUCCESS);

      uint64_t v = 0;
      REQUIRE(vkGetValue_B(ctxB.device(), semBHandle, &v) == VK_SUCCESS);
      REQUIRE(v == 8);
   }

   /* ---------- CPU signal on B is observed back on A ---------- */
   {
      REQUIRE(signalTo(vkSignalSemaphore_B, ctxB.device(), semBHandle, 20) == VK_SUCCESS);
      REQUIRE(waitFor(vkWaitSemaphores_A, ctxA.device(), semAHandle, 20, 5 * kSecond) == VK_SUCCESS);

      uint64_t v = 0;
      REQUIRE(vkGetValue_A(ctxA.device(), semAHandle, &v) == VK_SUCCESS);
      REQUIRE(v == 20);
   }

   /* Drain A's GPU submit before tearing the contexts down. */
   REQUIRE(vkDeviceWaitIdle(ctxA.device()) == VK_SUCCESS);
}
