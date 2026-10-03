#include "sq/graphics/storage_texture.hpp"

#include <stdexcept>

namespace sq::graphics {

StorageTexture::StorageTexture(VkDevice device, GpuAllocator& allocator,
                               std::uint32_t width, std::uint32_t height, VkFormat format)
    : allocator_(&allocator), device_(device), width_(width), height_(height) {
    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = 0, // ★ CUBE_COMPATIBLE は付けない
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (vkCreateImage(device_, &image_create_info, nullptr, &image_) != VK_SUCCESS) {
        throw std::runtime_error("StorageTexture::StorageTexture : イメージの作成に失敗しました。");
    };

    VkMemoryRequirements memory_requirements;
    vkGetImageMemoryRequirements(device_, image_, &memory_requirements);

    allocation_ = allocator.allocate(memory_requirements,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        false);
    if (vkBindImageMemory(device_, image_, allocation_.memory, allocation_.offset) != VK_SUCCESS) {
        throw std::runtime_error("StorageTexture::StorageTexture : メモリの割り当てに失敗しました。");
    }

    VkImageViewCreateInfo sample_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image_,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_create_info.format,
        .subresourceRange ={
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = image_create_info.mipLevels,
            .layerCount = image_create_info.arrayLayers
        }
    };

    if (int result = vkCreateImageView(device_, &sample_view_create_info, nullptr, &view_); result != VK_SUCCESS) {
        throw std::runtime_error("StorageTexture::StorageTexture : サンプルビューの作成に失敗しました。");
    }
}

StorageTexture::~StorageTexture() {
    vkDestroyImageView(device_, view_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
    if (allocator_ != nullptr) {
        allocator_->free(allocation_);
    }

    device_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    allocator_ = nullptr;
}

VkImageView StorageTexture::view() const {
    return view_;
}

VkImage StorageTexture::handle() const {
    return image_;
}

std::uint32_t StorageTexture::width() const {
    return width_;
}

std::uint32_t StorageTexture::height() const {
    return height_;
}

}  // namespace sq::graphics
