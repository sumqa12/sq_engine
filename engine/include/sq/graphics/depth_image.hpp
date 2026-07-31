#pragma once

#include <vulkan/vulkan.h>

#include "gpu_allocator.hpp"

namespace sq::graphics {

class DepthImage {
    public:
        DepthImage(VkPhysicalDevice physical_device, VkDevice device, GpuAllocator& allocator,
                   VkExtent2D extent, VkFormat depth_format);
        ~DepthImage();  // view → image → memory の順で破棄

        // コピー禁止

        [[nodiscard]] VkImageView view() const;
        [[nodiscard]] VkFormat format() const;

    private:
        GpuAllocator* allocator_ = nullptr; // 破棄時に free するため保持 (所有しない)
        Allocation allocation_{};
        VkDevice device_ = VK_NULL_HANDLE;
        VkImage image_ = VK_NULL_HANDLE;
        VkImageView view_ = VK_NULL_HANDLE;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
};

}