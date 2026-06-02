/*
 * SPDX-License-Identifier: MIT
 *
 * Small RAII wrappers around Vulkan handles for tests. Intentionally
 * hand-rolled (no Vulkan-Hpp dependency) and minimal: own the handle,
 * destroy on scope exit, support move, forbid copy.
 */
#pragma once

#include <vulkan/vulkan.h>

#include <unistd.h>

#include <utility>


namespace vkr {

class Instance {
   VkInstance h_ = VK_NULL_HANDLE;
public:
   Instance() = default;
   explicit Instance(VkInstance h) : h_(h) {}
   ~Instance() { reset(); }

   Instance(Instance&& o) noexcept
      : h_(std::exchange(o.h_, VK_NULL_HANDLE)) {}
   Instance& operator=(Instance&& o) noexcept {
      if (this != &o) { reset(); h_ = std::exchange(o.h_, VK_NULL_HANDLE); }
      return *this;
   }
   Instance(const Instance&) = delete;
   Instance& operator=(const Instance&) = delete;

   void reset() {
      if (h_ != VK_NULL_HANDLE) { vkDestroyInstance(h_, nullptr); h_ = VK_NULL_HANDLE; }
   }
   VkInstance get() const { return h_; }
   operator VkInstance() const { return h_; }
};


class Device {
   VkDevice h_ = VK_NULL_HANDLE;
public:
   Device() = default;
   explicit Device(VkDevice h) : h_(h) {}
   ~Device() { reset(); }

   Device(Device&& o) noexcept
      : h_(std::exchange(o.h_, VK_NULL_HANDLE)) {}
   Device& operator=(Device&& o) noexcept {
      if (this != &o) { reset(); h_ = std::exchange(o.h_, VK_NULL_HANDLE); }
      return *this;
   }
   Device(const Device&) = delete;
   Device& operator=(const Device&) = delete;

   void reset() {
      if (h_ != VK_NULL_HANDLE) { vkDestroyDevice(h_, nullptr); h_ = VK_NULL_HANDLE; }
   }
   VkDevice get() const { return h_; }
   operator VkDevice() const { return h_; }
};


/* Generic owner for any handle destroyed with vkDestroyXxx(device, h, ac). */
template <typename Handle,
          void (*Destroy)(VkDevice, Handle, const VkAllocationCallbacks*)>
class DeviceChild {
   VkDevice owner_ = VK_NULL_HANDLE;
   Handle h_ = VK_NULL_HANDLE;
public:
   DeviceChild() = default;
   DeviceChild(VkDevice owner, Handle h) : owner_(owner), h_(h) {}
   ~DeviceChild() { reset(); }

   DeviceChild(DeviceChild&& o) noexcept
      : owner_(o.owner_), h_(std::exchange(o.h_, VK_NULL_HANDLE)) {}
   DeviceChild& operator=(DeviceChild&& o) noexcept {
      if (this != &o) {
         reset();
         owner_ = o.owner_;
         h_ = std::exchange(o.h_, VK_NULL_HANDLE);
      }
      return *this;
   }
   DeviceChild(const DeviceChild&) = delete;
   DeviceChild& operator=(const DeviceChild&) = delete;

   void reset() {
      if (h_ != VK_NULL_HANDLE) { Destroy(owner_, h_, nullptr); h_ = VK_NULL_HANDLE; }
   }
   Handle get() const { return h_; }
   operator Handle() const { return h_; }
   VkDevice device() const { return owner_; }
};

using DeviceMemory = DeviceChild<VkDeviceMemory, vkFreeMemory>;
using Buffer       = DeviceChild<VkBuffer, vkDestroyBuffer>;
using Semaphore    = DeviceChild<VkSemaphore, vkDestroySemaphore>;


/* RAII wrapper for a mapped device-memory pointer (calls vkUnmapMemory). */
class MappedMemory {
   VkDevice owner_ = VK_NULL_HANDLE;
   VkDeviceMemory mem_ = VK_NULL_HANDLE;
   void* ptr_ = nullptr;
public:
   MappedMemory() = default;
   MappedMemory(VkDevice d, VkDeviceMemory m, void* p)
      : owner_(d), mem_(m), ptr_(p) {}
   ~MappedMemory() { reset(); }

   MappedMemory(MappedMemory&& o) noexcept
      : owner_(o.owner_), mem_(o.mem_), ptr_(std::exchange(o.ptr_, nullptr)) {}
   MappedMemory& operator=(MappedMemory&& o) noexcept {
      if (this != &o) {
         reset();
         owner_ = o.owner_; mem_ = o.mem_;
         ptr_ = std::exchange(o.ptr_, nullptr);
      }
      return *this;
   }
   MappedMemory(const MappedMemory&) = delete;
   MappedMemory& operator=(const MappedMemory&) = delete;

   void reset() {
      if (ptr_ != nullptr) { vkUnmapMemory(owner_, mem_); ptr_ = nullptr; }
   }
   void* get() const { return ptr_; }
};


/* POSIX fd RAII for the export-fd handle. */
class UniqueFd {
   int fd_ = -1;
public:
   UniqueFd() = default;
   explicit UniqueFd(int fd) : fd_(fd) {}
   ~UniqueFd() { reset(); }

   UniqueFd(UniqueFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
   UniqueFd& operator=(UniqueFd&& o) noexcept {
      if (this != &o) { reset(); fd_ = std::exchange(o.fd_, -1); }
      return *this;
   }
   UniqueFd(const UniqueFd&) = delete;
   UniqueFd& operator=(const UniqueFd&) = delete;

   void reset() { if (fd_ >= 0) ::close(fd_); fd_ = -1; }
   int get() const { return fd_; }
   int release() { return std::exchange(fd_, -1); }
};

} // namespace vkr
