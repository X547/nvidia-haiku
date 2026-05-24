/*
 * SPDX-License-Identifier: MIT
 */
#include "VulkanFixture.h"

#include <sstream>
#include <stdexcept>
#include <string>


namespace vkr {

namespace {

inline void check(VkResult res, const char* expr) {
   if (res != VK_SUCCESS) {
      std::ostringstream oss;
      oss << expr << " failed: VkResult=" << static_cast<int>(res);
      throw std::runtime_error(oss.str());
   }
}

#define VK_CHECK(expr) ::vkr::check((expr), #expr)

VkInstance createInstance(uint32_t apiVersion,
                          const std::vector<const char*>& exts)
{
   VkApplicationInfo app{};
   app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   app.pApplicationName = "VulkanTests";
   app.applicationVersion = 1;
   app.pEngineName = "VulkanTests";
   app.engineVersion = 1;
   app.apiVersion = apiVersion;

   VkInstanceCreateInfo ci{};
   ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   ci.pApplicationInfo = &app;
   ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
   ci.ppEnabledExtensionNames = exts.empty() ? nullptr : exts.data();

   VkInstance h = VK_NULL_HANDLE;
   VK_CHECK(vkCreateInstance(&ci, nullptr, &h));
   return h;
}

VkPhysicalDevice pickPhysicalDevice(VkInstance inst)
{
   uint32_t count = 0;
   VK_CHECK(vkEnumeratePhysicalDevices(inst, &count, nullptr));
   if (count == 0)
      throw std::runtime_error("no Vulkan physical devices");

   std::vector<VkPhysicalDevice> devs(count);
   VK_CHECK(vkEnumeratePhysicalDevices(inst, &count, devs.data()));

   /* Prefer a discrete GPU; fall back to the first device. */
   for (auto d : devs) {
      VkPhysicalDeviceProperties props{};
      vkGetPhysicalDeviceProperties(d, &props);
      if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
         return d;
   }
   return devs[0];
}

uint32_t pickQueueFamily(VkPhysicalDevice pdev)
{
   uint32_t count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &count, nullptr);
   std::vector<VkQueueFamilyProperties> props(count);
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &count, props.data());

   for (uint32_t i = 0; i < count; i++) {
      if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
         return i;
   }
   throw std::runtime_error("no graphics-capable queue family");
}

VkDevice createDevice(VkPhysicalDevice pdev, uint32_t qfi,
                      const std::vector<const char*>& exts)
{
   const float prio = 1.0f;

   VkDeviceQueueCreateInfo qci{};
   qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
   qci.queueFamilyIndex = qfi;
   qci.queueCount = 1;
   qci.pQueuePriorities = &prio;

   VkDeviceCreateInfo ci{};
   ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
   ci.queueCreateInfoCount = 1;
   ci.pQueueCreateInfos = &qci;
   ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
   ci.ppEnabledExtensionNames = exts.empty() ? nullptr : exts.data();

   VkDevice d = VK_NULL_HANDLE;
   VK_CHECK(vkCreateDevice(pdev, &ci, nullptr, &d));
   return d;
}

} // namespace


TestFixture::TestFixture(const DeviceInit& init)
{
   instance_ = Instance(createInstance(init.apiVersion, init.instanceExtensions));
   physDev_ = pickPhysicalDevice(instance_.get());
   queueFamilyIndex_ = pickQueueFamily(physDev_);
   device_ = Device(createDevice(physDev_, queueFamilyIndex_, init.deviceExtensions));
   vkGetDeviceQueue(device_.get(), queueFamilyIndex_, 0, &queue_);
   vkGetPhysicalDeviceMemoryProperties(physDev_, &memProps_);
}

uint32_t TestFixture::findMemoryType(uint32_t typeBits,
                                     VkMemoryPropertyFlags flags) const
{
   for (uint32_t i = 0; i < memProps_.memoryTypeCount; i++) {
      if (!(typeBits & (1u << i)))
         continue;
      if ((memProps_.memoryTypes[i].propertyFlags & flags) == flags)
         return i;
   }
   return UINT32_MAX;
}

} // namespace vkr
