#pragma once

#include <vulkan/vulkan.h>

#include "sq/graphics/physical_device.hpp"

namespace sq::graphics {

// このエンジンが使用する論理デバイス（VkDevice）およびキューを所有しています。
class Device {
public:
    Device(VkPhysicalDevice physical_device, const QueueFamilyIndices& indices,
           bool enable_validation);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] VkDevice handle() const;
    [[nodiscard]] VkPhysicalDevice physical_device() const;
    [[nodiscard]] VkQueue graphics_queue() const;
    [[nodiscard]] VkQueue present_queue() const;

private:
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
};

}  // namespace sq::graphics
