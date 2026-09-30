#include "sq/graphics/shadow_map.hpp"

#include <stdexcept>

namespace sq::graphics {

ShadowMap::ShadowMap(VkDevice device, GpuAllocator& allocator,
                     VkRenderPass shadow_render_pass, std::uint32_t size, VkFormat depth_format)
    : allocator_(&allocator), device_(device), extent_{ size, size } {

    VkImageCreateInfo image_create_info = {};
    image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create_info.imageType = VK_IMAGE_TYPE_2D;
    image_create_info.extent = { size, size, 1 };
    image_create_info.extent.depth = 1;
    image_create_info.mipLevels = 1;
    image_create_info.arrayLayers = 1;
    image_create_info.format = depth_format;
    image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_create_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &image_create_info, nullptr, &image_) != VK_SUCCESS) {
        throw std::runtime_error("ShadowMap::ShadowMap : イメージの作成に失敗しました。");
    }

    VkMemoryRequirements memory_requirements;
    vkGetImageMemoryRequirements(device_, image_, &memory_requirements);

    allocation_ = allocator.allocate(memory_requirements,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        false);   // ★ optimal tiling なので false)
    if (vkBindImageMemory(device_, image_, allocation_.memory, allocation_.offset) != VK_SUCCESS) {
        throw std::runtime_error("ShadowMap::ShadowMap : メモリの割り当てに失敗しました。");
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
        throw std::runtime_error("ShadowMap::ShadowMap : イメージビューの作成に失敗しました。");
    }

    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = shadow_render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &view_;
    framebuffer_info.width = size;
    framebuffer_info.height = size;
    framebuffer_info.layers = 1;

    if (vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer_) != VK_SUCCESS) {
        throw std::runtime_error("ShadowMap::ShadowMap : フレームバッファの作成に失敗しました！");
    }
    // ★ イメージのレイアウト遷移はここでは書かない。
    //   シャドウレンダーパスの initialLayout = UNDEFINED / finalLayout = DEPTH_STENCIL_READ_ONLY
    //   が毎フレーム面倒を見る（⓪-1）。t;
}

ShadowMap::~ShadowMap() {
    // framebuffer -> view -> image -> allocator_->free(allocation_) の順で破棄する。
    vkDestroyFramebuffer(device_, framebuffer_, nullptr);
    vkDestroyImageView(device_, view_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
    if (allocator_ != nullptr) {
        allocator_->free(allocation_);
    }

    allocator_ = nullptr;
    framebuffer_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
}

VkImageView ShadowMap::view() const {
    return view_;
}

VkFramebuffer ShadowMap::framebuffer() const {
    return framebuffer_;
}

VkExtent2D ShadowMap::extent() const {
    return extent_;
}

}  // namespace sq::graphics
