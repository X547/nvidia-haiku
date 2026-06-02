/*
 * SPDX-License-Identifier: MIT
 *
 * Catch2 test fixture: creates an instance, picks a physical device, creates
 * a logical device. Designed for reuse by additional test files — just
 * construct a TestFixture inside a TEST_CASE with the required extensions.
 */
#pragma once

#include "VulkanRaii.h"

#include <cstdint>
#include <vector>


namespace vkr {

struct DeviceInit {
   uint32_t apiVersion = VK_API_VERSION_1_1;
   std::vector<const char*> instanceExtensions;
   std::vector<const char*> deviceExtensions;
   /* Optional chain appended to VkDeviceCreateInfo::pNext, e.g. a
    * VkPhysicalDevice*Features struct enabling an extension feature. Must
    * outlive the TestFixture construction. */
   const void* deviceNext = nullptr;
};


class TestFixture {
public:
   explicit TestFixture(const DeviceInit& init = {});

   TestFixture(const TestFixture&) = delete;
   TestFixture& operator=(const TestFixture&) = delete;

   VkInstance instance() const { return instance_; }
   VkPhysicalDevice physicalDevice() const { return physDev_; }
   VkDevice device() const { return device_; }
   uint32_t queueFamilyIndex() const { return queueFamilyIndex_; }
   VkQueue queue() const { return queue_; }
   const VkPhysicalDeviceMemoryProperties& memProps() const { return memProps_; }

   /* First memory type satisfying flags within the typeBits mask, or
    * UINT32_MAX. */
   uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const;

   /* vkGetDeviceProcAddr wrapper. */
   template <typename Fn>
   Fn loadDeviceFn(const char* name) const {
      return reinterpret_cast<Fn>(vkGetDeviceProcAddr(device_, name));
   }

private:
   Instance instance_;
   VkPhysicalDevice physDev_ = VK_NULL_HANDLE;
   Device device_;
   uint32_t queueFamilyIndex_ = 0;
   VkQueue queue_ = VK_NULL_HANDLE;
   VkPhysicalDeviceMemoryProperties memProps_{};
};

} // namespace vkr
