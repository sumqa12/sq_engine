#include "sq/graphics/hdr_texture.hpp"

#include <stdexcept>
#include <chrono>

// ★ STB_IMAGE_IMPLEMENTATION はここでは定義しない（texture.cpp で1箇所だけ定義済み）。
#include <stb_image.h>
#include <spdlog/spdlog.h>

#include "sq/graphics/buffer.hpp"
#include "sq/graphics/image_utils.hpp"
#include "sq/graphics/single_time_commands.hpp"

namespace sq::graphics {
using namespace std::chrono;
HdrTexture::HdrTexture(VkDevice device, GpuAllocator& allocator,
                       std::uint32_t graphics_queue_family, VkQueue graphics_queue,
                       const std::string& path)
    : allocator_(&allocator), device_(device) {

    //   1. 画像の読み込み
    int width, height, channels;
    const auto start = steady_clock::now();
    float* pixels = stbi_loadf(path.c_str(), &width, &height, &channels, 4 /* RGBA */);
    const auto end = steady_clock::now();
    spdlog::info("HdrTexture::HdrTexture : 画像の読み込み(デコード) [{}ms]", duration_cast<milliseconds>(end - start).count());

    float gray[] = {0.5f, 0.5f, 0.5f, 1.0f}; // 1x1の灰色
    if (pixels == nullptr) {
        spdlog::warn("HdrTexture::HdrTexture : 画像の読み込みに失敗しました。");
        pixels = gray; width = 1; height = 1;
    } else if (width <= 0 || height <= 0) {
        spdlog::warn("HdrTexture::HdrTexture : 画像のサイズが不正です。");
        pixels = gray; width = 1; height = 1;
    }

    width_ = width;
    height_ = height;

    //   2. StagingBuffer staging(*allocator_, device_, pixels, image_size);
    VkDeviceSize image_size = width * height * 4 * sizeof(float); // sizeof(float) を忘れると下 3/4 が黒くなる
    StagingBuffer staging_buffer(*allocator_, device_, pixels, image_size);  // phase11 ③: アロケータ経由

    if (pixels != gray) {
        stbi_image_free(pixels);
    }

    //   3. VkImageCreateInfo → allocate（DEVICE_LOCAL, linear = false）→ vkBindImageMemory
    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R32G32B32A32_SFLOAT,
        .extent = {
            .width = static_cast<std::uint32_t>(width),
            .height = static_cast<std::uint32_t>(height),
            .depth = 1
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    if (vkCreateImage(device_, &image_create_info, nullptr, &image_) != VK_SUCCESS) {
        throw std::runtime_error("HdrTexture::HdrTexture : イメージの作成に失敗しました。");
    };

    VkMemoryRequirements memory_requirements;
    vkGetImageMemoryRequirements(device_, image_, &memory_requirements);

    allocation_ = allocator.allocate(memory_requirements,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        false);

    if (vkBindImageMemory(device_, image_, allocation_.memory, allocation_.offset) != VK_SUCCESS) {
        throw std::runtime_error("HdrTexture::HdrTexture : メモリの割り当てに失敗しました。");
    }

    //   4. 一時プール生成〜プール破棄 -> イメージのレイアウト遷移
    //   SingleTimeCommands cmd(device_, graphics_queue_family, graphics_queue);
    //        transition_image_layout(UNDEFINED -> TRANSFER_DST, 0, 1)
    //        vkCmdCopyBufferToImage
    //        transition_image_layout(TRANSFER_DST -> SHADER_READ_ONLY, 0, 1)
    //      cmd.submit_and_wait();
    //      ★ staging はこの submit_and_wait より**後**まで生かしておくこと（スコープに注意）。
    SingleTimeCommands cmd(device_, graphics_queue_family, graphics_queue);
    transition_image_layout(cmd.handle(), image_, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = width;
    region.imageExtent.height = height;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd.handle(), staging_buffer.handle(), image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transition_image_layout(cmd.handle(), image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    cmd.submit_and_wait();


    //   5. view_: 2D, COLOR, levelCount = 1
    VkImageViewCreateInfo view_create_info = {};
    view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_create_info.image = image_;
    view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_create_info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_create_info.subresourceRange.levelCount = 1;
    view_create_info.subresourceRange.layerCount = 1;
    view_create_info.subresourceRange.baseMipLevel = 0;
    view_create_info.subresourceRange.baseArrayLayer = 0;
    if (vkCreateImageView(device_, &view_create_info, nullptr, &view_) != VK_SUCCESS) {
        throw std::runtime_error("HdrTexture::HdrTexture : イメージビューの作成に失敗しました。");
    }
    // ★ R32G32B32A32_SFLOAT のリニアフィルタは optimalTilingFeatures の
    //   SAMPLED_IMAGE_FILTER_LINEAR_BIT が必要。デスクトップ GPU ではほぼ対応しているが、
    //   非対応ならサンプラを NEAREST にするか R16G16B16A16_SFLOAT へ変換してから上げる。
    //   気になるなら vkGetPhysicalDeviceFormatProperties で確認して warn を出す程度でよい。
}

HdrTexture::~HdrTexture() {
    vkDestroyImageView(device_, view_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
    if (allocator_ != nullptr) {
        allocator_->free(allocation_);
    }
    allocator_ = nullptr;
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

VkImageView HdrTexture::view() const {
    return view_;
}

std::uint32_t HdrTexture::width() const {
    return width_;
}

std::uint32_t HdrTexture::height() const {
    return height_;
}

}  // namespace sq::graphics
