#include "sq/graphics/depth_image.hpp"
#include "sq/graphics/buffer.hpp"

#include <stdexcept>

namespace sq::graphics {
    DepthImage::DepthImage(VkPhysicalDevice physical_device, VkDevice device, GpuAllocator& allocator,
               VkExtent2D extent, VkFormat depth_format)
        : allocator_(&allocator), device_(device), format_(depth_format) {

        VkImageCreateInfo image_create_info = {};
        image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create_info.imageType = VK_IMAGE_TYPE_2D;
        image_create_info.extent.width = extent.width;
        image_create_info.extent.height = extent.height;
        image_create_info.extent.depth = 1;
        image_create_info.mipLevels = 1;
        image_create_info.arrayLayers = 1;
        image_create_info.format = depth_format;
        image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_create_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;


        vkCreateImage(device_, &image_create_info, nullptr, &image_);

        VkMemoryRequirements memory_requirements;
        vkGetImageMemoryRequirements(device_, image_, &memory_requirements);

        // plan13
        allocation_ = allocator.allocate(memory_requirements,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            /*linear=*/false);   // ★ optimal tiling なので false)
        if (VkResult result = vkBindImageMemory(device_, image_, allocation_.memory, allocation_.offset)
            ; result != VK_SUCCESS) {
            throw std::runtime_error("DepthImage::DepthImage : メモリの割り当てに失敗しました。");
        }

        VkImageViewCreateInfo image_view_create_info = {};
        image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        image_view_create_info.subresourceRange.levelCount = image_create_info.mipLevels;
        image_view_create_info.subresourceRange.layerCount = image_create_info.arrayLayers;
        image_view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        image_view_create_info.format = image_create_info.format;
        image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        image_view_create_info.image = image_;
        if (int result = vkCreateImageView(device_, &image_view_create_info, nullptr, &view_); result != VK_SUCCESS) {
            throw std::runtime_error("深度バッファ用のイメージビュー作成に失敗しました。");
        }
    }

    DepthImage::~DepthImage() {
        vkDeviceWaitIdle(device_);

        vkDestroyImageView(device_, view_, nullptr);
        vkDestroyImage(device_, image_, nullptr);
        allocator_->free(allocation_);

        allocator_ = nullptr;
        device_ = VK_NULL_HANDLE;
        view_ = VK_NULL_HANDLE;
        image_ = VK_NULL_HANDLE;
    }

    VkImageView DepthImage::view() const {
        return view_;
    }

    VkFormat DepthImage::format() const {
        return format_;
    }
}
