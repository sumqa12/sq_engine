#include "sq/graphics/shadow_map.hpp"

#include <format>
#include <stdexcept>
#include <string>

#include "sq/graphics/image_utils.hpp"            // 初期遷移（phase17 ⓪-5）
#include "sq/graphics/single_time_commands.hpp"

namespace sq::graphics {

ShadowMap::ShadowMap(VkDevice device, GpuAllocator& allocator,
                     std::uint32_t queue_family_index, VkQueue queue,
                     VkRenderPass shadow_render_pass, VkFormat depth_format,
                     const ShadowMapConfig& config)
    : allocator_(&allocator), device_(device), extent_{ config.size, config.size }, layers_(config.layers) {
    VkImageCreateInfo image_create_info = {};
    image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create_info.imageType = VK_IMAGE_TYPE_2D;
    image_create_info.extent = { config.size, config.size, 1 };
    image_create_info.extent.depth = 1;
    image_create_info.mipLevels = 1;
    image_create_info.arrayLayers = config.layers;
    if (config.sample_view_type == VK_IMAGE_VIEW_TYPE_CUBE
        || config.sample_view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
        image_create_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
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

    //   サンプル用ビュー
    VkImageViewCreateInfo sample_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image_,
        .viewType = config.sample_view_type,
        .format = image_create_info.format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel = 0,
            .levelCount = image_create_info.mipLevels,
            .baseArrayLayer = 0,
            .layerCount = config.layers,
        },
    };
    if (vkCreateImageView(device_, &sample_view_create_info, nullptr, &sample_view_) != VK_SUCCESS) {
        throw std::runtime_error("ShadowMap::ShadowMap : サンプルビューの作成に失敗しました。");
    }

    //   層ごとのビュー layer_views_（config.layers 本）
    //     ★ CUBE_ARRAY のイメージでも、描画用は 2D でよい（1層 = 1面ずつ描くため）。
    layer_views_.resize(config.layers);
    for (std::uint32_t i = 0; i < config.layers; i++) {
        VkImageViewCreateInfo layer_view_create_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image_,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = image_create_info.format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel = 0,
                .levelCount = image_create_info.mipLevels,
                .baseArrayLayer = i,
                .layerCount = 1,
            },
        };
        if (vkCreateImageView(device_, &layer_view_create_info, nullptr, &layer_views_[i]) != VK_SUCCESS) {
            throw std::runtime_error(std::format("ShadowMap::ShadowMap : レイヤービュー{}層の作成に失敗しました。", i));
        }
    }

    //   3. 層ごとのフレームバッファ
    //     ★ framebuffer_info.layers は「この FB が何層に描くか」で、イメージの層数ではない。1 のまま。
    framebuffers_.resize(config.layers);
    for (std::uint32_t i = 0; i < config.layers; i++) {
        VkFramebufferCreateInfo framebuffer_create_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = shadow_render_pass,
            .attachmentCount = 1,
            .pAttachments = &layer_views_[i],
            .width = config.size,
            .height = config.size,
            .layers = 1,
        };
        if (vkCreateFramebuffer(device_, &framebuffer_create_info, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            throw std::runtime_error(std::format("ShadowMap::ShadowMap : フレームバッファ{}層の作成に失敗しました。", i));
        }
    }

    // 全層を DEPTH_STENCIL_READ_ONLY_OPTIMAL へ初期遷移する。
    const bool has_stencil = depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT
                      || depth_format == VK_FORMAT_D24_UNORM_S8_UINT
                      || depth_format == VK_FORMAT_D16_UNORM_S8_UINT;
    const VkImageAspectFlags aspect = has_stencil
        ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
        : VK_IMAGE_ASPECT_DEPTH_BIT;

    SingleTimeCommands cmd(device_, queue_family_index, queue);

    transition_image_layout(cmd.handle(), image_,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        0, 1, 0, config.layers, aspect);

    cmd.submit_and_wait();
    //   aspect（遷移のバリアの aspectMask）:
    //     ステンシル付きフォーマット（D24_UNORM_S8_UINT / D32_SFLOAT_S8_UINT / D16_UNORM_S8_UINT）なら
    //     DEPTH | STENCIL、そうでなければ DEPTH。
    //   ★ ビューは DEPTH だけ、バリアは DEPTH | STENCIL、と**逆になる**のに注意。
    //     深度とステンシルの両方を持つイメージのレイアウトは「両方まとめて」しか遷移できない
    //     （separateDepthStencilLayouts を有効にしていない限り）。DEPTH だけで遷移すると
    //     バリデーションが「aspectMask にステンシルが足りない」と言う。
    //   ★ この遷移の後の内容は未定義（クリアしていない）。読むのは本パスだけで、本パスが読むのは
    //     「そのフレームでシャドウパスが描いた層」だけなので問題ない。
    //   ★ 以前ここにあった「レイアウト遷移はここでは書かない（レンダーパスが毎フレーム面倒を見る）」は、
    //     「全層を毎フレーム描く」前提でだけ正しかった（③ で崩れる）。
}

ShadowMap::~ShadowMap() {
    //   ★ フレームバッファはビューを参照しているので、ビューより先に壊すこと。
    for (auto& framebuffer : framebuffers_) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (auto& layer_view : layer_views_) {
        vkDestroyImageView(device_, layer_view, nullptr);
    }
    vkDestroyImageView(device_, sample_view_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
    if (allocator_ != nullptr) {
        allocator_->free(allocation_);
    }

    allocator_ = nullptr;
    device_ = VK_NULL_HANDLE;
    sample_view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
}

VkImageView ShadowMap::sample_view() const {
    return sample_view_;
}

VkFramebuffer ShadowMap::framebuffer(std::uint32_t layer) const {
    return framebuffers_.at(layer);
}

VkExtent2D ShadowMap::extent() const {
    return extent_;
}

std::uint32_t ShadowMap::layers() const {
    return layers_;
}

}  // namespace sq::graphics
