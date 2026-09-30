#include "sq/graphics/cubemap.hpp"

#include <stdexcept>
#include <spdlog/spdlog.h>

namespace sq::graphics {

Cubemap::Cubemap(VkDevice device, GpuAllocator& allocator,
                 std::uint32_t size, std::uint32_t mips, VkFormat format)
    : allocator_(&allocator), device_(device), size_(size), mips_(mips) {
    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {
            .width = size,
            .height = size,
            .depth = 1
        },
        .mipLevels = mips,
        .arrayLayers = 6,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (vkCreateImage(device_, &image_create_info, nullptr, &image_) != VK_SUCCESS) {
        throw std::runtime_error("Cubemap::Cubemap : イメージの作成に失敗しました。");
    };

    VkMemoryRequirements memory_requirements;
    vkGetImageMemoryRequirements(device_, image_, &memory_requirements);

    allocation_ = allocator.allocate(memory_requirements,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        false);
    if (vkBindImageMemory(device_, image_, allocation_.memory, allocation_.offset) != VK_SUCCESS) {
        throw std::runtime_error("CubeMap::CubeMap : メモリの割り当てに失敗しました。");
    }

    VkImageViewCreateInfo sample_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image_,
        .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
        .format = image_create_info.format,
        .subresourceRange ={
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = image_create_info.mipLevels,
            .layerCount = image_create_info.arrayLayers
        }
    };

    if (int result = vkCreateImageView(device_, &sample_view_create_info, nullptr, &sample_view_); result != VK_SUCCESS) {
        throw std::runtime_error("CubeMap::CubeMap : サンプルビューの作成に失敗しました。");
    }

    storage_views_.resize(mips);
    for (std::size_t mip = 0; mip < mips; ++mip) {
        VkImageViewCreateInfo storage_view_create_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image_,
            .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
            .format = image_create_info.format,
            .subresourceRange ={
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = static_cast<std::uint32_t>(mip),
                .levelCount = 1,
                .layerCount = image_create_info.arrayLayers
            }
        };
        if (int result = vkCreateImageView(device_, &storage_view_create_info, nullptr, &storage_views_[mip]); result != VK_SUCCESS) {
            throw std::runtime_error("CubeMap::CubeMap : ストレージビューの作成に失敗しました。");
        }
    }
}

Cubemap::~Cubemap() {
    for (std::size_t mip = 0; mip < mips_; mip++) {
        vkDestroyImageView(device_, storage_views_[mip], nullptr);
    }
    vkDestroyImageView(device_, sample_view_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
    if (allocator_ != nullptr) {
        allocator_->free(allocation_);
    }

    device_ = VK_NULL_HANDLE;
    storage_views_.clear();
    sample_view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    allocator_ = nullptr;
}

VkImageView Cubemap::sample_view() const {
    return sample_view_;
}

VkImageView Cubemap::storage_view(std::uint32_t mip) const {
    return storage_views_.at(mip);
}

VkImage Cubemap::handle() const {
    return image_;
}

std::uint32_t Cubemap::size() const {
    return size_;
}

std::uint32_t Cubemap::mips() const {
    return mips_;
}

}  // namespace sq::graphics
