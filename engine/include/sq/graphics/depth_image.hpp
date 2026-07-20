#pragma once

#include <vulkan/vulkan.h>

#include "sq/graphics/physical_device.hpp"

namespace sq::graphics {

class DepthImage {
    public:
        DepthImage(VkPhysicalDevice physical_device, VkDevice device,
                   VkExtent2D extent, VkFormat depth_format);
        ~DepthImage();  // view → image → memory の順で破棄

        // コピー禁止

        [[nodiscard]] VkImageView view() const;
        [[nodiscard]] VkFormat format() const;

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkImage image_ = VK_NULL_HANDLE;
        VkDeviceMemory memory_ = VK_NULL_HANDLE;
        VkImageView view_ = VK_NULL_HANDLE;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
};

}